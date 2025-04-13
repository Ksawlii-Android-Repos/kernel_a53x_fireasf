#!/bin/bash
#
# Build script for FireAsf (Galaxy A53).
# Based on build script for Quicksilver, by Ghostrider.
# Copyright (C) 2020-2021 Adithya R. (original version)
# Copyright (C) 2022-2025 Flopster101 (rewrite)
# Copyright (C) 2025 Ksawlii (rebrand)

## Variables
# Toolchains
AOSP_REPO="https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+/refs/heads/master"
AOSP_ARCHIVE="https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/master"
PC_REPO="https://github.com/kdrag0n/proton-clang"
LZ_REPO="https://gitlab.com/Jprimero15/lolz_clang.git"
SL_REPO="http://ftp.twaren.net/Unix/Kernel/tools/llvm/files/"
GC_REPO="https://api.github.com/repos/greenforce-project/greenforce_clang/releases/latest"
ZC_REPO="https://raw.githubusercontent.com/ZyCromerZ/Clang/refs/heads/main/Clang-main-link.txt"
RV_REPO="https://api.github.com/repos/Rv-Project/RvClang/releases/latest"
GCC64_REPO="https://github.com/LineageOS/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-gnu-9.3"

# Other
DEFAULT_DEFCONFIG="a53x_defconfig"
KERNEL_URL="https://github.com/Ksawlii-Android-Repos/kernel_a53x_fireasf"
AK3_URL="https://github.com/Ksawlii-Android-Repos/AnyKernel3-a53x"
AK3_TEST=0
SECONDS=0
DATE="$(date '+%Y%m%d-%H%M')"
DIR_DATE="$(date +%e-%m-%Y | tr -d ' ')"
BUILD_HOST="$USER@$(hostname)"

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
GCC64_DIR="$WP/gcc64"
AC_DIR="$WP/aospclang"
PC_DIR="$WP/protonclang"
LZ_DIR="$WP/lolzclang"
SL_DIR="$WP/slimllvm"
GC_DIR="$WP/greenforceclang"
ZC_DIR="$WP/zycclang"
RV_DIR="$WP/rvclang"
AK3_DIR="$WP/AK3-A53X"
AK3_BRANCH="master"
KDIR="$(readlink -f .)"
USE_GCC_BINUTILS="0"

# Custom toolchain directory
if [[ -z "$CUST_DIR" ]]; then
    CUST_DIR="$WP/custom-toolchain"
else
    echo -e "\nINFO: Overriding custom toolchain path..."
fi

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

# Dependencies
UB_DEPLIST="lz4 brotli flex bc cpio kmod ccache zip binutils-aarch64-linux-gnu"
if grep -q "Ubuntu" /etc/os-release; then
    sudo apt update -qq
    sudo apt install $UB_DEPLIST -y
else
    echo -e "\nINFO: Your distro is not Ubuntu, skipping dependencies installation..."
    echo -e "INFO: Make sure you have these dependencies installed before proceeding: $UB_DEPLIST\n"
fi

## Customizable vars
# Kernel verison
FIRE_VER="6.0"

# Toggles
USE_CCACHE=1
DO_TAR=1
DO_ZIP=1

# Upload build log
BUILD_LOG=1

# aosp, proton, lolz, slim, greenforce, zyc, rv, custom
if [[ -z "$CLANG_TYPE" ]]; then
    CLANG_TYPE="aosp"
else
    echo -e "\nINFO: Overriding default toolchain"
fi

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
DEFCONFIG=$DEFAULT_DEFCONFIG

for arg in "$@"; do
    if [[ "$arg" == *m* ]]; then
        echo "INFO: menuconfig argument passed, kernel configuration menu will be shown"
        DO_MENUCONFIG=1
    fi
    if [[ "$arg" == *k* ]]; then
        echo "INFO: KernelSU argument passed, a KernelSU build will be made"
        DO_KSU=1
    fi
    if [[ "$arg" == *c* ]]; then
        echo "INFO: clean argument passed, output directory will be wiped"
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
    if [[ "$arg" == *o* ]]; then
        echo "INFO: oshi.at argument passed, build will be uploaded to oshi.at"
        DO_OSHI=1
    fi
    if [[ "$arg" == *r* ]]; then
        echo "INFO: config regeneration mode"
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
    DEFCONFIG="a53x-ksu_defconfig"
else
    FIRE_TYPE="Vanilla"
    FIRE_TYPE_SHORT="V"
fi

if [[ "$DO_OC" == "1" ]]; then
    FIRE_TYPE="$FIRE_TYPE+Unlocked"
    FIRE_TYPE_SHORT="$FIRE_TYPE_SHORT+U"
fi

ZIP_PATH="$KDIR/kernel_build/FireAsf/$DIR_DATE/FireAsf_$FIRE_VER-$FIRE_TYPE-$CODENAME-$DATE.zip"
TAR_PATH="$KDIR/kernel_build/FireAsf/$DIR_DATE/FireAsf_$FIRE_VER-$FIRE_TYPE-$CODENAME-$DATE.tar"

echo -e "\nINFO: Build info:
- Device: $DEVICE ($CODENAME)
- Addons: $FK_TYPE
- FireAsf version: $FK_VER
- Linux version: $LINUX_VER
- Defconfig: $DEFCONFIG
- Build date: $DATE
- Build type: $BUILD_TYPE
- Clean build: $([ "$DO_CLEAN" -eq 1 ] && echo "Yes" || echo "No")
"

get_toolchain() {
    local toolchain_type="$1"
    local toolchain_dir=""

    case "$toolchain_type" in
        aosp)
            toolchain_dir="$AC_DIR"
            USE_GCC_BINUTILS=1
            if [[ ! -d "$toolchain_dir" ]]; then
                echo -e "\nINFO: AOSP Clang not found! Cloning to $toolchain_dir..."
                CURRENT_CLANG=$(curl -s "$AOSP_REPO" | grep -oE "clang-r[0-9a-f]+" | sort -u | tail -n1)
                if ! curl -LSsO "$AOSP_ARCHIVE/$CURRENT_CLANG.tar.gz"; then
                    echo "ERROR: Cloning failed! Aborting..."
                    exit 1
                fi
                mkdir -p "$toolchain_dir" && tar -xf ./*.tar.gz -C "$toolchain_dir" && rm ./*.tar.gz
                touch "$toolchain_dir/bin/aarch64-linux-gnu-elfedit" && chmod +x "$toolchain_dir/bin/aarch64-linux-gnu-elfedit"
                touch "$toolchain_dir/bin/arm-linux-gnueabi-elfedit" && chmod +x "$toolchain_dir/bin/arm-linux-gnueabi-elfedit"
            fi
            ;;
        proton)
            toolchain_dir="$PC_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
                echo -e "\nINFO: Proton Clang not found! Cloning to $toolchain_dir..."
                if ! git clone -q --depth=1 "$PC_REPO" "$toolchain_dir"; then
                    echo "ERROR: Cloning failed! Aborting..."
                    exit 1
                fi
            fi
            ;;
        lolz)
            toolchain_dir="$LZ_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
                echo -e "\nINFO: Lolz Clang not found! Cloning to $toolchain_dir..."
                if ! git clone -q --depth=1 "$LZ_REPO" "$toolchain_dir"; then
                    echo "ERROR: Cloning failed! Aborting..."
                    exit 1
                fi
            fi
            ;;
        slim)
            toolchain_dir="$SL_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
                echo -e "\nINFO: Slim LLVM not found! Cloning to $toolchain_dir..."
                FILENAMES=$(curl -s "$SL_REPO" | grep -oP 'llvm-[\d.]+-x86_64\.tar\.xz')
                LATEST_FILE=$(echo "$FILENAMES" | sort -V | tail -n 1)
                if ! wget -q --show-progress -O "$WP/${LATEST_FILE}" "${SL_REPO}${LATEST_FILE}"; then
                    echo "ERROR: Cloning failed! Aborting..."
                    exit 1
                fi
                mkdir -p "$toolchain_dir"
                EXTRACTED_FOLDER=$(basename "$LATEST_FILE" .tar.xz)
                tar -xf "$WP/${LATEST_FILE}" -C "$toolchain_dir"
                mv "$toolchain_dir/$EXTRACTED_FOLDER"/* "$toolchain_dir"
                rmdir "$toolchain_dir/$EXTRACTED_FOLDER"
                rm "$WP/${LATEST_FILE}"
            fi
            ;;
        greenforce)
            USE_GCC_BINUTILS=1
            toolchain_dir="$GC_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
                echo -e "\nINFO: Greenforce Clang not found! Cloning to $toolchain_dir..."
                LATEST_RELEASE=$(curl -s $GC_REPO | grep "browser_download_url" | grep ".tar.gz" | cut -d '"' -f 4)
                if [[ -z "$LATEST_RELEASE" ]]; then
                    echo "ERROR: Failed to fetch the latest Greenforce Clang release! Aborting..."
                    exit 1
                fi
                if ! wget -q --show-progress -O "$WP/greenforce-clang.tar.gz" "$LATEST_RELEASE"; then
                    echo "ERROR: Download failed! Aborting..."
                    exit 1
                fi
                mkdir -p "$toolchain_dir"
                tar -xf "$WP/greenforce-clang.tar.gz" -C "$toolchain_dir"
                rm "$WP/greenforce-clang.tar.gz"
            fi
            ;;
        custom)
            toolchain_dir="$CUST_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
                echo -e "\nERROR: Custom toolchain not found! Aborting..."
                echo "INFO: Please provide a toolchain at $CUST_DIR or select a different toolchain"
                exit 1
            fi
            ;;
        zyc)
            toolchain_dir="$ZC_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
            echo -e "\nINFO: ZyC Clang not found! Cloning to $toolchain_dir..."
            fi

            # Check and cache the latest version
            ZYC_VERSION_FILE="$WP/zyc-clang-version.txt"
            LATEST_VERSION=$(curl -s "$ZC_REPO" | head -n 1)
            if [[ -z "$LATEST_VERSION" ]]; then
                echo "INFO: Failed to check ZyC Clang version"
            else
                if [[ -f "$ZYC_VERSION_FILE" ]]; then
                    CURRENT_VERSION=$(cat "$ZYC_VERSION_FILE")
                    if [[ "$CURRENT_VERSION" != "$LATEST_VERSION" ]]; then
                        echo "INFO: A new version of ZyC Clang is available: $LATEST_VERSION"
                        echo "$LATEST_VERSION" > "$ZYC_VERSION_FILE"
                    fi
                else
                    echo "$LATEST_VERSION" > "$ZYC_VERSION_FILE"
                fi
            fi

            if [[ ! -d "$toolchain_dir" ]]; then
                if [[ -f "$ZYC_VERSION_FILE" ]]; then
                    echo "$LATEST_VERSION" > "$ZYC_VERSION_FILE"
                fi
                if [[ -z "$LATEST_VERSION" ]]; then
                    echo "ERROR: Failed to fetch the latest ZyC Clang release! Aborting..."
                    exit 1
                fi
                if ! wget -q --show-progress -O "$WP/zyc-clang.tar.gz" "$LATEST_VERSION"; then
                    echo "ERROR: Download failed! Aborting..."
                    rm -f "$ZYC_VERSION_FILE"
                    exit 1
                fi
                mkdir -p "$toolchain_dir"
                if ! tar -xf "$WP/zyc-clang.tar.gz" -C "$toolchain_dir"; then
                    echo "ERROR: Extraction failed! Aborting..."
                    rm -f "$WP/zyc-clang.tar.gz" "$ZYC_VERSION_FILE"
                    exit 1
                fi
                rm "$WP/zyc-clang.tar.gz"
            fi
            ;;
        rv)
            toolchain_dir="$RV_DIR"
            if [[ ! -d "$toolchain_dir" ]]; then
            echo -e "\nINFO: RvClang not found! Fetching the latest version..."
            LATEST_RELEASE=$(curl -s "$RV_REPO" | grep "browser_download_url" | grep ".tar.gz" | cut -d '"' -f 4)
            if [[ -z "$LATEST_RELEASE" ]]; then
                echo "ERROR: Failed to fetch the latest RvClang release! Aborting..."
                exit 1
            fi
            if ! wget -q --show-progress -O "$WP/rvclang.tar.gz" "$LATEST_RELEASE"; then
                echo "ERROR: Download failed! Aborting..."
                exit 1
            fi
            mkdir -p "$toolchain_dir"
            if ! tar -xf "$WP/rvclang.tar.gz" -C "$toolchain_dir"; then
                echo "ERROR: Extraction failed! Aborting..."
                rm -f "$WP/rvclang.tar.gz"
                exit 1
            fi
            rm "$WP/rvclang.tar.gz"
            if [[ -d "$toolchain_dir/RvClang" ]]; then
                mv "$toolchain_dir/RvClang"/* "$toolchain_dir/"
                rmdir "$toolchain_dir/RvClang"
            fi
            fi
            ;;
        *)
            echo -e "\nERROR: Unknown toolchain type: $toolchain_type"
            exit 1
            ;;
    esac

    if [[ "$USE_GCC_BINUTILS" == "1" ]]; then
        if [[ ! -d "$GCC64_DIR" ]]; then
            echo "INFO: GCC64 not found! Cloning to $GCC64_DIR..."
            if ! git clone -q --depth=1 "$GCC64_REPO" "$GCC64_DIR"; then
                echo "ERROR: Cloning failed! Aborting..."
                exit 1
            fi
        fi
    fi

}

prep_toolchain() {
    local toolchain_type="$1"
    local toolchain_dir=""

    case "$toolchain_type" in
        aosp)
            toolchain_dir="$AC_DIR"
            echo "INFO: Toolchain: AOSP Clang"
            ;;
        proton)
            toolchain_dir="$PC_DIR"
            echo "INFO: Toolchain: Proton Clang"
            ;;
        lolz)
            toolchain_dir="$LZ_DIR"
            echo "INFO: Toolchain: Lolz Clang"
            ;;
        slim)
            toolchain_dir="$SL_DIR"
            echo "INFO: Toolchain: Slim LLVM Clang"
            ;;
        greenforce)
            toolchain_dir="$GC_DIR"
            echo "INFO: Toolchain: Greenforce Clang"
            ;;
        custom)
            toolchain_dir="$CUST_DIR"
            echo "INFO: Toolchain: Custom"
            ;;
        zyc)
            toolchain_dir="$ZC_DIR"
            echo "INFO: Toolchain: ZyC Clang"
            ;;
        rv)
            toolchain_dir="$RV_DIR"
            echo "INFO: Toolchain: RvClang"
            ;;
        *)
            echo "ERROR: Unknown toolchain type: $toolchain_type"
            exit 1
            ;;
    esac

    export PATH="${toolchain_dir}/bin:${PATH}"
    if [[ "$USE_GCC_BINUTILS" == "1" ]]; then
        export PATH="${GCC64_DIR}/bin:${PATH}"
    fi
    KBUILD_COMPILER_STRING=$("$toolchain_dir/bin/clang" -v 2>&1 | head -n 1 | sed 's/(https..*//' | sed 's/ version//')
    export KBUILD_COMPILER_STRING

    if [[ "$USE_GCC_BINUTILS" == "1" ]]; then
        CCARM64_PREFIX="aarch64-linux-"
    else
        CCARM64_PREFIX="aarch64-linux-gnu-"
    fi
}

## Pre-build dependencies
get_toolchain "$CLANG_TYPE"
prep_toolchain "$CLANG_TYPE"

# Build the caption properly and escape special characters
CAPTION_BUILD="Build info:
*Device*: \`${DEVICE} [${CODENAME}]\`
*Kernel Version*: \`${LINUX_VER}\`
*Compiler*: \`${KBUILD_COMPILER_STRING}\`
*Build host*: \`${BUILD_HOST}\`
*Branch*: \`$(git rev-parse --abbrev-ref HEAD)\`
*Commit*: \`$(git rev-parse --short HEAD)\`
*Build type*: \`${BUILD_TYPE}\`
"

# Final caption with MD5
CAPTION_FINAL="${CAPTION_BUILD}"

tgs() {
    local file="$1"
    curl -fsSL -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendDocument" \
        -F chat_id="${TELEGRAM_CHAT_ID}" \
        -F document=@"$file" \
        -F parse_mode="Markdown" \
        -F disable_web_page_preview="true" \
        -F caption="${CAPTION_FINAL}" &>/dev/null
}

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

    make -j"$(nproc --all)" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" "$DEFCONFIG" $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1')

    VERSION_STR="\"-FireAsf-$FIRE_VER-$FIRE_TYPE_SHORT\""

    rm -f "$OUT_KERNEL"

    if [[ "$DO_REGEN" = "1" ]]; then
        cp -f out/.config arch/arm64/configs/$DEFCONFIG
        echo "INFO: Configuration regenerated. Check the changes!"
        exit 0
    fi

    scripts/config --file "$KDIR/out/.config" --set-val LOCALVERSION "$VERSION_STR"

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

    echo -e "\nINFO: Starting compilation...\n"

    make -j"$(nproc --all)" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" dtbs $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1')
    if [[ "$USE_CCACHE" == "1" ]]; then
        make -j"$(nproc --all)" O=out CC="ccache clang" CROSS_COMPILE="$CCARM64_PREFIX" $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1')
    else
        make -j"$(nproc --all)" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1')
    fi
    make -j"$(nproc --all)" O=out CC="clang" CROSS_COMPILE="$CCARM64_PREFIX" INSTALL_MOD_STRIP="--strip-debug --keep-section=.ARM.attributes" INSTALL_MOD_PATH="$MOD_OUTDIR" modules_install $([[ "$arg" == *q* ]] && echo '> /dev/null 2>&1')
}

packing() {
    if [[ "$DO_ZIP" == "1" ]]; then
        if [[ -d "$AK3_DIR" ]]; then
            AK3_TEST=1
            echo -e "\nINFO: AK3_TEST flag set because local AnyKernel3 dir was found"
        else
            if ! git clone -q -b "$AK3_BRANCH" --depth=1 "$AK3_URL" "$AK3_DIR"; then
                echo -e "\nERROR: Failed to clone AnyKernel3!"
                exit 1
            fi
        fi
        if [ ! -d "kernel_build/FireAsf/$DIR_DATE" ]; then
            mkdir -p "kernel_build/FireAsf/$DIR_DATE" 
        fi
        echo -e "\nINFO: Building zip..."
        cd "$AK3_DIR"
        cp -f "$OUT_VENDORBOOTIMG" vendor_boot.img
        cp -f "$OUT_DTBIMAGE" dtb
        cp -f "$OUT_KERNEL" .
        zip -r9 -q "$ZIP_PATH" * -x .git .github README.md
        cd "$KDIR"
        echo -e "INFO: Done! \nINFO: Output: $ZIP_PATH\n"
        if [[ "$AK3_TEST" != "1" ]]; then
            rm -rf "$AK3_DIR"
        fi
    fi

    if [[ "$DO_TAR" == "1" ]]; then
        echo -e "\nINFO: Building tar..."
        cd "$KDIR/kernel_build"
        rm -f "$TAR_PATH"
        lz4 -c -12 -B6 --content-size "$OUT_BOOTIMG" > boot.img.lz4 2>/dev/null
        lz4 -c -12 -B6 --content-size "$OUT_VENDORBOOTIMG" > vendor_boot.img.lz4 2>/dev/null
        tar -cf "$TAR_PATH" boot.img.lz4 vendor_boot.img.lz4
        rm -f boot.img.lz4 vendor_boot.img.lz4
        cd "$KDIR"
        echo -e "INFO: Done! \nINFO: Output: $TAR_PATH\n"
    fi
}

post_build() {
    if [[ ! -f "$OUT_KERNEL" ]]; then
        echo -e "\nERROR: Kernel files not found! Compilation failed?"
        exit 1
    fi

    rm -rf "$TMPDIR"
    rm -f "$OUT_BOOTIMG" "$OUT_VENDORBOOTIMG"
    mkdir -p "$TMPDIR" "$MODULES_DIR/0.0" "$PLATFORM_RAMDISK_DIR"

    cp -rf "$IN_PLATFORM"/* "$PLATFORM_RAMDISK_DIR/"
    mkdir "$PLATFORM_RAMDISK_DIR/first_stage_ramdisk"
    cp -f "$PLATFORM_RAMDISK_DIR/fstab.s5e8825" "$PLATFORM_RAMDISK_DIR/first_stage_ramdisk/fstab.s5e8825"

    if ! find "$MOD_OUTDIR/lib/modules" -mindepth 1 -type d | read; then
        echo -e "\nERROR: Unknown error!\n"
        exit 1
    fi

    missing_modules=""
    for module in $(cat "$IN_DLKM/modules.load"); do
        i=$(find "$MOD_OUTDIR/lib/modules" -name "$module")
        if [[ -f "$i" ]]; then
            cp -f "$i" "$MODULES_DIR/0.0/$module"
        else
            missing_modules="$missing_modules $module"
        fi
    done

    if [[ -n "$missing_modules" ]]; then
        echo "ERROR: the following modules were not found: $missing_modules"
        exit 1
    fi

    depmod 0.0 -b "$DLKM_RAMDISK_DIR"
    sed -i 's/\([^ ]\+\)/\/lib\/modules\/\1/g' "$MODULES_DIR/0.0/modules.dep"
    cd "$MODULES_DIR/0.0"
    for i in $(find . -name "modules.*" -type f); do
        if [[ "$(basename "$i")" != "modules.dep" && "$(basename "$i")" != "modules.softdep" && "$(basename "$i")" != "modules.alias" ]]; then
            rm -f "$i"
        fi
    done
    cd "$KDIR"

    cp -f "$IN_DLKM/modules.load" "$MODULES_DIR/0.0/modules.load"
    mv "$MODULES_DIR/0.0"/* "$MODULES_DIR/"
    rm -rf "$MODULES_DIR/0.0"

    echo -e "\nINFO: Building dtb image..."
    if [[ "$DO_OC" == "1" ]]; then
        python "$MKDTBOIMG" create "$OUT_DTBIMAGE" --custom0=0x00000000 --custom1=0xff000000 --version=0 --page_size=2048 "$IN_DTB_OC" || exit 1
    else
        python "$MKDTBOIMG" create "$OUT_DTBIMAGE" --custom0=0x00000000 --custom1=0xff000000 --version=0 --page_size=2048 "$IN_DTB" || exit 1
    fi

    echo -e "\nINFO: Building boot image..."
    "$MKBOOTIMG" --header_version 4 \
        --kernel "$OUT_KERNEL" \
        --output "$OUT_BOOTIMG" \
        --ramdisk "$PREBUILT_RAMDISK" \
        --os_version 15.0.0 \
        --os_patch_level 2025-04 || exit 1
    echo -e "INFO: Done!"

    echo -e "\nINFO: Building vendor_boot image..."
    cd "$DLKM_RAMDISK_DIR"
    find . | cpio --quiet -o -H newc -R root:root | lz4 -9cl > ../ramdisk_dlkm.lz4
    cd ../ramdisk_platform
    find . | cpio --quiet -o -H newc -R root:root | lz4 -9cl > ../ramdisk_platform.lz4
    cd ..
    echo "buildtime_bootconfig=enable" > bootconfig

    "$MKBOOTIMG" --header_version 4 \
        --vendor_boot "$OUT_VENDORBOOTIMG" \
        --vendor_bootconfig "$(pwd)/bootconfig" \
        --dtb "$OUT_DTBIMAGE" \
        --vendor_ramdisk "$(pwd)/ramdisk_platform.lz4" \
        --ramdisk_type dlkm \
        --ramdisk_name dlkm \
        --vendor_ramdisk_fragment "$(pwd)/ramdisk_dlkm.lz4" \
        --os_version 15.0.0 \
        --os_patch_level 2025-04 || exit 1

    cd "$KDIR"
    echo -e "INFO: Done!"

    packing
}

upload() {
    cd "$KDIR"

    if [[ "$DO_TG" == "1" ]]; then
        echo -e "\nINFO: Uploading to Telegram\n"
        tgs "$TAR_PATH"
    fi

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
clean_tmp

upload
