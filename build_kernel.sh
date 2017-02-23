
#!/bin/bash

export PATH=$(pwd)/toolchain/aarch64-linux-android-4.9/bin:$PATH
export ARCH=arm64
# export SEC_BUILD_OPTION_HW_REVISION=02

mkdir out

make -C $(pwd) O=$(pwd)/out CROSS_COMPILE=aarch64-linux-android- KCFLAGS=-mno-android VARIANT_DEFCONFIG=msm8976_sec_a9xprolte_sea_defconfig msm8976_sec_defconfig SELINUX_DEFCONFIG=selinux_defconfig SELINUX_LOG_DEFCONFIG=selinux_log_defconfig 

make -j$(nproc --all) -C $(pwd) O=$(pwd)/out CROSS_COMPILE=aarch64-linux-android- KCFLAGS=-mno-android
