#!/bin/bash
# ====================================================
# ARM交叉编译脚本 (100ask imx6ull)
# 使用 buildroot 工具链编译 ARMv7 版本
# ====================================================
set -e

# 交叉编译器路径
TOOLCHAIN_DIR=/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
CROSS_COMPILE=$TOOLCHAIN_DIR/bin/arm-buildroot-linux-gnueabihf-
SYSROOT=$TOOLCHAIN_DIR/arm-buildroot-linux-gnueabihf/sysroot

CXX=${CROSS_COMPILE}g++
CC=${CROSS_COMPILE}gcc

SRC_DIR="$(dirname "$0")/src"
BUILD_DIR="$(dirname "$0")/build"

echo "=== ARM 交叉编译 (100ask imx6ull) ==="
echo "工具链: $CXX"
echo "源文件: $SRC_DIR"
echo "输出目录: $BUILD_DIR"
echo "======================================"

# 检查工具链
if ! command -v $CXX &> /dev/null; then
    echo "[ERROR] 找不到交叉编译器: $CXX"
    echo "请检查工具链路径或设置 PATH:"
    echo "  export PATH=\$TOOLCHAIN_DIR/bin:\$PATH"
    exit 1
fi

$CXX --version | head -1

mkdir -p "$BUILD_DIR"

# 编译参数
CXXFLAGS="-std=c++11 -O2 -Wall -DUSE_MPC -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
LDFLAGS="-static-libstdc++ -static-libgcc -lpthread"

echo ""
echo "--- 编译 ffmpeg_sender (ARM) ---"
$CXX $CXXFLAGS "$SRC_DIR/ffmpeg_sender.cpp" -o "$BUILD_DIR/ffmpeg_sender_arm" $LDFLAGS
echo "  -> $BUILD_DIR/ffmpeg_sender_arm"

echo ""
echo "--- 编译 ffmpeg_receiver (ARM) ---"
$CXX $CXXFLAGS "$SRC_DIR/ffmpeg_receiver.cpp" -o "$BUILD_DIR/ffmpeg_receiver_arm" $LDFLAGS
echo "  -> $BUILD_DIR/ffmpeg_receiver_arm"

echo ""
echo "=== 编译完成 ==="
ls -lh "$BUILD_DIR/ffmpeg_sender_arm" "$BUILD_DIR/ffmpeg_receiver_arm"

echo ""
echo "=== 部署到开发板 ==="
echo "1. 传输文件到开发板:"
echo "   cd $BUILD_DIR"
echo "   adb push ffmpeg_sender_arm /tmp/"
echo "   adb push ffmpeg_receiver_arm /tmp/"
echo ""
echo "2. 在开发板串口运行:"
echo "   chmod +x /tmp/ffmpeg_sender_arm /tmp/ffmpeg_receiver_arm"
echo ""
echo "3. 发送端(开发板) -> 接收端(开发机):"
echo "   开发机: ffmpeg_receiver_arm /tmp/received.ts"
echo "   开发板: ffmpeg_sender_arm /tmp/test.mp4 --ip 192.168.153.128"