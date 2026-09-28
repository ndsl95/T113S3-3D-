#!/bin/bash
#
# 交叉编译 T113-S3 拆机板用的静态 LVGL 9.3 中文 fbdev 程序。
#
# 为什么用静态链接：板子 rootfs 里多个动态库已损坏（原厂 Qt 界面和几个
# 二进制都会空指针崩溃），所以程序不能依赖目标机上的任何库。
#
# 为什么用这个工具链：目标是 armhf（glibc 2.25，加载器
# /lib/ld-linux-armhf.so.3），而此工具链是软浮点（加载器 ld-linux.so.3）。
# 完全静态链接的软浮点程序内部自洽，内核照常执行，ABI 差异不产生影响；
# 但如果做成动态链接就绝对跑不起来。
#
# 依赖（开发机上已就绪）：
#   - WSL2 Ubuntu 22.04 + cmake + ninja
#   - 易百特 arm-buildroot-linux-gnueabi_sdk-buildroot 工具链
#   - LVGL 9.3.0 源码树
#   - lv_font_cn_22.c / lv_font_cn_32.c（由 tools/make_cn_ui.ps1 生成）
#
# 用法：./build.sh     产物在 dist/

set -e

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HOME/lvgl-printer-build}"
TOOLCHAIN="${T113_TOOLCHAIN:-$HOME/t113-hmi/toolchain/arm-buildroot-linux-gnueabi_sdk-buildroot}"
LVGL_SRC="${LVGL_SRC:-$HOME/t113-hmi/src/lvgl-9.3.0}"
DIST="$SRC_DIR/dist"

export PATH="$PATH:$TOOLCHAIN/bin"
export T113_TOOLCHAIN="$TOOLCHAIN"

echo "gcc      : $(which arm-linux-gnueabi-gcc)"
echo "toolchain: $TOOLCHAIN"
echo "lvgl src : $LVGL_SRC"
echo "work     : $WORK"

rm -rf "$WORK"
mkdir -p "$WORK"
for f in main.c evdev_touch.c CMakeLists.txt lv_conf.h lv_font_cn_22.c lv_font_cn_32.c; do
    [ -f "$SRC_DIR/$f" ] || { echo "缺少源文件: $f"; exit 1; }
    cp "$SRC_DIR/$f" "$WORK/$f"
    # Windows 端写出的文件可能带 UTF-8 BOM，去掉
    sed -i "1s/^\xEF\xBB\xBF//" "$WORK/$f"
done
ln -sfn "$LVGL_SRC" "$WORK/lvgl"

# 让 lv_conf.h 声明界面用的两个中文字库
sed -i "s|^#define LV_FONT_CUSTOM_DECLARE.*$|#define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(lv_font_cn_22) LV_FONT_DECLARE(lv_font_cn_32)|" "$WORK/lv_conf.h"
grep -n "LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE" "$WORK/lv_conf.h"

cat > "$WORK/toolchain-t113.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_ROOT "$ENV{T113_TOOLCHAIN}")
set(CMAKE_SYSROOT "${TOOLCHAIN_ROOT}/arm-buildroot-linux-gnueabi/sysroot")

set(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/arm-linux-gnueabi-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/arm-linux-gnueabi-g++")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cmake -S "$WORK" -B "$WORK/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$WORK/toolchain-t113.cmake" \
  -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -4

cmake --build "$WORK/build" --parallel "$(nproc)" 2>&1 | tail -8

mkdir -p "$DIST"
arm-linux-gnueabi-strip -o "$DIST/lvgl-demo" "$WORK/build/bin/lvgl-demo"
gzip -9 -c "$DIST/lvgl-demo" > "$DIST/lvgl-demo.gz"
sha256sum "$DIST/lvgl-demo" | awk '{print $1}' > "$DIST/lvgl-demo.sha256"

file "$DIST/lvgl-demo" | cut -c1-110
ls -l "$DIST/lvgl-demo" "$DIST/lvgl-demo.gz"
echo "sha256: $(cat "$DIST/lvgl-demo.sha256")"
