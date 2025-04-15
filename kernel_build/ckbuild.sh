#!/bin/bash
#
# Build script for FireAsf (Galaxy A53).
# Based on build script for Quicksilver, by Ghostrider.
# Copyright (C) 2020-2021 Adithya R. (original version)
# Copyright (C) 2022-2025 Flopster101 (rewrite)
# Copyright (C) 2025 Ksawlii (rebrand)

## Variables
# Other
DEFAULT_DEFCONFIG="a53x_defconfig"
KERNEL_URL="https://github.com/Ksawlii-Android-Repos/kernel_a53x_fireasf"
AK3_URL="https://github.com/Ksawlii-Android-Repos/AnyKernel3-a53x"
AK3_TEST=0
SECONDS=0
DATE="$(date '+%Y%m%d-%H%M')"
DIR_DATE="$(date +%e-%m-%Y | tr -d ' ')"
BUILD_HOST="$USER@$(hostname)"
SCRIPTS_DIR="kernel_build/scripts"

# Workspace
if [[ -d /workspace ]]; then
    WP="/workspace"
    IS_GP=1
else
    IS_GP=0
fi

if [[ -z "$WP" ]]; then
    echo -e "\nERROR: Environment not Gitpod! Please set the WP env var...\n"
    exit 1
fi

if [[ ! -d drivers ]]; then
    echo -e "\nERROR: Please execute from top-level kernel tree\n"
    exit 1
fi

if [[ "$IS_GP" == "1" ]]; then
    export KBUILD_BUILD_USER="Ksawlii"
    export KBUILD_BUILD_HOST="buildbot"
fi

export PATH="$(pwd)/kernel_build/bin:$PATH"

# Directories
AK3_DIR="$WP/AK3-A53X"
AK3_BRANCH="master"
KDIR="$(readlink -f .)"
USE_GCC_BINUTILS="0"

## Inherited paths
OUTDIR="$KDIR/out"
MOD_OUTDIR="$KDIR/modules_out"
TMPDIR="$KDIR/kernel_build/tmp"
IN_PLATFORM="$KDIR/kernel_build/vboot_platform"
IN_DLKM="$KDIR/kernel_build/vboot_dlkm"
IN_DTB="$OUTDIR/arch/arm64/boot/dts/exynos/s5e8825.dtb"
IN_DTB_OC="$OUTDIR/arch/arm64/boot/dts/exynos/s5e8825_oc.dtb"
PLATFORM_RAMDISK_DIR="$TMPDIR/ramdisk_platform"
DLKM_RAMDISK_DIR="$TMPDIR/ramdisk_dlkm"
PREBUILT_RAMDISK="$KDIR/kernel_build/boot/ramdisk"
MODULES_DIR="$DLKM_RAMDISK_DIR/lib/modules"
OUT_KERNEL="$OUTDIR/arch/arm64/boot/Image"
OUT_BOOTIMG="$KDIR/kernel_build/zip/boot.img"
OUT_VENDORBOOTIMG="$KDIR/kernel_build/zip/vendor_boot.img"
OUT_DTBIMAGE="$TMPDIR/dtb.img"

# Tools
MKBOOTIMG="$(pwd)/kernel_build/mkbootimg/mkbootimg.py"
MKDTBOIMG="$(pwd)/kernel_build/dtb/mkdtboimg.py"

## Customizable vars
# Kernel verison
FIRE_VER="6.0"

# Toggles
USE_CCACHE=1
DO_TAR=1
DO_ZIP=1

# Upload build log
BUILD_LOG=1

## Info message
LINKER="ld.lld"
DEVICE="Galaxy A53"
CODENAME="a53x"

## Secrets
if [ -f "../chat_ci" ]; then
    TELEGRAM_CHAT_ID="$(cat ../chat_ci)"
fi
if [ -f "../bot_token" ]; then
    TELEGRAM_BOT_TOKEN="$(cat ../bot_token)"
fi

## Parse arguments
DO_KSU=0
DO_CLEAN=0
DO_MENUCONFIG=0
IS_RELEASE=0
DO_TG=0
DO_REGEN=0
DO_OC=0
DO_FLTO=0
QUIET=0
DO_PERMISSIVE=0
DEFCONFIG=$DEFAULT_DEFCONFIG

for arg in "$@"; do
    if [[ "$arg" == *m* ]]; then
        echo "INFO: Menuconfig argument passed, kernel configuration menu will be shown"
        DO_MENUCONFIG=1
    fi
    if [[ "$arg" == *k* ]]; then
        echo "INFO: KernelSU argument passed, a KernelSU build will be made"
        DO_KSU=1
    fi
    if [[ "$arg" == *c* ]]; then
        echo "INFO: Clean argument passed, output directory will be wiped"
        DO_CLEAN=1
    fi
    if [[ "$arg" == *R* ]]; then
        echo "INFO: Release argument passed, build marked as release"
        IS_RELEASE=1
    fi
    if [[ "$arg" == *t* ]]; then
        echo "INFO: Telegram argument passed, build will be uploaded to CI"
        DO_TG=1
    fi
    if [[ "$arg" == *b* ]]; then
        echo "INFO: Bashupload.com argument passed, build will be uploaded to bashupload.com"
        DO_BASHUP=1
    fi
    if [[ "$arg" == *r* ]]; then
        echo "INFO: Config regeneration mode"
        DO_REGEN=1
    fi
    if [[ "$arg" == *u* ]]; then
        echo "INFO: Unlocked variant argument passed, unlocked build will be made"
        DO_OC=1
    fi
    if [[ "$arg" == *l* ]]; then
        echo "INFO: Full-LTO argument passed"
        echo "WARNING: Full-LTO is VERY resource heavy and may take a long time to compile"
        DO_FLTO=1
    fi
    if [[ "$arg" == *q* ]]; then
        echo "INFO: Quiet argument passed"
        echo "WARNING: Only errors and warnings will be shown"
        DO_FLTO=1
    fi
    if [[ "$arg" == *p* ]]; then
        echo "INFO: Permissive argument passed"
        echo "WARNING: Selinux will be permissive"
        DO_PERMISSIVE=1
    fi
done

if [[ "$IS_RELEASE" == "1" ]]; then
    BUILD_TYPE="Release"
else
    BUILD_TYPE="Testing"
fi

## Build type
LINUX_VER=$(make kernelversion 2>/dev/null)

if [[ "$DO_KSU" == "1" ]]; then
    FIRE_TYPE="KSUNext"
    FIRE_TYPE_SHORT="KN"
else
    FIRE_TYPE="Vanilla"
    FIRE_TYPE_SHORT="V"
fi

if [[ "$DO_OC" == "1" ]]; then
    FIRE_TYPE="$FIRE_TYPE+Unlocked"
    FIRE_TYPE_SHORT="$FIRE_TYPE_SHORT+U"
fi

if [ "$DO_PERMISSIVE" = "1" ]; then
    FIRE_TYPE="$FIRE_TYPE+Permissive"
    FIRE_TYPE_SHORT="$FIRE_TYPE+P"
fi

ZIP_PATH="$KDIR/kernel_build/FireAsf/$DIR_DATE/FireAsf_$FIRE_VER-$FIRE_TYPE-$CODENAME-$DATE.zip"
TAR_PATH="$KDIR/kernel_build/FireAsf/$DIR_DATE/FireAsf_$FIRE_VER-$FIRE_TYPE-$CODENAME-$DATE.tar"

echo -e "\nINFO: Build info:
- Device: $DEVICE ($CODENAME)
- Addons: $FIRE_TYPE
- FireAsf version: $FIRE_VER
- Linux version: $LINUX_VER
- Defconfig: $DEFCONFIG
- Build date: $DATE
- Build type: $BUILD_TYPE
- Clean build: $([ "$DO_CLEAN" -eq 1 ] && echo "Yes" || echo "No")
"

# Dependencies
source "$SCRIPTS_DIR/deps.sh"

# Configure Toolchain(s)
source "$SCRIPTS_DIR/tc.sh"

# Setup other things
source "$SCRIPTS_DIR/upload.sh"
source "$SCRIPTS_DIR/post.sh"
source "$SCRIPTS_DIR/images.sh"
source "$SCRIPTS_DIR/pack.sh"

prep_build() {
    if [[ "$USE_CCACHE" == "1" ]]; then
        echo "INFO: Using ccache"
        if [[ "$IS_GP" == "1" ]]; then
            export CCACHE_DIR="$WP/.ccache"
            ccache -M 10G
        else
            echo "WARNING: Environment is not Gitpod, please make sure you setup your own ccache configuration!"
        fi
    fi

    echo -e "INFO: Compiler: $KBUILD_COMPILER_STRING\n"
}

build() {
    export PLATFORM_VERSION="15"
    export ANDROID_MAJOR_VERSION="v"
    export TARGET_SOC="s5e8825"

    export LLVM=1
    export LLVM_IAS=1
    export ARCH=arm64

    rm -rf "$MOD_OUTDIR" 2>/dev/null

    make -j"$JOBS" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" "$DEFCONFIG" $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '2>&1 | tee log.txt')

    VERSION_STR="\"-FireAsf-$FIRE_VER-$FIRE_TYPE_SHORT\""

    rm -f "$OUT_KERNEL"

    if [[ "$DO_REGEN" = "1" ]]; then
        if [[ "$DO_KSU" == "1" ]]; then
            echo "ERROR: Regenerate config(s) without DO_KSU=1"
            exit 1
        fi
        cp -f out/.config arch/arm64/configs/$DEFCONFIG
        echo "INFO: Configuration regenerated. Check the changes!"
        exit 0
    fi

    scripts/config --file "$KDIR/out/.config" --set-val LOCALVERSION "$VERSION_STR"

    if [[ "$DO_KSU" == "1" ]]; then
        # KernelSU Next
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU
        scripts/config --file "$KDIR/out/.config" --enable KSU_WITH_KPROBES
        scripts/config --file "$KDIR/out/.config" --disable KSU_DEBUG
        scripts/config --file "$KDIR/out/.config" --disable KSU_ALLOWLIST_WORKAROUND
        # susfs
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT 
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_SUS_PATH
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_SUS_MOUNT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_SUS_KSTAT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_TRY_UMOUNT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_SPOOF_UNAME
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_ENABLE_LOG
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_OPEN_REDIRECT
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_KSU_SUSFS_SUS_SU
        scripts/config --file "$KDIR/out/.config" --disable CONFIG_KSU_SUSFS_SUS_OVERLAYFS
    fi

    if [[ "$DO_OC" == "1" ]]; then
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_SOC_S5E8825_OVERCLOCK
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_SOC_S5E8825_GPU_OC
        scripts/config --file "$KDIR/out/.config" --set-val CONFIG_SOC_S5E8825_CL1_UV 0
        scripts/config --file "$KDIR/out/.config" --set-val CONFIG_SOC_S5E8825_CL0_UV 0
    fi

    if [[ "$DO_MENUCONFIG" == "1" ]]; then
        make O=out menuconfig $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '')
    fi

    if [[ "$DO_FLTO" == "1" ]]; then
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_LTO_CLANG_FULL
        scripts/config --file "$KDIR/out/.config" --disable CONFIG_LTO_CLANG_THIN
    fi

    if [[ "$DO_PERMISSIVE" == "1" ]]; then
        scripts/config --file "$KDIR/out/.config" --enable CONFIG_SECURITY_SELINUX_ALWAYS_PERMISSIVE
    fi

    echo -e "\nINFO: Starting compilation...\n"

    make -j"$JOBS" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" dtbs $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '2>&1 | tee log.txt')
    if [[ "$USE_CCACHE" == "1" ]]; then
        make -j"$JOBS" O=out CC="ccache clang" CROSS_COMPILE="$CCARM64_PREFIX" $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '2>&1 | tee log.txt')
    else
        make -j"$JOBS" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '2>&1 | tee log.txt')
    fi
    make -j"$JOBS" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" INSTALL_MOD_STRIP="--strip-debug --keep-section=.ARM.attributes" INSTALL_MOD_PATH="$MOD_OUTDIR" modules_install $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '2>&1 | tee log.txt')
}

clean() {
    make clean $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '')
    make mrproper $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1' || echo '')
}

clean_tmp() {
    echo -e "INFO: Cleaning after build..."
    rm -rf "$TMPDIR" "$MOD_OUTDIR" "$OUT_VENDORBOOTIMG" "$OUT_BOOTIMG"
}

# Do a clean build?
if [[ "$DO_CLEAN" == "1" ]]; then
    clean
fi

## Run build
prep_build
build
post_build
build_images
packing
clean_tmp

upload

# Unset variables after build
unset GCC64_DIR AC_DIR PC_DIR LZ_DIR SL_DIR GC_DIR ZC_DIR RV_DIR CUST_DIR KBUILD_COMPILER_STRING CCARM64_PREFIX
