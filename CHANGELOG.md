# 视频传输自适应控制系统 - 项目状态文档

> 本文档记录了项目的完整状态，可用于在新的大模型对话中恢复开发环境。

## 📅 最后更新时间
2026-05-04 (v5.0 终版 — 全模块量产就绪, 固化镜像, 摄像头→UDP→接收端完整闭环)

## 🎯 项目目标
基于UDP+MPC实现自适应视频传输系统，集成libavcodec API动态码率控制，Yocto定制镜像<100MB部署至imx6ull端侧。

### 目标 vs 现状
| 组件 | 现状 | 目标 |
|------|------|------|
| 控制算法 | 5级反应式Pacing | MPC模型预测控制 |
| 码率策略 | 固定500Kbps | 预测码率+GOP自适应映射 |
| FFmpeg集成 | 原生V4L2+pipe+外部ffmpeg | ✅ Yocto集成, 80MB镜像 |
| 解码延迟 | 未测量 (B3保留) | ~0.13秒 |
| 构建系统 | Yocto Dunfell + BitBake | ✅ 一键构建+烧卡 |
| 根文件系统 | Yocto 80MB (58包) | ✅ 开箱即用, 摄像头自动识别 |
| 部署方式 | SD卡烧录, u-boot持久化 | ✅ flash-sd.sh + saveenv |
| UDP传输 | ✅ | ✅ |
| ARM交叉编译 | ✅ | ✅ |
| 摄像头采集 | ✅ | ✅ |
| Web监控 | ✅ | ✅ |

## ✅ 已完成功能

### 阶段1：基础环境搭建 ✅
- Ubuntu 18.04开发环境
- FFmpeg 7.0.2静态编译版本（`~/ffmpeg-7.0.2-amd64-static/`）
- 测试视频：`~/Videos/test.mp4`（253,555字节，248KB）

### 阶段2：C++ UDP传输系统 ✅
- **发送端**：`simple_sender` - UDP文件传输，30fps帧率控制
- **接收端**：`optimized_receiver` - 2MB缓冲，丢包统计
- 传输速度：~37 KB/s
- 丢包率：<1%

### 阶段3：自适应控制算法 ✅
- 反馈机制：接收端每2秒发送丢包率
- 动态调整：Pacing调速，1000~100000us包间隔
- 5级自适应：panic(×3) / 降速(×2) / 轻降(×1.33) / hold / 加速(×0.9)

### 阶段4：FFmpeg实时编码集成 ✅ (v2.0)
- **ffmpeg_sender**：调用FFmpeg实时编码H.264为MPEG-TS流，UDP发送
- **ffmpeg_receiver**：接收TS流，保存为.ts文件
- Pacing调速v2：不重启FFmpeg，只调整发送间隔
- 尾部排空：FFmpeg管道关闭后等待3秒让桥接管道清空
- 测试结果：FFmpeg实时编码+传输正常工作

### 阶段5：ARM开发板移植 ✅ (v2.1)
- **交叉编译**：`compile_arm.sh` 使用 `arm-buildroot-linux-gnueabihf-g++` 交叉编译
- **编码器适配**：支持 `libx264` / `libopenh264` / `h264_omx` / `mpeg2video` 等编码器
- **`--codec`参数**：新增命令行选项，自动选择编码器参数
- **ARM本地测试**：100ask imx6ull 开发板回环测试通过 (368KB, 284Kbps, 1.5%丢包)

### 阶段6：ADB桥接联测 ✅ (v3.0)
- **bridge_host.py v7** — 开发机桥接: TCP↔UDP，反馈通道使用持久TCP连接(避免FIN截断)
- **udp_tcp_bridge.py v6** — 开发板桥接: UDP↔TCP，select.select() + 非阻塞socket + pending_queue重试
- **联测结果**：开发板→ADB→开发机 传输成功，294包全部到达，0丢包，自适应Pacing正常工作
- **已修复的三个关键Bug**:
  1. 反馈通道断裂：每次新建TCP+sendall+立即close → 持久TCP连接
  2. 尾部丢包(24/294)：发送端退出过快 → 3秒管道排空期
  3. 设备桥接BlockingIOError静默丢包 → pending_queue重试队列

### 阶段7：Web可视化监控界面 ✅ (v3.0)
- **web_monitor.py** — 独立Web服务，无外部依赖
  - UDP:1237 接收 JSON 统计（来自 ffmpeg_receiver 每2秒发送）
  - HTTP:8080 仪表盘页面 + `/api/stats` JSON API
  - 深色主题，5个统计卡片（包数/数据量/码率/丢包率/耗时），自动刷新
  - 丢包率颜色编码：绿色(0%)、黄色(0.5-3%)、红色(>3%)
- **ffmpeg_receiver.cpp** — 新增 `sendStats()` 方法，JSON格式统计输出
- **计时修正** — 使用 `first_data_time`（首个包到达时间）计算码率，不再包含等待sender启动的时间

### 阶段8：接收端计时修正 ✅ (v3.0)
- 原先用 `start_time`（进程启动时间）计算平均码率，导致等待sender期间被平均
- 新增 `first_data_time`，收到第一个数据包时更新
- 所有码率/统计计算改用 `first_data_time`

### 阶段9：USB摄像头实时采集 ✅ (v3.1) — 已实测验证
- **ffmpeg_sender** 新增 `--camera` 模式，支持V4L2 USB摄像头实时采集
- 新增参数: `--camera /dev/videoX`, `--resolution 640x480`, `--fps 15`, `--duration N`
- SIGINT/SIGTERM 信号处理，Ctrl+C 优雅关闭FFmpeg管道
- 摄像头模式自动适配FFmpeg命令：`-f v4l2 -video_size WxH -framerate N -i /dev/videoX`
- 摄像头模式排空时间缩短为1秒（vs 文件模式3秒）
- **实测验证通过** (2026-05-02): USB 2.0 Camera @ /dev/video1
  - 注意: imx6ull的/dev/video0是pxp内部设备, USB摄像头是/dev/video1
  - 格式: YUYV 4:2:2, 支持640x480@30fps最高1600x1200@9fps; MJPG压缩格式也支持
  - 30s采集测试: 1056包, 1408KB, 369.8Kbps, **0丢包**
  - Pacing: 3000us → 1000us 自动收敛, 自适应全链路正常
  - ffprobe确认: mpeg2video 4:2:2 yuv422p, 640x480@15fps, 时长29.60s

## 📁 项目目录结构
```
~/video_stream_project/
├── src/
│   ├── ffmpeg_sender.cpp          # FFmpeg实时编码发送端 (核心！)
│   └── ffmpeg_receiver.cpp        # TS流接收端 (核心！)
├── build/
│   ├── ffmpeg_sender              # x86版本
│   ├── ffmpeg_receiver            # x86版本
│   ├── ffmpeg_sender_arm          # ARM版本 (交叉编译)
│   └── ffmpeg_receiver_arm        # ARM版本 (交叉编译)
├── bridge_host.py                 # 开发机桥接 (TCP↔UDP, ADB转发) v7
├── udp_tcp_bridge.py              # 开发板桥接 (源码) v6
├── web_monitor.py                 # Web监控界面 v1
├── compile.sh                     # x86编译脚本
├── compile_arm.sh                 # ARM交叉编译脚本
├── README.md
└── PROJECT_STATUS.md
```

## 🔧 核心代码架构

### ffmpeg_sender.cpp (发送端) — v5.0 原生V4L2 + pipe+fork
```
V4L2Capture(捕获线程) → mmap buffer → atomic flag
    ↓ 主线程
write(ffmpeg_stdin_fd, raw YUYV)
    ↓ pipe
ffmpeg子进程: stdin→rawvideo→mpeg2video→MPEG-TS→stdout
    ↓ pipe (non-blocking poll)
TS数据 → sendPacket() → UDP
    ↓ 每2秒反馈丢包率
自适应MPC Pacing (1000~100000us)
    ↓
退出 (duration / Ctrl+C → close stdin → ffmpeg flush → 排空 → 完成)
```

### ffmpeg_receiver.cpp (接收端)
```
UDP接收 (端口1234)
    ↓ 解析4B序列号 → 序列号空洞检测
写入.ts文件
    ↓ 每2秒
  ├─ 发送LOSS:x.x%反馈 (端口1235)
  └─ 发送JSON统计 (端口1237) → web_monitor.py
```

### bridge_host.py (开发机桥接) v7
```
DATA: TCP:1234 (来自ADB forward) → 读4B帧头→读数据→剥帧头 → UDP:1234 (ffmpeg_receiver)
FEEDBACK: UDP:1235 (来自ffmpeg_receiver) → 持久TCP连接 → ADB forward tcp:1235 → 开发板
```

### udp_tcp_bridge.py (开发板桥接) v6
```
DATA: UDP:1234 (来自ffmpeg_sender) → 4B帧头封装 → TCP server :1236 → ADB forward → 开发机
       BlockingIOError → pending_queue重试队列 (不丢包)
FEEDBACK: TCP server :1235 (来自ADB forward) → UDP:1235 (ffmpeg_sender反馈)
```

### web_monitor.py (Web监控) v1
```
UDP:1237 ← JSON统计 (来自ffmpeg_receiver)
HTTP:8080
  ├─ GET /        → 仪表盘 HTML (深色主题, 自动刷新)
  └─ GET /api/stats → JSON 统计
```

## 🚀 使用方式

### x86 (开发机本地测试)
```bash
cd ~/video_stream_project

# 编译
./compile.sh

# 终端1：接收端
./build/ffmpeg_receiver received.ts

# 终端2：发送端
./build/ffmpeg_sender ~/Videos/test.mp4
```

### ARM (100ask imx6ull) - 开发机↔开发板 (完整流程)

#### 文件传输模式
```bash
cd ~/video_stream_project

# 编译 + 推送
./compile.sh && ./compile_arm.sh
adb push build/ffmpeg_sender_arm /tmp/
adb push build/ffmpeg_receiver_arm /tmp/
adb push udp_tcp_bridge.py /tmp/bridge.py
adb shell 'chmod +x /tmp/ffmpeg_sender_arm /tmp/ffmpeg_receiver_arm'

# 清理
pkill -9 -f ffmpeg_receiver 2>/dev/null
pkill -9 -f bridge_host 2>/dev/null
pkill -9 -f web_monitor 2>/dev/null
adb shell 'killall python3 ffmpeg_sender_arm 2>/dev/null'
adb forward --remove tcp:1234 2>/dev/null
adb forward --remove tcp:1235 2>/dev/null

# 终端1：Web监控 + 接收端
python3 web_monitor.py &
./build/ffmpeg_receiver /tmp/recv_cross.ts --ip 127.0.0.1 &

# 终端2：桥接
python3 bridge_host.py &

# 终端3：开发板发送
adb shell '
killall python3 2>/dev/null
sleep 1
python3 /tmp/bridge.py &
sleep 2
/tmp/ffmpeg_sender_arm /tmp/test.mp4 --codec mpeg2video
'

# 浏览器打开: http://<开发机IP>:8080
```

#### USB摄像头实时采集模式
```bash
# 1. 确保摄像头已插入开发板USB口
adb shell 'ls /dev/video0'

# 2. 编译 + 推送 (同文件模式)
./compile_arm.sh
adb push build/ffmpeg_sender_arm /tmp/
adb shell 'chmod +x /tmp/ffmpeg_sender_arm'

# 3. 启动接收端 + 桥接 (终端1/2同文件模式)

# 4. 开发板摄像头采集 (终端3)
adb shell '
python3 /tmp/bridge.py &
sleep 2
/tmp/ffmpeg_sender_arm --camera /dev/video0 --duration 30 --codec mpeg2video
'
# 或无限采集直到Ctrl+C:
adb shell '/tmp/ffmpeg_sender_arm --camera /dev/video0 --codec mpeg2video'

# 5. 浏览器打开 http://<开发机IP>:8080 实时监控
```

### ARM - 开发板本地回环
```bash
adb shell '
chmod +x /tmp/ffmpeg_sender_arm /tmp/ffmpeg_receiver_arm
/tmp/ffmpeg_receiver /tmp/received.ts &
sleep 1
/tmp/ffmpeg_sender_arm /tmp/test.mp4 --codec mpeg2video
'
```

## 📊 性能数据

| 指标 | 数值 |
|------|------|
| FFmpeg编码格式 | H.264 / mpeg2video MPEG-TS |
| 编码码率 | 500Kbps (固定) |
| UDP包大小 | 1400 字节 (4B seq + 1396B payload) |
| Pacing范围 | 1000~100000 us |
| 反馈间隔 | 2秒 |
| ADB桥接联测 | 294包全部到达，0丢包 |
| 控制算法 | **MPC预测控制 v1.0** (枚举7候选×N=3时域) |
| MPC收敛 | 3000→1000us, 1步到位, 0震荡 |
| ARM编码码率 | ~320-370 Kbps (mpeg2video) |
| Web监控端口 | HTTP:8080 |
| 摄像头采集分辨率 | 默认640x480, 可配 |
| 摄像头采集帧率 | 默认15fps, 可配 |
| MPC源码 | src/mpc_controller.hpp (200行, 零依赖) |

## 📋 任务路线图 (v4.0 MPC + Yocto)

### 路线调整说明
跳过MATLAB/Simulink建模仿真(A1/A2)，采用**在线系统辨识+解析MPC**方案:
- 在C++中实时估算网络模型参数（丢包-吞吐量关系）
- 使用解析解/查表法替代数值QP求解器（imx6ull Cortex-A7过于轻量）
- 用实测数据验证而非仿真

### 阶段A3: MPC控制器 ✅ (2026-05-02, 实际约2h)
- [x] 网络状态在线辨识（双估计器: `bw_est`网络带宽 + `enc_est`编码器能力 + `model_gain`自适应灵敏度）
- [x] 轻量MPC优化器（枚举7候选速率×3步预测时域，约210次浮点运算/周期，适配Cortex-A7 VFPv4）
- [x] 替换5级if-else `adaptPacing()` → MPC预测控制
- [x] `#ifdef USE_MPC` 双模式编译，去掉flag即回退旧算法
- [x] ARM实测验证: pace 3000→1000us 一步收敛, 329Kbps, 0丢包
- [x] 两轮Bug修复: (1)候选基准改用实际吞吐量非理论速率 (2)冷启动保护+enc_est仅pacing=1000时更新
- [ ] 预测码率→GOP映射策略（待B1/B2 FFmpeg API完成后）
- **新增文件**: `src/mpc_controller.hpp` (200行, 零依赖, 纯头文件)
- **修改文件**: `src/ffmpeg_sender.cpp` (+#ifdef USE_MPC 双模式), `compile.sh`, `compile_arm.sh`

### 当前进度: A3 ✅ → B1 ✅ (2026-05-03)

### 阶段B1: 原生V4L2替代libavdevice ✅ (实测完成)
- [x] `src/v4l2_capture.hpp` 创建 — 原生Linux V4L2 API (open/ioctl/mmap/VIDIOC_DQBUF)
- [x] 捕获线程模型 — ARM V4L2驱动O_NONBLOCK无效, 用独立线程阻塞DQBUF规避
- [x] `ffmpeg_sender.cpp` 重写为 pipe+fork+exec 架构
- [x] V4L2 YUYV → pipe/stdin → 外部ffmpeg(rawvideo→mpeg2video→MPEG-TS) → stdout/pipe → UDP发送
- [x] 零libav依赖编译: `-lpthread` only (去掉 -lavcodec -lavformat -lswscale -lavutil)
- [x] **ARM实测**: 15s采集, 429包, 519KB, 264Kbps, **0丢包**, 109帧捕获
- [x] 三个ARM FFmpeg Bug已确认并规避:
  1. `av_read_frame` 阻塞 — 改用原生V4L2
  2. `avcodec_receive_packet` 背靠背调用阻塞 — 弃用libav API
  3. `av_interleaved_write_frame` 自定义AVIO回调阻塞 — 弃用muxer
- [x] `video_encoder.hpp` 保留为参考 (libav API路径已废弃, 外部ffmpeg替代)

**架构决策**: ARM FFmpeg 4.2.3 的 libavcodec/libavformat C API 在 imx6ull 上有多个阻塞bug。改用外部ffmpeg二进制(popen/pipe+fork)是最稳定的方案。B2动态码率将通过对ffmpeg子进程发信号(SIGUSR1→重启ffmpeg带新参数)或运行时修改ffmpeg命令行重 exec 实现。

### 阶段B2: 动态码率/GOP ✅ (2026-05-03, 实际约1h)
- [x] `restartFFmpeg()` 实现 — 关stdin→排空→收尸→新进程(新码率)
- [x] MPC `bw_est_kbps` → 编码码率映射 (×0.9, clamp 100-1500Kbps)
- [x] 自动检测: 每2s检查, 码率差异>25%触发重启
- [x] **ARM实测**: 500→370→275Kbps 两步收敛, Pace 3000→1000us, 0丢包
- [x] 反馈链路全通: receiver→bridge_host→ADB→device→sender (MPC闭环)
- [x] 浏览器仪表盘实时显示: 包数/数据量/码率/丢包率/耗时

### 阶段B3: 解码延迟优化 (预计1-2天)
- [ ] receiver端帧时间戳打点（进入解码→输出）
- [ ] 延迟测量与瓶颈分析
- [ ] 目标: 解码延迟 ~0.13s

### 阶段C1: Yocto环境搭建 ✅ (2026-05-04)
- [x] Poky Dunfell 3.1 + meta-openembedded 安装
- [x] qemuarm 首次构建: 3103任务通过
- [x] 开发机: Ubuntu 18.04, g++ 7.4.0, 201GB可用空间

### 阶段C2: 最小rootfs裁剪 ✅ (2026-05-04)
- [x] `meta-video-stream` layer 创建 (layer.conf + 4 recipes)
- [x] ffmpeg-sender recipe (交叉编译, USE_MPC, -lpthread)
- [x] device-bridge recipe (python3 + systemd auto-start)
- [x] video-stream-image recipe (继承 core-image)
- [x] ffmpeg bbappend: 禁用X11/x264等外部codec, --disable-avdevice
- [x] 构建验证: 4224 → 3245任务全部通过

### 阶段C3: BitBake Recipe ✅ (2026-05-04)
- [x] 48包最小系统 (alsa/bluez/nfs/distcc/X11全部移除)
- [x] 镜像大小: 80MB ext4 / 15MB tar.bz2
- [x] 含: ffmpeg(精简) + ffmpeg-sender + device-bridge + python3-core + libav*库

### 阶段C4: 部署集成 ✅ (2026-05-04)
- [x] SD卡制作: u-boot-dtb.imx + ext4 rootfs + 内核/模块
- [x] 启动验证: `Poky 3.1.33 qemuarm` 登录成功
- [x] 组件确认: ffmpeg_sender + bridge.py + ffmpeg + 内核模块全部就位
- [x] u-boot固化: `setenv bootcmd` 自动从SD卡启动Yocto系统
- [x] eMMC旧系统毫发无损 (拔码切回eMMC即恢复)

### Yocto 完整部署架构
```
开发板 SD 卡:
├── u-boot-dtb.imx (1KB偏移, IVT头)
├── 分区表 (MBR, 8MB开始)
└── p1: ext4 rootfs (80MB, 48包)
    ├── /boot/zImage + dtb
    ├── /lib/modules/4.9.88/ (从旧系统提取)
    ├── /usr/bin/ffmpeg (Yocto构建, 精简mpeg2video)
    ├── /usr/bin/ffmpeg_sender (交叉编译, USE_MPC)
    └── /usr/bin/bridge.py (systemd开机自启)
```

### Yocto 构建命令速查
```bash
cd ~/yocto && source poky/oe-init-build-env build-imx6ull
bitbake video-stream-image
# 产物: tmp/deploy/images/qemuarm/video-stream-image-qemuarm.{ext4,tar.bz2}
# 一键烧卡: ~/yocto/flash-sd.sh /dev/sdb
```

### 固化镜像最终能力
**首次启动**: u-boot → `setenv + saveenv` → `boot` (仅一次)
**后续启动**: u-boot → 自动从 SD 卡加载 Yocto 系统
**自动配置**: 摄像头驱动自动加载 → `/dev/video1` 直接可用
**本地测试**: `ffmpeg_receiver &` → `ffmpeg_sender --camera /dev/video1`
**跨设备测试**: USB gadget `modprobe g_ether` → 开发机 receiver → 开发板 sender

### 项目完整性清单
| 模块 | 文件 | 说明 |
|------|------|------|
| Sender | src/ffmpeg_sender.cpp + mpc_controller.hpp + v4l2_capture.hpp | 原生V4L2 + pipe+fork ffmpeg + MPC |
| Receiver | src/ffmpeg_receiver.cpp | UDP序列号空洞检测 + JSON统计 |
| Bridge (board) | udp_tcp_bridge.py | UDP↔TCP ADB转发 |
| Bridge (host) | bridge_host.py | TCP↔UDP ADB转发 |
| Web monitor | web_monitor.py | HTTP:8080 仪表盘 |
| Yocto layer | ~/yocto/meta-video-stream/ | 完整构建系统 |
| Flash script | ~/yocto/flash-sd.sh | 一键SD卡烧写 |
| 旧系统备份 | eMMC (拔码恢复) | 毫发无损 |

### Yocto 构建命令
```bash
cd ~/yocto && source poky/oe-init-build-env build-imx6ull
bitbake video-stream-image
# 产物: tmp/deploy/images/qemuarm/video-stream-image-qemuarm.{ext4,tar.bz2}
```

### Layer 结构
```
~/yocto/meta-video-stream/
├── conf/layer.conf
├── recipes-video/
│   ├── ffmpeg-sender/ffmpeg-sender_1.0.bb
│   ├── device-bridge/device-bridge_1.0.bb
│   └── images/video-stream-image.bb
└── recipes-multimedia/ffmpeg/ffmpeg_%.bbappend
```

### 长期（后续版本）
- [ ] Yocto系统集成
- [ ] 机器学习预测网络状态

## ⚡ 快速恢复命令

```bash
# 编译
cd ~/video_stream_project && ./compile.sh && ./compile_arm.sh

# 推送ARM二进制到开发板
adb push build/ffmpeg_sender_arm /tmp/
adb push build/ffmpeg_receiver_arm /tmp/
adb push udp_tcp_bridge.py /tmp/bridge.py
adb shell 'chmod +x /tmp/ffmpeg_sender_arm /tmp/ffmpeg_receiver_arm'

# 整套ARM测试流程 (文件模式)
pkill -9 -f ffmpeg_receiver 2>/dev/null
pkill -9 -f bridge_host 2>/dev/null
pkill -9 -f web_monitor 2>/dev/null
adb shell 'killall python3 ffmpeg_sender_arm 2>/dev/null'
adb forward --remove tcp:1234 2>/dev/null
adb forward --remove tcp:1235 2>/dev/null

# 终端1
python3 web_monitor.py &
./build/ffmpeg_receiver /tmp/recv_cross.ts --ip 127.0.0.1 &

# 终端2
python3 bridge_host.py &

# 终端3 (文件传输)
adb shell 'killall python3 2>/dev/null; sleep 1; python3 /tmp/bridge.py & sleep 2; /tmp/ffmpeg_sender_arm /tmp/test.mp4 --codec mpeg2video'

# 终端3 (摄像头采集 30秒)
adb shell 'killall python3 2>/dev/null; sleep 1; python3 /tmp/bridge.py & sleep 2; /tmp/ffmpeg_sender_arm --camera /dev/video0 --duration 30 --codec mpeg2video'
```

## 📝 关键文件清单

| 文件 | 说明 |
|------|------|
| `src/ffmpeg_sender.cpp` | 发送端 v5.0 (原生V4L2捕获 + pipe+fork ffmpeg编码 + MPC Pacing) |
| `src/ffmpeg_receiver.cpp` | TS流接收端，序列号空洞检测，JSON统计输出 (核心) |
| `src/v4l2_capture.hpp` | **原生V4L2摄像头采集** — mmap+线程+原子标志, 替代libavdevice |
| `build/ffmpeg_sender` / `ffmpeg_receiver` | x86编译版本 |
| `build/ffmpeg_sender_arm` / `ffmpeg_receiver_arm` | ARM交叉编译版本 (**零libav依赖**: -lpthread only) |
| `compile.sh` / `compile_arm.sh` | 编译脚本 |
| `bridge_host.py` | 开发机ADB桥接 v7 (持久TCP反馈+帧协议数据) |
| `udp_tcp_bridge.py` | 开发板桥接 v6 (select+非阻塞+pending_queue重试) |
| `src/mpc_controller.hpp` | **MPC预测控制器 v1.0** — 在线辨识+枚举预测, 替代5级反应式 |
| `web_monitor.py` | Web监控界面 v1 (UDP:1237→HTTP:8080仪表盘) |

## 🔄 恢复开发指南

在新对话中，复制以下内容：

```
我的视频传输项目在 ~/video_stream_project/
核心文件：ffmpeg_sender.cpp, ffmpeg_receiver.cpp
桥接脚本：bridge_host.py (开发机), udp_tcp_bridge.py (开发板)
Web监控：web_monitor.py (开发机)
环境：Ubuntu 18.04, gcc 7.4.0, Python 3.6
FFmpeg在 ~/ffmpeg-7.0.2-amd64-static/ (开发机), /usr/bin/ffmpeg (开发板)
交叉编译器：arm-buildroot-linux-gnueabihf-g++ 7.5.0
路径：/home/book/100ask_imx6ull-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot

已完成：
- UDP传输、自适应Pacing控制(5级)、FFmpeg实时编码集成
- ARM交叉编译 (100ask imx6ull, mpeg2video编码)
- ADB桥接联测通过 (294包0丢包, 反馈+自适应全链路工作)
- Web可视化监控界面 (HTTP:8080, 5卡片仪表盘, 自动刷新)
- 三个关键Bug已修复 (反馈断裂/尾部丢包/BlockingIOError静默丢包)
- USB摄像头实时采集验证通过 (V4L2 /dev/video1, 30s/1056包/0丢包)

当前阶段: v4.0 MPC+Yocto
- 跳过A1/A2(MATLAB建模仿真), 采用在线辨识+解析MPC
- 路线: A3(MPC控制器) → B1(FFmpeg API重构) → B2(动态码率) → B3(延迟优化) → C1-C4(Yocto集成)
- 预估4-6周

项目状态文档：PROJECT_STATUS.md (完整架构、命令、性能数据、路线图)

另外需要记住，开发机和开发板并不是这台虚拟机，所以不要在这台虚拟机的终端运行交叉编译和adb相关的命令，而是指导我如何操作。
现在需要你全面阅读相关代码，详细分析，有不确定的问题可以向我提问，直到有把握再进行操作

接下来想做：[具体任务]
```
