/*
 * SMSC/Microchip LAN9303 switch driver
 *
 * Copyright (c) 2014 Stefan Roese <sr@...x.de>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/phy.h>

/* Generate phy-addr and -reg from the input address */
#define PHY_ADDR(x)		((((x) >> 6) + 0x10) & 0x1f)
#define PHY_REG(x)		(((x) >> 1) & 0x1f)

#define LAN9303_ID		0x0050
#define LAN9303_ID_VAL		0x9303

#define BYTE_TEST		0x0064

#define SWITCH_CSR_DATA		0x01ac
#define SWITCH_CSR_CMD		0x01b0

#define SWITCH_CSR_CMD_BUSY	0x80000000
#define SWITCH_CSR_CMD_READ	0x40000000
#define SWITCH_CSR_CMD_BE_ALL	0x000f0000

#define TIMEOUT			100	/* msecs */

static u32 reg_addr;
static DEFINE_SPINLOCK(sysfs_lock);

static u16 lan9303_read(struct phy_device *phydev, int reg)
{
    return phydev->bus->read(phydev->bus, PHY_ADDR(reg), PHY_REG(reg));
}

static void lan9303_write(struct phy_device *phydev, int reg, u16 val)
{
	phydev->bus->write(phydev->bus, PHY_ADDR(reg), PHY_REG(reg), val);
}

static u32 lan9303_read32(struct phy_device *phydev, int reg)
{
	u32 val;

	mutex_lock(&phydev->bus->mdio_lock);
	val = lan9303_read(phydev, reg);
	val |= (lan9303_read(phydev, reg + 2) << 16) & 0xffff0000;
	mutex_unlock(&phydev->bus->mdio_lock);

	return val;
}

static void lan9303_write32(struct phy_device *phydev, int reg, u32 val)
{
	mutex_lock(&phydev->bus->mdio_lock);
	lan9303_write(phydev, reg, val & 0xffff);
	lan9303_write(phydev, reg + 2, (val >> 16) & 0xffff);
	mutex_unlock(&phydev->bus->mdio_lock);
}

static int lan9303_wait_idle(struct phy_device *dev)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(TIMEOUT);

	while (time_after(timeout, jiffies)) {
		if (!(lan9303_read32(dev, SWITCH_CSR_CMD) & SWITCH_CSR_CMD_BUSY))
			return 0;
	}

	pr_err("Timed out waiting for idle (reg=0x%08x)!\n",
	       lan9303_read32(dev, SWITCH_CSR_CMD));

	return -ETIMEDOUT;
}

static u32 lan9303_read_indirect(struct phy_device *dev, int reg)
{
	u32 tmp;

	lan9303_wait_idle(dev);
	lan9303_write32(dev, SWITCH_CSR_CMD,
			SWITCH_CSR_CMD_BUSY | SWITCH_CSR_CMD_READ |
			SWITCH_CSR_CMD_BE_ALL | reg);

	/* Flush the previous write by reading the BYTE_TEST register */
	tmp = lan9303_read32(dev, BYTE_TEST);
	lan9303_wait_idle(dev);

	return lan9303_read32(dev, SWITCH_CSR_DATA);
}

static void lan9303_write_indirect(struct phy_device *dev, int reg, u32 val)
{
	u32 tmp;

	lan9303_wait_idle(dev);
	lan9303_write32(dev, SWITCH_CSR_DATA, val);
	lan9303_write32(dev, SWITCH_CSR_CMD,
			SWITCH_CSR_CMD_BUSY | SWITCH_CSR_CMD_BE_ALL | reg);

	/* Flush the previous write by reading the BYTE_TEST register */
	tmp = lan9303_read32(dev, BYTE_TEST);
	lan9303_wait_idle(dev);
}

static ssize_t lan9303_reg_addr_show(struct device *d,
				     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%04x\n", reg_addr);
}

static ssize_t lan9303_reg_addr_store(struct device *d,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	sscanf(buf, "%x", &reg_addr);

	return strnlen(buf, count);
}

static DEVICE_ATTR(lan9303_reg_addr, S_IRUGO | S_IWUSR,
		   lan9303_reg_addr_show, lan9303_reg_addr_store);

static ssize_t lan9303_reg_val_show(struct device *d,
				    struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = dev_get_drvdata(d);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&sysfs_lock, flags);
	val = lan9303_read_indirect(phydev, reg_addr);
	spin_unlock_irqrestore(&sysfs_lock, flags);

	return sprintf(buf, "%08x\n", val);
}

static ssize_t lan9303_reg_val_store(struct device *d,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct phy_device *phydev = dev_get_drvdata(d);
	unsigned long flags;
	u32 val;

	sscanf(buf, "%x", &val);

	spin_lock_irqsave(&sysfs_lock, flags);
	lan9303_write_indirect(phydev, reg_addr, val);
	spin_unlock_irqrestore(&sysfs_lock, flags);

	return strnlen(buf, count);
}

static DEVICE_ATTR(lan9303_reg_val, S_IRUGO | S_IWUSR,
		   lan9303_reg_val_show, lan9303_reg_val_store);

static struct attribute *lan9303_sysfs_entries[] = {
	&dev_attr_lan9303_reg_addr.attr,
	&dev_attr_lan9303_reg_val.attr,
	NULL
};

static struct attribute_group lan9303_attribute_group = {
	.name = NULL,		/* put in device directory */
	.attrs = lan9303_sysfs_entries,
};

static int lan9303_match_phy_device(struct phy_device *phydev)
{
	/*
	 * One dummy read access needed to reliably detect the switch
	 */
	lan9303_read32(phydev, BYTE_TEST);

	if (lan9303_read(phydev, LAN9303_ID + 2) == LAN9303_ID_VAL) {
		phydev->phy_id = lan9303_read32(phydev, LAN9303_ID);
		return 1;
	}

	return 0;
}

static int lan9303_config_init(struct phy_device *phydev)
{
	int ret;

	dev_set_drvdata(&phydev->dev, phydev);
	ret = sysfs_create_group(&phydev->dev.kobj, &lan9303_attribute_group);
	if (ret) {
		dev_err(&phydev->dev, "unable to create sysfs file, err=%d\n",
			ret);
		return ret;
	}

	dev_info(&phydev->dev, "SMSC LAN9303 switch found, Chip ID:%04x, Revision:%04x\n",
		 lan9303_read(phydev, LAN9303_ID + 2),
		 lan9303_read(phydev, LAN9303_ID));

	return 0;
}

static struct phy_driver lan9303_pdriver = {
	.phy_id		= LAN9303_ID_VAL,
	.phy_id_mask	= 0xffffffff,
	.name		= "SMSC LAN9303",
	.features	= PHY_BASIC_FEATURES,
	.flags		= 0,
	.config_init	= lan9303_config_init,
	.config_aneg	= genphy_config_aneg,
	.read_status	= genphy_read_status,
	.match_phy_device = lan9303_match_phy_device,
	.driver  = { .owner = THIS_MODULE, },
};

static int __init lan9303_init(void)
{
	return phy_driver_register(&lan9303_pdriver);
}
module_init(lan9303_init);

static void __exit lan9303_exit(void)
{
	phy_driver_unregister(&lan9303_pdriver);
}
module_exit(lan9303_exit);

MODULE_DESCRIPTION("SMSC LAN9303 switch driver");
MODULE_AUTHOR("Stefan Roese <sr@...x.de>");
MODULE_LICENSE("GPL-2.0+");
