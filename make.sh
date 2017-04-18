# !/bin/bash

export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabi-

build_skyline_linux() {

    if ! [ -f ".config" ]; then
        make imx_v7_skyline_defconfig
        [ $? != 0 ] && exit 1
        make menuconfig
        [ $? != 0 ] && exit 1
    fi

    make uImage LOADADDR=0x10008000 -j4

    [ $1 = "imx6q" ] && make imx6q-skyline.dtb
    [ $1 = "imx6dl" ] && make imx6dl-skyline.dtb
    return $?
}

case "$1" in
     "")
        build_skyline_linux imx6q
        ;;
     "build")
        build_skyline_linux $2
        ;;
    "clean")
        make clean
        ;;
    "distclean")
        make distclean
        ;;
    "menuconfig")
        build_skyline_linux
        ;;
esac
