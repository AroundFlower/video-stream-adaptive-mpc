#include <iostream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <signal.h>
#include <algorithm>
#include <set>

class FFmpegReceiver {
private:
    int data_sockfd;
    int feedback_sockfd;
    int stats_sockfd;             // Web监控统计socket
    const int DATA_PORT = 1234;
    const int FEEDBACK_PORT = 1235;
    const int STATS_PORT = 1237;  // Web监控统计端口
    const int BUFFER_SIZE = 2048;

    // 统计
    std::atomic<int> packet_count{0};
    std::atomic<long long> total_bytes{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point first_data_time; // 收到首个包的时间
    int recv_buffer_size = 4 * 1024 * 1024; // 4MB接收缓冲

    // 序列号跟踪（用于精确丢包率）
    uint32_t first_seq;             // 首次收到的序列号
    uint32_t last_seq;              // 最后收到的序列号
    uint32_t total_holes;           // 窗口内总空洞数（缺失包数）
    bool has_first_seq;
    std::set<uint32_t> hole_set;    // 记录已经确认的序列号空洞（用于去重）

    // 丢包检测
    float smoothed_loss_rate;
    std::ofstream output_file;
    std::atomic<bool> running{true};
    std::string feedback_ip;

public:
    FFmpegReceiver(const std::string& ip = "127.0.0.1") : feedback_ip(ip) {
        resetSequenceTracking();

        // 数据socket
        data_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(data_sockfd, SOL_SOCKET, SO_RCVBUF,
                   &recv_buffer_size, sizeof(recv_buffer_size));

        // 增大接收超时
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(data_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in data_addr;
        memset(&data_addr, 0, sizeof(data_addr));
        data_addr.sin_family = AF_INET;
        data_addr.sin_port = htons(DATA_PORT);
        data_addr.sin_addr.s_addr = INADDR_ANY;
        bind(data_sockfd, (struct sockaddr*)&data_addr, sizeof(data_addr));

        // 反馈socket
        feedback_sockfd = socket(AF_INET, SOCK_DGRAM, 0);

        // Web监控统计socket
        stats_sockfd = socket(AF_INET, SOCK_DGRAM, 0);

        start_time = std::chrono::steady_clock::now();
        first_data_time = start_time;  // 默认值，收到首个包后更新

        std::cout << "=== FFmpeg TS Stream Receiver (SeqLoss v3) ===" << std::endl;
        std::cout << "Listening on UDP port " << DATA_PORT << std::endl;
        std::cout << "Buffer: " << (recv_buffer_size/1024/1024) << "MB" << std::endl;
        std::cout << "Loss detection: sequence-number based" << std::endl;
        std::cout << "Stats output: UDP port " << STATS_PORT << " (web monitor)" << std::endl;
        std::cout << "===========================================" << std::endl;
    }

    ~FFmpegReceiver() {
        if (output_file.is_open()) {
            output_file.close();
        }
        close(data_sockfd);
        close(feedback_sockfd);
        close(stats_sockfd);
        printStats();
    }

    void resetSequenceTracking() {
        first_seq = 0;
        last_seq = 0;
        total_holes = 0;
        has_first_seq = false;
        hole_set.clear();
        smoothed_loss_rate = 0.0f;
    }

    // 更新序列号跟踪，计算新空洞
    // 返回当前窗口内的丢包数
    void updateSequenceTracking(uint32_t this_seq) {
        if (!has_first_seq) {
            first_seq = this_seq;
            last_seq = this_seq;
            has_first_seq = true;
            return;
        }

        if (this_seq > last_seq) {
            // 检查 [last_seq+1, this_seq-1] 范围内的空洞
            for (uint32_t missing = last_seq + 1; missing < this_seq; missing++) {
                if (hole_set.find(missing) == hole_set.end()) {
                    hole_set.insert(missing);
                    total_holes++;
                }
            }
            last_seq = this_seq;

            // 限制 hole_set 大小，避免无限增长
            if (hole_set.size() > 50000) {
                // 清理 old holes，只保留最近的窗口
                std::set<uint32_t> new_holes;
                uint32_t window_start = (last_seq > 30000) ? last_seq - 30000 : 0;
                for (auto it = hole_set.lower_bound(window_start);
                     it != hole_set.end(); ++it) {
                    new_holes.insert(*it);
                }
                size_t removed = hole_set.size() - new_holes.size();
                hole_set.swap(new_holes);
                // 减少 total_holes 以匹配删掉的部分
                total_holes = std::max((uint32_t)0,
                    total_holes - (uint32_t)std::min(removed, (size_t)total_holes));
            }
        } else if (this_seq < last_seq) {
            // 这可能是一个重传的旧包，或者序列号回绕
            // 如果是已知空洞，移除它并减少空洞计数
            auto it = hole_set.find(this_seq);
            if (it != hole_set.end()) {
                hole_set.erase(it);
                if (total_holes > 0) total_holes--;
            }
        }
        // this_seq == last_seq：重复包，忽略
    }

    // 基于序列号空洞计算丢包率
    float computeLossRate() {
        if (!has_first_seq || total_packets_sent() == 0) {
            return 0.0f;
        }

        // 窗口内总预期包数 = last - first + 1
        uint32_t expected_total = (last_seq - first_seq + 1);
        if (expected_total > 2) {
            // 去掉第一个和最后一个（可能窗口不完全稳定），仅用内部空洞计算
            // loss = holes / expected
            float loss = (float)total_holes * 100.0f / std::max(1U, expected_total);

            // 上限60%
            return std::min(loss, 60.0f);
        }
        return 0.0f;
    }

    // 总收到过的序列号数量（last - first - holes + 1，不含空洞的）
    int total_packets_sent() {
        if (!has_first_seq) return 0;
        return packet_count.load();
    }

    // 发送丢包率反馈
    void sendFeedback(float loss_rate) {
        struct sockaddr_in sender_addr;
        memset(&sender_addr, 0, sizeof(sender_addr));
        sender_addr.sin_family = AF_INET;
        sender_addr.sin_port = htons(FEEDBACK_PORT);
        sender_addr.sin_addr.s_addr = inet_addr(feedback_ip.c_str());

        char feedback[64];
        snprintf(feedback, sizeof(feedback), "LOSS:%.1f", loss_rate);
        sendto(feedback_sockfd, feedback, strlen(feedback), 0,
               (struct sockaddr*)&sender_addr, sizeof(sender_addr));
    }

    // 发送JSON统计到Web监控 (UDP:1237)
    void sendStats(long long elapsed_ms) {
        struct sockaddr_in stats_addr;
        memset(&stats_addr, 0, sizeof(stats_addr));
        stats_addr.sin_family = AF_INET;
        stats_addr.sin_port = htons(STATS_PORT);
        stats_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        int kbps = total_bytes.load() * 8 * 1000 / std::max(1LL, elapsed_ms) / 1000;

        char json[256];
        snprintf(json, sizeof(json),
            "{\"packets\":%d,\"bytes\":%lld,\"kbps\":%d,\"loss\":%.1f,"
            "\"first_seq\":%u,\"last_seq\":%u,\"holes\":%u,\"elapsed_ms\":%lld}",
            packet_count.load(), total_bytes.load(), kbps, smoothed_loss_rate,
            first_seq, last_seq, total_holes, elapsed_ms);
        sendto(stats_sockfd, json, strlen(json), 0,
               (struct sockaddr*)&stats_addr, sizeof(stats_addr));
    }

    void startReceiving(const std::string& filename) {
        output_file.open(filename, std::ios::binary);
        if (!output_file.is_open()) {
            throw std::runtime_error("Cannot create output file");
        }

        std::cout << "Saving to: " << filename << std::endl;
        std::cout << "Press 'q' and Enter to stop...\n" << std::endl;

        char buffer[BUFFER_SIZE];
        struct sockaddr_in sender_addr;
        socklen_t addr_len = sizeof(sender_addr);

        auto last_feedback = start_time;
        auto last_data_time = start_time;
        bool data_received_ever = false;

        while (running) {
            int bytes = recvfrom(data_sockfd, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&sender_addr, &addr_len);

            if (bytes > 0) {
                if (bytes < 4 + 1) {
                    // 包太小（小于4字节序列号+1字节数据），忽略
                    continue;
                }

                // 解析前4字节：大端序列号
                uint32_t seq = 0;
                memcpy(&seq, buffer, 4);
                seq = __builtin_bswap32(seq);  // 网络序→主机序

                // 写入文件（不含序列号头）
                output_file.write(buffer + 4, bytes - 4);
                total_bytes += (bytes - 4);
                packet_count++;

                if (!data_received_ever) {
                    data_received_ever = true;
                    first_data_time = std::chrono::steady_clock::now();
                }
                last_data_time = std::chrono::steady_clock::now();

                // 更新序列号跟踪
                updateSequenceTracking(seq);

                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - first_data_time).count();
                auto fb_interval = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_feedback).count();

                // 每2秒反馈一次
                if (fb_interval >= 2000 && packet_count > 0) {
                    // 基于序列号的精确丢包率
                    float raw_loss = computeLossRate();

                    // 如果没有序列号信息（理论上不可能，但安全），回退到启发式
                    if (!has_first_seq) {
                        if (packet_count < 5) raw_loss = 30.0f;
                        else if (packet_count < 15) raw_loss = 10.0f;
                        else raw_loss = 0.0f;
                    }

                    // 指数平滑，防止突变
                    smoothed_loss_rate = smoothed_loss_rate * 0.7f + raw_loss * 0.3f;

                    sendFeedback(smoothed_loss_rate);
                    sendStats(elapsed);     // 同步发送JSON统计到Web监控
                    last_feedback = now;

                    // 显示状态
                    int speed_kbps = total_bytes.load() * 8 * 1000 / std::max(1L, (long)elapsed) / 1000;
                    printf("\r[Status] Pkts:%d | %.0fKB | %dKbps | Loss:%.1f%% | Seq:%u-%u Holes:%u | %.1fs",
                           packet_count.load(), total_bytes.load()/1024.0,
                           speed_kbps, smoothed_loss_rate,
                           first_seq, last_seq, total_holes, elapsed/1000.0);
                    fflush(stdout);
                }
            } else {
                // recvfrom超时 - 没有数据到达
                if (!data_received_ever) {
                    // 还没收到任何数据，继续等待
                    continue;
                }

                auto now = std::chrono::steady_clock::now();
                auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_data_time).count();

                // 如果超过5秒没有任何数据到达(且曾经有数据)，判定传输完成
                if (idle > 5000 && packet_count > 0) {
                    std::cout << "\n[Stream] Idle timeout (5s) - transfer complete" << std::endl;
                    std::cout << "  Last seq: " << last_seq << ", holes: " << total_holes << std::endl;
                    break;
                }
            }
        }

        // 结束前再发一次最终反馈 + 统计
        if (packet_count > 0) {
            float final_loss = computeLossRate();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - first_data_time).count();
            sendFeedback(final_loss);
            sendStats(elapsed);
            std::cout << "  Final loss feedback sent: " << final_loss << "%" << std::endl;
        }
    }

    void printStats() {
        auto end = std::chrono::steady_clock::now();
        auto total = std::chrono::duration_cast<std::chrono::milliseconds>(end - first_data_time).count();

        std::cout << "\n\n=== Receiver Statistics ===" << std::endl;
        std::cout << "Packets received: " << packet_count.load() << std::endl;
        std::cout << "Total data: " << (total_bytes.load() / 1024) << " KB" << std::endl;
        std::cout << "Total time: " << total << " ms" << std::endl;

        if (has_first_seq && packet_count > 0) {
            uint32_t expected = last_seq - first_seq + 1;
            float actual_loss = (float)total_holes * 100.0f / std::max(1U, expected);
            std::cout << "First seq: " << first_seq << std::endl;
            std::cout << "Last seq: " << last_seq << std::endl;
            std::cout << "Expected packets (seq range): " << expected << std::endl;
            std::cout << "Seq holes (lost): " << total_holes << std::endl;
            std::cout << "Actual loss rate: " << actual_loss << "%" << std::endl;
        }

        if (total > 0 && packet_count > 0) {
            float avg_speed = (total_bytes.load() * 1000.0 / total) * 8 / 1000;
            std::cout << "Average bitrate: " << avg_speed << " Kbps" << std::endl;
            std::cout << "Average speed: " << (total_bytes.load() * 1000.0 / total / 1024) << " KB/s" << std::endl;
        }
        std::cout << "Smoothed loss rate: " << smoothed_loss_rate << "%" << std::endl;
    }

    void stop() {
        running = false;
    }
};

// Ctrl+C处理
FFmpegReceiver* global_receiver = nullptr;
void signalHandler(int) {
    std::cout << "\n[User] Stopping..." << std::endl;
    if (global_receiver) global_receiver->stop();
}

int main(int argc, char* argv[]) {
    std::string outfile = "ffmpeg_stream.ts";
    std::string feedback_ip = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc) {
            feedback_ip = argv[++i];
        } else if (arg.find("--") != 0) {
            outfile = arg;
        }
    }

    std::cout << "Feedback target: " << feedback_ip << std::endl;

    try {
        FFmpegReceiver receiver(feedback_ip);
        global_receiver = &receiver;
        signal(SIGINT, signalHandler);

        receiver.startReceiving(outfile);

        std::cout << "\nOutput file: " << outfile << std::endl;
        std::cout << "Run 'ffprobe " << outfile << "' to verify." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}