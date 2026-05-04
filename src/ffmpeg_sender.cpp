#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <stdio.h>
#include <signal.h>

#ifdef USE_MPC
#include "mpc_controller.hpp"
#endif
#include "v4l2_capture.hpp"
#include <sys/wait.h>

// 全局变量用于SIGINT信号处理
class FFmpegSender;
static FFmpegSender* g_sender = nullptr;
static volatile bool g_running = true;

static void sigHandler(int) {
    g_running = false;
}

class FFmpegSender {
private:
    // 网络
    int data_sockfd;
    int feedback_sockfd;
    struct sockaddr_in dest_addr;
    const int DATA_PORT = 1234;
    const int FEEDBACK_PORT = 1235;
    const int MAX_PAYLOAD_SIZE = 1396;  // 包体=1500MTU - 20IP - 8UDP - 4seq
    const int MAX_UDP_DATAGRAM = 1500;

    // 发送调速（Pacing）
    int pacing_delay_us;   // 每次发送后的休眠微秒数
    float last_loss_rate;
    bool feedback_received;
#ifdef USE_MPC
    MPCPacingController mpc_;
#endif

    // FFmpeg进程（只启动一次）
    FILE* ffmpeg_pipe;
    int ffmpeg_fd;         // fileno(ffmpeg_pipe) 缓存
    std::string ffmpeg_path;
    std::string vcodec;    // 视频编码器

    // 序列号
    uint32_t seq_num;

    // 目标IP（默认127.0.0.1，可通过参数指定）
    std::string target_ip;

    // 摄像头采集模式
    bool use_camera;
    std::string camera_device;
    std::string resolution;
    int fps;
    int duration_sec;

#ifdef USE_MPC
    int interval_bytes_sent_;
    std::chrono::steady_clock::time_point interval_start_;
#endif
    // 原生V4L2摄像头采集 + pipe到外部ffmpeg编码
    V4L2Capture capture_;
    int64_t frame_counter_;
    int cam_w_, cam_h_;
    int ffmpeg_stdin_fd_;       // 写raw YUYV给ffmpeg stdin
    pid_t ffmpeg_pid_;           // ffmpeg子进程
    int current_bitrate_kbps_;   // 当前编码码率 (MPC动态调整)

    // 统计
    int total_packets_sent;
    int total_bytes_sent;

public:
    FFmpegSender(const std::string& ffmpeg = "",
                 const std::string& ip = "127.0.0.1",
                 const std::string& codec = "libx264",
                 bool camera = false,
                 const std::string& cam_dev = "/dev/video0",
                 const std::string& res = "640x480",
                 int cam_fps = 15,
                 int duration = 0)
        : ffmpeg_pipe(nullptr), ffmpeg_fd(-1), ffmpeg_path(ffmpeg),
          vcodec(codec), seq_num(1), target_ip(ip),
          use_camera(camera), camera_device(cam_dev), resolution(res),
          fps(cam_fps), duration_sec(duration)
#ifdef USE_MPC
          , interval_bytes_sent_(0)
#endif
          , frame_counter_(0), cam_w_(640), cam_h_(480)
          , ffmpeg_stdin_fd_(-1), ffmpeg_pid_(0)
          , current_bitrate_kbps_(500)
    {
        if (ffmpeg_path.empty()) {
            // 默认：先尝试 $HOME/ffmpeg-...，再试 /usr/bin/ffmpeg
            const char* home = getenv("HOME");
            std::string user_path = std::string(home) + "/ffmpeg-7.0.2-amd64-static/ffmpeg";
            struct stat st;
            if (stat(user_path.c_str(), &st) == 0) {
                ffmpeg_path = user_path;
            } else {
                ffmpeg_path = "/usr/bin/ffmpeg";
            }
        }

        // 数据socket（目标UDP:1234）
        data_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(DATA_PORT);
        dest_addr.sin_addr.s_addr = inet_addr(target_ip.c_str());

        int send_buf = 4 * 1024 * 1024;
        setsockopt(data_sockfd, SOL_SOCKET, SO_SNDBUF, &send_buf, sizeof(send_buf));

        // 反馈socket（非阻塞）
        feedback_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in fb_addr;
        memset(&fb_addr, 0, sizeof(fb_addr));
        fb_addr.sin_family = AF_INET;
        fb_addr.sin_port = htons(FEEDBACK_PORT);
        fb_addr.sin_addr.s_addr = INADDR_ANY;
        bind(feedback_sockfd, (struct sockaddr*)&fb_addr, sizeof(fb_addr));

        int flags = fcntl(feedback_sockfd, F_GETFL, 0);
        fcntl(feedback_sockfd, F_SETFL, flags | O_NONBLOCK);

        // 初始发送间隔：3ms → ~333包/秒 → ~1400*333*8 = ~3.7Mbps
        pacing_delay_us = 3000;
        last_loss_rate = 0;
        feedback_received = false;
        total_packets_sent = 0;
        total_bytes_sent = 0;
#ifdef USE_MPC
        interval_start_ = std::chrono::steady_clock::now();
#endif

        std::cout << "=== FFmpeg Adaptive Sender (Seq+Poll v3) ===" << std::endl;
        if (use_camera) {
            std::cout << "Mode: CAMERA (V4L2)" << std::endl;
            std::cout << "Device: " << camera_device << " | " << resolution
                      << " @" << fps << "fps" << std::endl;
            if (duration_sec > 0)
                std::cout << "Duration: " << duration_sec << "s" << std::endl;
            else
                std::cout << "Duration: unlimited (Ctrl+C to stop)" << std::endl;
        }
        std::cout << "FFmpeg: " << ffmpeg_path << std::endl;
        std::cout << "Codec: " << vcodec << std::endl;
        std::cout << "Max UDP datagram: " << MAX_UDP_DATAGRAM << " bytes" << std::endl;
        std::cout << "Max payload: " << MAX_PAYLOAD_SIZE << " bytes (+4 seq)" << std::endl;
        std::cout << "Initial pacing: " << pacing_delay_us << " us/pkt" << std::endl;
        std::cout << "===========================================" << std::endl;
    }

    ~FFmpegSender() {
        stopFFmpeg();
        close(data_sockfd);
        close(feedback_sockfd);
    }

    std::string buildFFmpegCmd(const std::string& input_file) {
        char cmd[2048];

        // 根据编码器选择不同的参数
        std::string codec_opts;
        if (vcodec == "libx264") {
            codec_opts = "-c:v libx264 -preset ultrafast -tune zerolatency";
        } else if (vcodec == "libopenh264") {
            codec_opts = "-c:v libopenh264";
        } else if (vcodec == "h264_omx") {
            codec_opts = "-c:v h264_omx";
        } else if (vcodec == "mpeg2video") {
            codec_opts = "-c:v mpeg2video";
        } else {
            codec_opts = "-c:v " + vcodec;
        }

        if (use_camera) {
            // V4L2 摄像头实时采集模式
            // -f v4l2 指定输入格式为Video4Linux2
            // -video_size 设置采集分辨率
            // -framerate 设置采集帧率
            // 不用 -re，V4L2 本身就是实时源
            snprintf(cmd, sizeof(cmd),
                "%s -f v4l2 -video_size %s -framerate %d -i \"%s\" "
                "%s "
                "-b:v 500k -maxrate 500k -bufsize 1000k "
                "-r %d -g %d "
                "-f mpegts pipe:1 2>/dev/null",
                ffmpeg_path.c_str(), resolution.c_str(), fps, input_file.c_str(),
                codec_opts.c_str(),
                fps, fps * 2  // GOP = 2×fps
            );
        } else {
            snprintf(cmd, sizeof(cmd),
                "%s -re -i \"%s\" "
                "%s "
                "-b:v 500k -maxrate 500k -bufsize 1000k "
                "-r 15 -g 30 "
                "-f mpegts pipe:1 2>/dev/null",
                ffmpeg_path.c_str(), input_file.c_str(),
                codec_opts.c_str()
            );
        }
        return std::string(cmd);
    }

    bool startFFmpeg(const std::string& input_file) {
        stopFFmpeg();
        if (use_camera) {
            // Parse resolution
            size_t xpos = resolution.find('x');
            if (xpos != std::string::npos) {
                cam_w_ = atoi(resolution.substr(0, xpos).c_str());
                cam_h_ = atoi(resolution.substr(xpos + 1).c_str());
            }
            // Open native V4L2 capture
            if (!capture_.open(input_file.c_str(), cam_w_, cam_h_, fps)) {
                std::cerr << "[ERROR] V4L2 capture open failed" << std::endl;
                return false;
            }
            if (!capture_.start()) {
                std::cerr << "[ERROR] V4L2 capture start failed" << std::endl;
                return false;
            }

            // Pipe+fork: V4L2 YUYV → ffmpeg stdin, ffmpeg stdout → TS → sender
            int stdin_pipe[2], stdout_pipe[2];
            if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
                std::cerr << "[ERROR] pipe() failed" << std::endl;
                return false;
            }

            pid_t pid = fork();
            if (pid < 0) {
                std::cerr << "[ERROR] fork() failed" << std::endl;
                return false;
            }

            if (pid == 0) {
                // --- child: exec ffmpeg ---
                close(stdin_pipe[1]);
                close(stdout_pipe[0]);
                dup2(stdin_pipe[0], STDIN_FILENO);
                dup2(stdout_pipe[1], STDOUT_FILENO);
                close(stdin_pipe[0]);
                close(stdout_pipe[1]);

                char cmd[1024];
                int br = current_bitrate_kbps_;
                snprintf(cmd, sizeof(cmd),
                    "%s -f rawvideo -pixel_format yuyv422 "
                    "-video_size %dx%d -framerate %d -i - "
                    "-c:v %s -b:v %dk -maxrate %dk -bufsize %dk "
                    "-r %d -g %d -f mpegts pipe:1 2>/dev/null",
                    ffmpeg_path.c_str(), cam_w_, cam_h_, fps,
                    vcodec.c_str(), br, br, br * 2,
                    fps, fps * 2);

                execl("/bin/sh", "sh", "-c", cmd, (char*)nullptr);
                _exit(1);
            }

            // --- parent: set up pipes ---
            close(stdin_pipe[0]);
            close(stdout_pipe[1]);

            ffmpeg_stdin_fd_ = stdin_pipe[1];   // write YUYV here
            ffmpeg_fd        = stdout_pipe[0];   // read TS from here
            ffmpeg_pid_      = pid;

            // Non-blocking read from ffmpeg stdout
            int flags = fcntl(ffmpeg_fd, F_GETFL, 0);
            fcntl(ffmpeg_fd, F_SETFL, flags | O_NONBLOCK);

            frame_counter_ = 0;
            std::cout << "\n[FFmpeg] Pipe+fork encoder: " << vcodec
                      << " " << fps << "fps " << current_bitrate_kbps_
                      << "Kbps (raw YUYV stdin)" << std::endl;
            return true;
        }
        // File mode: popen
        std::string cmd = buildFFmpegCmd(input_file);
        std::cout << "\n[FFmpeg] Encoder: " << vcodec << " "
                  << (use_camera ? fps : 15) << "fps 500Kbps" << std::endl;
        ffmpeg_pipe = popen(cmd.c_str(), "r");
        if (!ffmpeg_pipe) {
            std::cerr << "[ERROR] FFmpeg start failed!" << std::endl;
            return false;
        }
        ffmpeg_fd = fileno(ffmpeg_pipe);
        if (ffmpeg_fd < 0) {
            std::cerr << "[ERROR] Cannot get pipe fd!" << std::endl;
            pclose(ffmpeg_pipe);
            ffmpeg_pipe = nullptr;
            ffmpeg_fd = -1;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    }

    // Restart ffmpeg child with a new bitrate.
    // Called from camera loop when MPC recommends a significant change.
    bool restartFFmpeg(int new_bitrate_kbps) {
        std::cout << "\n[FFmpeg] Restarting: " << current_bitrate_kbps_
                  << " → " << new_bitrate_kbps << " Kbps" << std::endl;

        // 1. Close stdin → old ffmpeg sees EOF, flushes & exits
        if (ffmpeg_stdin_fd_ >= 0) {
            close(ffmpeg_stdin_fd_);
            ffmpeg_stdin_fd_ = -1;
        }

        // 2. Drain remaining stdout data (ffmpeg's last buffered TS packets)
        char drain_buf[8192];
        struct pollfd pfd;
        pfd.fd = ffmpeg_fd;
        pfd.events = POLLIN;
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
                int n = read(ffmpeg_fd, drain_buf, sizeof(drain_buf));
                if (n > 0) sendPacket(drain_buf, n);
                else break;
            } else break;
        }

        // 3. Close stdout & reap old child
        close(ffmpeg_fd);
        ffmpeg_fd = -1;
        int status;
        waitpid(ffmpeg_pid_, &status, 0);
        ffmpeg_pid_ = 0;

        // 4. Start new ffmpeg with updated bitrate
        current_bitrate_kbps_ = new_bitrate_kbps;

        int stdin_pipe[2], stdout_pipe[2];
        if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
            std::cerr << "[ERROR] pipe() failed in restart" << std::endl;
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[ERROR] fork() failed in restart" << std::endl;
            return false;
        }

        if (pid == 0) {
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            close(stdin_pipe[0]);
            close(stdout_pipe[1]);

            char cmd[1024];
            int br = current_bitrate_kbps_;
            snprintf(cmd, sizeof(cmd),
                "%s -f rawvideo -pixel_format yuyv422 "
                "-video_size %dx%d -framerate %d -i - "
                "-c:v %s -b:v %dk -maxrate %dk -bufsize %dk "
                "-r %d -g %d -f mpegts pipe:1 2>/dev/null",
                ffmpeg_path.c_str(), cam_w_, cam_h_, fps,
                vcodec.c_str(), br, br, br * 2,
                fps, fps * 2);

            execl("/bin/sh", "sh", "-c", cmd, (char*)nullptr);
            _exit(1);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        ffmpeg_stdin_fd_ = stdin_pipe[1];
        ffmpeg_fd        = stdout_pipe[0];
        ffmpeg_pid_      = pid;

        int flags = fcntl(ffmpeg_fd, F_GETFL, 0);
        fcntl(ffmpeg_fd, F_SETFL, flags | O_NONBLOCK);

        return true;
    }

    void stopFFmpeg() {
        capture_.release();

        // Close stdin pipe → ffmpeg sees EOF, flushes remaining output & exits
        if (ffmpeg_stdin_fd_ >= 0) {
            close(ffmpeg_stdin_fd_);
            ffmpeg_stdin_fd_ = -1;
        }

        // Drain remaining stdout & reap child
        if (ffmpeg_pid_ > 0) {
            // Let ffmpeg finish flushing (drain with timeout)
            char drain_buf[8192];
            struct pollfd pfd;
            pfd.fd = ffmpeg_fd;
            pfd.events = POLLIN;
            auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(2000);
            while (std::chrono::steady_clock::now() < deadline) {
                if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
                    int n = read(ffmpeg_fd, drain_buf, sizeof(drain_buf));
                    if (n <= 0) break;
                } else {
                    break;  // no more data within 200ms
                }
            }
            close(ffmpeg_fd);
            ffmpeg_fd = -1;

            // Reap child (kill if stuck)
            int status;
            if (waitpid(ffmpeg_pid_, &status, WNOHANG) == 0) {
                usleep(500000);  // 500ms grace
                if (waitpid(ffmpeg_pid_, &status, WNOHANG) == 0) {
                    kill(ffmpeg_pid_, SIGTERM);
                    waitpid(ffmpeg_pid_, &status, 0);
                }
            }
            ffmpeg_pid_ = 0;
        }

        if (ffmpeg_pipe) {
            pclose(ffmpeg_pipe);
            ffmpeg_pipe = nullptr;
            ffmpeg_fd = -1;
        }
    }

    // 读取接收端反馈（非阻塞）
    void checkFeedback() {
        char buffer[64];
        struct sockaddr_in sender_addr;
        socklen_t addr_len = sizeof(sender_addr);

        while (true) {
            int bytes = recvfrom(feedback_sockfd, buffer, sizeof(buffer)-1, 0,
                                (struct sockaddr*)&sender_addr, &addr_len);
            if (bytes <= 0) break;

            buffer[bytes] = '\0';
            float loss = 0;
            sscanf(buffer, "LOSS:%f", &loss);
            last_loss_rate = loss;
            feedback_received = true;
        }
    }

#ifdef USE_MPC
    // === MPC自适应Pacing算法 v4 ===
    // 枚举MPC：评估7个候选速率，预测N=3步，选最小成本
    void adaptPacing() {
        if (!feedback_received) return;

        // 计算过去2秒实际吞吐量
        auto now = std::chrono::steady_clock::now();
        auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - interval_start_).count();
        int actual_kbps = 0;
        if (interval_ms > 0) {
            actual_kbps = (int)((long long)interval_bytes_sent_ * 8 / interval_ms);
        }

        int old_delay = pacing_delay_us;
        int new_pacing = mpc_.update(last_loss_rate, actual_kbps, pacing_delay_us);
        pacing_delay_us = new_pacing;

        if (old_delay != new_pacing) {
            printf("\n[MPC] Pace %d->%dus | Loss %.1f%% | Thru %dKbps | "
                   "BW %.0fKbps | ENC %.0fKbps | Gain %.2f | Cost %.1f",
                   old_delay, new_pacing, last_loss_rate, actual_kbps,
                   mpc_.bw_est_kbps, mpc_.enc_est_kbps, mpc_.model_gain, mpc_.last_cost);
            fflush(stdout);
        }

        // 重置interval统计
        interval_bytes_sent_ = 0;
        interval_start_ = now;
        feedback_received = false;
    }
#else
    // === 自适应Pacing算法 v2 (legacy 5-level) ===
    void adaptPacing() {
        if (!feedback_received) return;

        int old_delay = pacing_delay_us;

        if (last_loss_rate > 15.0f) {
            pacing_delay_us = std::min(pacing_delay_us * 3, 100000);
        } else if (last_loss_rate > 8.0f) {
            pacing_delay_us = std::min(pacing_delay_us * 2, 50000);
        } else if (last_loss_rate > 3.0f) {
            pacing_delay_us = std::min(pacing_delay_us * 4 / 3, 30000);
        } else if (last_loss_rate > 0.5f) {
            // hold
        } else {
            pacing_delay_us = std::max(pacing_delay_us * 9 / 10, 1000);
        }

        if (old_delay != pacing_delay_us) {
            int est_kbps = (MAX_PAYLOAD_SIZE * 8 * 1000000 / pacing_delay_us) / 1000;
            printf("\n[Adapt] Pacing: %dus -> %dus (~%dKbps) | Loss:%.1f%%",
                   old_delay, pacing_delay_us, est_kbps, last_loss_rate);
            fflush(stdout);
        }

        feedback_received = false;
    }
#endif

    // 发送单个UDP包，前面加4字节序列号
    // 返回发送的数据字节数（不含序列号头）
    void sendPacket(const char* data, int len) {
        // 构建UDP数据： [4字节序列号(大端)] + [payload]
        uint32_t packet_seq = __builtin_bswap32(seq_num);

        // 将大块数据以 MAX_PAYLOAD_SIZE 分片发送，每片带独立序列号
        int offset = 0;
        while (offset < len) {
            int chunk = std::min(MAX_PAYLOAD_SIZE, len - offset);

            // 分配: 4字节头 + payload
            char udp_buf[4 + MAX_PAYLOAD_SIZE];
            memcpy(udp_buf, &packet_seq, 4);
            memcpy(udp_buf + 4, data + offset, chunk);

            sendto(data_sockfd, udp_buf, 4 + chunk, 0,
                   (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            total_packets_sent++;
            total_bytes_sent += chunk;
#ifdef USE_MPC
            interval_bytes_sent_ += chunk;
#endif
            offset += chunk;

            // 每个分片递增序列号
            seq_num++;
            packet_seq = __builtin_bswap32(seq_num);

            // 每个小包之间也做pacing
            if (pacing_delay_us > 0) {
                usleep(pacing_delay_us);
            }
        }
    }

    bool sendStream(const std::string& input_file) {
        struct stat st;
        if (stat(ffmpeg_path.c_str(), &st) != 0) {
            std::cerr << "[ERROR] FFmpeg not found: " << ffmpeg_path << std::endl;
            return false;
        }

        if (use_camera) {
            std::cout << "Camera: " << input_file << std::endl;
        } else {
            if (stat(input_file.c_str(), &st) != 0) {
                std::cerr << "[ERROR] Input not found: " << input_file << std::endl;
                return false;
            }
            std::cout << "Input: " << input_file << " (" << st.st_size << " bytes)" << std::endl;
        }

        if (!startFFmpeg(input_file)) return false;

        char buffer[8192];
        auto start_time = std::chrono::steady_clock::now();
        auto last_fb_check = start_time;
        auto last_status = start_time;
        auto last_pipe_data = start_time;   // 上次从pipe读到数据的时间

        struct pollfd pfd;
        pfd.fd = ffmpeg_fd;
        pfd.events = POLLIN;

        while (g_running) {
            // 摄像头模式：duration检查
            if (use_camera && duration_sec > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                if (elapsed >= duration_sec) {
                    std::cout << "\n[Camera] Duration " << duration_sec << "s reached, stopping..." << std::endl;
                    break;
                }
            }

            if (use_camera) {
                // --- Step 1: Dequeue frame from V4L2, write raw YUYV to ffmpeg stdin ---
                uint8_t* raw_data = nullptr;
                size_t raw_len = 0;
                int buf_idx = -1;
                bool got_frame = capture_.dequeue(&buf_idx, &raw_data, &raw_len);

                if (got_frame && raw_data && raw_len > 0) {
                    size_t written = 0;
                    while (written < raw_len) {
                        ssize_t n = write(ffmpeg_stdin_fd_,
                                          raw_data + written,
                                          raw_len - written);
                        if (n <= 0) {
                            if (errno == EAGAIN) { usleep(500); continue; }
                            std::cerr << "\n[ERROR] write to ffmpeg stdin failed"
                                      << std::endl;
                            break;
                        }
                        written += n;
                    }
                    capture_.enqueue(buf_idx);
                    frame_counter_++;
                    last_pipe_data = std::chrono::steady_clock::now();
                }

                // --- Step 2: Read TS from ffmpeg stdout (non-blocking poll) ---
                struct pollfd pfd;
                pfd.fd = ffmpeg_fd;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                    int bytes_read = read(ffmpeg_fd, buffer, sizeof(buffer));
                    if (bytes_read > 0) {
                        sendPacket(buffer, bytes_read);
                    } else if (bytes_read <= 0) {
                        break;  // ffmpeg closed stdout (EOF or error)
                    }
                }

                // --- Step 3: Feedback & status (every 2s) ---
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

                auto fb_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fb_check).count();
                if (fb_elapsed >= 2000) {
                    checkFeedback();
                    adaptPacing();
                    last_fb_check = now;

                    // B2: MPC动态码率 — 带宽估计映射到编码器码率
#ifdef USE_MPC
                    int target_br = (int)(mpc_.bw_est_kbps * 0.9f);
                    if (target_br < 100)  target_br = 100;
                    if (target_br > 1500) target_br = 1500;
                    int diff = abs(target_br - current_bitrate_kbps_);
                    if (diff > current_bitrate_kbps_ / 4) {   // >25% change
                        restartFFmpeg(target_br);
                    }
#endif
                }

                auto status_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status).count();
                if (status_elapsed >= 2000) {
                    int eff_kbps = (total_bytes_sent * 8 * 1000) / std::max(1L, (long)elapsed) / 1000;
                    printf("\n[Send] Pkts:%d | %.0fKB | %dKbps | Pace:%dus | Loss:%.1f%% | Seq:%u | Frames:%lld | %.1fs",
                           total_packets_sent,
                           total_bytes_sent / 1024.0,
                           eff_kbps,
                           pacing_delay_us,
                           last_loss_rate,
                           seq_num - 1,
                           (long long)frame_counter_,
                           elapsed / 1000.0);
                    fflush(stdout);
                    last_status = now;
                }

                // --- Step 4: Brief sleep if no frame ---
                if (!got_frame) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;  // skip popen/poll block below
            }
            // popen pipe path (file mode, or without USE_LIBAV_API)
            int ret = poll(&pfd, 1, 500);

            if (ret < 0) {
                std::cerr << "\n[ERROR] poll() on pipe failed" << std::endl;
                break;
            }

            if (ret > 0 && (pfd.revents & POLLIN)) {
                int bytes_read = fread(buffer, 1, sizeof(buffer), ffmpeg_pipe);

                if (bytes_read > 0) {
                    last_pipe_data = std::chrono::steady_clock::now();
                    sendPacket(buffer, bytes_read);

                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

                    auto fb_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fb_check).count();
                    if (fb_elapsed >= 2000) {
                        checkFeedback();
                        adaptPacing();
                        last_fb_check = now;
                    }

                    auto status_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status).count();
                    if (status_elapsed >= 2000) {
                        int eff_kbps = (total_bytes_sent * 8 * 1000) / std::max(1L, (long)elapsed) / 1000;
                        printf("\r[Send] Pkts:%d | %.0fKB | %dKbps | Pace:%dus | Loss:%.1f%% | Seq:%u | %.1fs",
                               total_packets_sent,
                               total_bytes_sent / 1024.0,
                               eff_kbps,
                               pacing_delay_us,
                               last_loss_rate,
                               seq_num - 1,
                               elapsed / 1000.0);
                        fflush(stdout);
                        last_status = now;
                    }
                } else {
                    if (feof(ffmpeg_pipe)) {
                        if (!use_camera)
                            std::cout << "\n[FFmpeg] Stream ended (file complete)" << std::endl;
                        break;
                    }
                    continue;
                }
            } else if (ret == 0) {
                // poll超时 - 500ms内pipe无数据
                auto now = std::chrono::steady_clock::now();
                auto pipe_stall = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pipe_data).count();

                // 如果超过10秒没有从pipe读到任何数据，认为FFmpeg卡死
                if (pipe_stall > 10000 && total_packets_sent > 0) {
                    std::cerr << "\n[ERROR] FFmpeg pipe stalled for " << (pipe_stall/1000) << "s! Aborting." << std::endl;
                    break;
                }

                // 在空闲期间也检查一下反馈
                auto fb_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fb_check).count();
                if (fb_elapsed >= 2000) {
                    checkFeedback();
                    adaptPacing();
                    last_fb_check = now;
                }
            } else if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                // pipe异常关闭
                std::cout << "\n[FFmpeg] Pipe closed (HUP/ERR)" << std::endl;
                break;
            }
        }

        // === 尾部排空: 等待桥接管道清空 ===
        // FFmpeg pipe已关闭，但最后一批UDP包可能还在ADB桥接管道中
        // 桥接链路: sender UDP → device bridge TCP → ADB → host bridge UDP → receiver
        // 摄像头模式：缩短排空时间 (被SIGINT/duration中断，非自然结束)
        int drain_ms = use_camera ? 1000 : 3000;
        std::cout << "\n[Drain] Pipeline flush, waiting " << (drain_ms/1000) << "s..." << std::endl;
        {
            auto drain_start = std::chrono::steady_clock::now();
            while (true) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - drain_start).count();
                if (elapsed >= drain_ms) break;

                // 持续检查反馈，让接收端报告最终丢包率
                checkFeedback();
                if (elapsed % 500 < 50) {  // 每500ms调整一次
                    adaptPacing();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        auto end = std::chrono::steady_clock::now();
        auto total = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_time).count();

        std::cout << "\n\n=== " << (use_camera ? "Capture" : "Transfer")
                  << " Complete ===" << std::endl;
        std::cout << "Packets sent: " << total_packets_sent << std::endl;
        std::cout << "Data: " << (total_bytes_sent / 1024) << " KB" << std::endl;
        std::cout << "Last seq: " << (seq_num - 1) << std::endl;
        std::cout << "Time: " << total << " ms" << std::endl;
        if (total > 0) {
            std::cout << "Avg: " << (total_bytes_sent * 1000.0 / total / 1024)
                      << " KB/s | "
                      << (total_bytes_sent * 8000.0 / total / 1000)
                      << " Kbps" << std::endl;
        }

        stopFFmpeg();
        return true;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <video_file> [options]" << std::endl;
        std::cout << "       " << argv[0] << " --camera /dev/video0 [options]" << std::endl;
        std::cout << std::endl;
        std::cout << "File mode options:" << std::endl;
        std::cout << "  <video_file>      Input video file" << std::endl;
        std::cout << std::endl;
        std::cout << "Camera mode options:" << std::endl;
        std::cout << "  --camera DEV      V4L2 camera device (e.g. /dev/video0)" << std::endl;
        std::cout << "  --resolution WxH  Capture resolution (default: 640x480)" << std::endl;
        std::cout << "  --fps N           Capture framerate (default: 15)" << std::endl;
        std::cout << "  --duration N      Capture duration in seconds (0=unlimited)" << std::endl;
        std::cout << std::endl;
        std::cout << "Common options:" << std::endl;
        std::cout << "  --ffmpeg PATH    FFmpeg path (default: auto-detect)" << std::endl;
        std::cout << "  --ip IP          Receiver IP (default: 127.0.0.1)" << std::endl;
        std::cout << "  --codec CODEC    Video codec (default: libx264)" << std::endl;
        std::cout << "                   ARM dev board: try libopenh264 or mpeg2video" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  " << argv[0] << " ~/Videos/test.mp4" << std::endl;
        std::cout << "  " << argv[0] << " --camera /dev/video0 --duration 30 --codec mpeg2video" << std::endl;
        std::cout << "  " << argv[0] << " --camera /dev/video0 --resolution 1280x720 --fps 10 --codec mpeg2video" << std::endl;
        return 1;
    }

    std::string video_file = "";
    std::string ffmpeg_path = "";
    std::string target_ip = "127.0.0.1";
    std::string codec = "libx264";
    bool use_camera = false;
    std::string camera_device = "/dev/video0";
    std::string resolution = "640x480";
    int fps = 15;
    int duration_sec = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--camera" && i + 1 < argc) {
            use_camera = true;
            camera_device = argv[++i];
        } else if (arg == "--resolution" && i + 1 < argc) {
            resolution = argv[++i];
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if (arg == "--ffmpeg" && i + 1 < argc) {
            ffmpeg_path = argv[++i];
        } else if (arg == "--ip" && i + 1 < argc) {
            target_ip = argv[++i];
        } else if (arg == "--codec" && i + 1 < argc) {
            codec = argv[++i];
        } else if (arg.find("--") != 0) {
            video_file = arg;
        }
    }

    // 验证参数
    if (!use_camera && video_file.empty()) {
        std::cerr << "[ERROR] Either <video_file> or --camera is required." << std::endl;
        return 1;
    }
    if (use_camera) {
        video_file = camera_device;  // 将设备路径作为输入
    }

    // 注册信号处理
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    try {
        FFmpegSender sender(ffmpeg_path, target_ip, codec,
                            use_camera, camera_device, resolution, fps, duration_sec);
        g_sender = &sender;
        sender.sendStream(video_file);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}