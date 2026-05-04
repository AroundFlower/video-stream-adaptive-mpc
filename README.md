# 嵌入式视频流自适应传输系统

基于 **UDP + MPC 模型预测控制** 的实时视频传输系统，运行于 **ARM Cortex-A7 (i.MX6 ULL)** 平台。原生 Linux V4L2 摄像头采集 → FFmpeg 硬件编码 → 自适应码率/速率控制 → UDP 网络传输 → 接收端存储 + Web 实时监控。

## 技术栈

`C++11` `Python3` `FFmpeg` `Yocto Dunfell` `Linux V4L2` `MPC` `UDP` `systemd`

## 系统架构

```
┌─ 开发板 (ARM i.MX6 ULL, Yocto 80MB rootfs) ─────────────┐
│                                                            │
│  USB摄像头 → V4L2Capture(线程, mmap) → raw YUYV            │
│       ↓                                                    │
│  pipe → fork → ffmpeg (mpeg2video编码 → MPEG-TS)          │
│       ↓                                                    │
│  MPC Pacing控制器 ← 反馈(丢包率)                           │
│       ↓ 动态码率(500→370→275Kbps)                          │
│  UDP 发送 → ADB桥接 / USB虚拟网卡                          │
│                                                            │
├─ 开发机 (Ubuntu 18.04) ────────────────────────────────────┤
│                                                            │
│  UDP 接收 → 序列号空洞检测 → .ts 文件存储                  │
│  Web 监控 (HTTP:8080, 5卡片仪表盘, 自动刷新)              │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

## 关键指标

| 指标 | 数值 |
|------|------|
| 编码格式 | mpeg2video, YUV 4:2:2 |
| 分辨率 / 帧率 | 640×480 @ 15fps |
| 动态码率范围 | 275–500 Kbps (MPC自动) |
| Pacing 范围 | 1000–100000 μs |
| 端到端丢包率 | **0%** |
| 反馈间隔 | 2 秒 |
| Yocto 镜像大小 | **<80 MB** (48 包) |
| 交叉编译器 | arm-buildroot-linux-gnueabihf-g++ 7.5.0 |

## 目录结构

```
video_stream_project/
├── src/
│   ├── ffmpeg_sender.cpp        # 发送端: V4L2 + pipe/fork ffmpeg + MPC
│   ├── ffmpeg_receiver.cpp      # 接收端: UDP 序列号空洞检测 + JSON 统计
│   ├── mpc_controller.hpp       # 轻量MPC预测控制器 (200行, 零依赖)
│   └── v4l2_capture.hpp         # 原生V4L2采集 (mmap + 捕获线程)
├── bridge_host.py               # 开发机 ADB 桥接 (TCP↔UDP)
├── udp_tcp_bridge.py            # 开发板 ADB 桥接 (UDP↔TCP)
├── web_monitor.py               # Web 监控界面 (HTTP:8080)
├── compile.sh                   # x86 编译脚本
├── compile_arm.sh               # ARM 交叉编译脚本
├── CHANGELOG.md                 # 完整项目状态与开发日志
├── yocto-layer/                 # Yocto Dunfell meta-video-stream layer
│   ├── conf/layer.conf
│   ├── recipes-video/
│   │   ├── ffmpeg-sender/       # 发送端 BitBake 配方
│   │   ├── device-bridge/       # 桥接脚本 + systemd 服务
│   │   └── images/              # 自定义镜像 (80MB)
│   └── recipes-multimedia/
│       └── ffmpeg/              # ffmpeg 瘦身配置
└── .gitignore
```

## 快速开始

### 本地回环测试 (x86)

```bash
./compile.sh
./build/ffmpeg_receiver output.ts &
./build/ffmpeg_sender test.mp4
```

### ARM 交叉编译 + ADB 联测

```bash
./compile_arm.sh
adb push build/ffmpeg_sender_arm /tmp/
adb push build/ffmpeg_receiver_arm /tmp/
adb push udp_tcp_bridge.py /tmp/bridge.py

# 开发机: 接收端 + 桥接
python3 web_monitor.py &
./build/ffmpeg_receiver /tmp/recv.ts --ip 127.0.0.1 &
python3 bridge_host.py &

# 开发板: 桥接 + 发送 (摄像头采集)
adb shell 'python3 /tmp/bridge.py &
/tmp/ffmpeg_sender_arm --camera /dev/video1 --codec mpeg2video'
```

### Yocto 构建 (完整系统镜像)

```bash
cd ~/yocto && source poky/oe-init-build-env build-imx6ull
bitbake video-stream-image
# 产物: tmp/deploy/images/qemuarm/video-stream-image-qemuarm.tar.bz2
```

## 开发阶段

| 阶段 | 内容 | 状态 |
|------|------|------|
| A3 | MPC 模型预测控制器 (枚举7候选×3步时域) | ✅ |
| B1 | 原生 V4L2 捕获 (替代 libavdevice) | ✅ |
| B2 | 动态码率 (MPC 驱动 ffmpeg 重启) | ✅ |
| B3 | 解码延迟优化 | 📋 保留 |
| C1-C4 | Yocto 集成 (Dunfell, 80MB 镜像, SD卡启动) | ✅ |
| 固化 | 镜像量产就绪, u-boot 持久化, 一键烧卡 | ✅ |

## License

MIT
