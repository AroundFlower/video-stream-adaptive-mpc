#!/bin/bash
# 视频传输项目编译脚本 (本地 x86_64)
set -e

SRC_DIR="$(cd "$(dirname "$0")" && pwd)/src"
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/build"

echo "=== 视频传输项目编译 (x86_64) ==="
echo "源文件: $SRC_DIR"
echo "输出目录: $BUILD_DIR"
echo "==================================="

mkdir -p "$BUILD_DIR"

# FFmpeg发送端 (v4 - MPC + 原生V4L2 + popen fallback)
echo -n "[1/2] 编译 ffmpeg_sender ... "
g++ -std=c++11 -O2 -Wall -DUSE_MPC \
    "$SRC_DIR/ffmpeg_sender.cpp" \
    -o "$BUILD_DIR/ffmpeg_sender" \
    -lpthread 2>&1 && echo "OK" || echo "FAIL"

# FFmpeg接收端 (v3 - 序列号空洞检测)
echo -n "[2/2] 编译 ffmpeg_receiver ... "
g++ -std=c++11 -O2 -Wall \
    "$SRC_DIR/ffmpeg_receiver.cpp" \
    -o "$BUILD_DIR/ffmpeg_receiver" \
    -lpthread 2>&1 && echo "OK" || echo "FAIL"

echo ""
echo "=== 编译完成 ==="
ls -lh "$BUILD_DIR/ffmpeg_sender" "$BUILD_DIR/ffmpeg_receiver"
echo ""
echo "== 本地回环测试命令 =="
echo "开发机终端1: $BUILD_DIR/ffmpeg_receiver output.ts"
echo "开发机终端2: $BUILD_DIR/ffmpeg_sender ~/Videos/test.mp4"