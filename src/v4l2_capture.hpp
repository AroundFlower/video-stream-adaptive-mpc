#ifndef V4L2_CAPTURE_HPP
#define V4L2_CAPTURE_HPP

// Native Linux V4L2 camera capture via mmap + dedicated capture thread
//
// ARM V4L2 drivers often ignore O_NONBLOCK on VIDIOC_DQBUF (kernel bug).
// Workaround: a background thread blocks in DQBUF; the main thread polls
// an atomic flag without ever touching the V4L2 fd.
//
// Lifecycle: open() → start() → [dequeue()/enqueue() loop] → stop() → release()

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <thread>
#include <atomic>

class V4L2Capture {
public:
    V4L2Capture()
        : fd_(-1), width_(0), height_(0), fps_(0),
          pixelformat_(0), buf_count_(0), buffers_(nullptr), streaming_(false),
          capture_running_(false), frame_ready_(false),
          pending_data_(nullptr), pending_len_(0), pending_idx_(-1) {}

    ~V4L2Capture() { release(); }

    bool open(const char* device, int width, int height, int fps) {
        width_ = width;
        height_ = height;
        fps_ = fps;

        // Open blocking — the capture thread will wait in DQBUF
        fd_ = ::open(device, O_RDWR);
        if (fd_ < 0) {
            fprintf(stderr, "[V4L2] Cannot open %s: %s\n", device, strerror(errno));
            return false;
        }

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
            fprintf(stderr, "[V4L2] VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
            return false;
        }
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            fprintf(stderr, "[V4L2] %s: not a capture device\n", device);
            return false;
        }
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            fprintf(stderr, "[V4L2] %s: mmap streaming not supported\n", device);
            return false;
        }

        // Set pixel format: YUYV 4:2:2
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "[V4L2] S_FMT YUYV failed: %s\n", strerror(errno));
            return false;
        }
        width_  = fmt.fmt.pix.width;
        height_ = fmt.fmt.pix.height;
        pixelformat_ = fmt.fmt.pix.pixelformat;
        printf("[V4L2] %dx%d %c%c%c%c\n", width_, height_,
               (pixelformat_>>0)&0xFF, (pixelformat_>>8)&0xFF,
               (pixelformat_>>16)&0xFF, (pixelformat_>>24)&0xFF);

        // Set framerate (non-fatal)
        struct v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
            fprintf(stderr, "[V4L2] S_PARM warning: %s\n", strerror(errno));

        // Request mmap buffers
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            fprintf(stderr, "[V4L2] REQBUFS failed: %s\n", strerror(errno));
            return false;
        }
        buf_count_ = req.count;

        buffers_ = new Buffer[buf_count_];
        for (unsigned i = 0; i < buf_count_; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                fprintf(stderr, "[V4L2] QUERYBUF %d: %s\n", i, strerror(errno));
                return false;
            }
            buffers_[i].start = mmap(nullptr, buf.length,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     fd_, buf.m.offset);
            buffers_[i].length = buf.length;
            if (buffers_[i].start == MAP_FAILED) {
                fprintf(stderr, "[V4L2] mmap %d: %s\n", i, strerror(errno));
                return false;
            }
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                fprintf(stderr, "[V4L2] QBUF %d: %s\n", i, strerror(errno));
                return false;
            }
        }
        printf("[V4L2] %d buffers mmap'd\n", buf_count_);
        return true;
    }

    bool start() {
        if (streaming_) return true;

        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            fprintf(stderr, "[V4L2] STREAMON failed: %s\n", strerror(errno));
            return false;
        }
        streaming_ = true;
        printf("[V4L2] Streaming ON\n");

        // Launch capture thread — blocks in DQBUF, writes to atomic slot
        capture_running_ = true;
        capture_thread_ = std::thread(&V4L2Capture::captureLoop, this);
        return true;
    }

    void stop() {
        if (!streaming_) return;

        // 1. Signal capture thread to exit
        capture_running_ = false;

        // 2. STREAMOFF — this wakes any blocked DQBUF on Linux
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;

        // 3. Join capture thread
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        frame_ready_ = false;
    }

    // Non-blocking: returns true if a frame is ready.
    // Main thread never touches the V4L2 fd.
    bool dequeue(int* buf_idx, uint8_t** data_ptr, size_t* data_len) {
        if (!frame_ready_) return false;

        *buf_idx   = pending_idx_;
        *data_ptr  = pending_data_;
        *data_len  = pending_len_;

        frame_ready_ = false;  // consumed — capture thread may now write next frame
        return true;
    }

    bool enqueue(int buf_idx) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = buf_idx;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] QBUF %d: %s\n", buf_idx, strerror(errno));
            return false;
        }
        return true;
    }

    void release() {
        stop();
        if (buffers_) {
            for (unsigned i = 0; i < buf_count_; i++) {
                if (buffers_[i].start && buffers_[i].start != MAP_FAILED)
                    munmap(buffers_[i].start, buffers_[i].length);
            }
            delete[] buffers_;
            buffers_ = nullptr;
            buf_count_ = 0;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int width()      const { return width_; }
    int height()     const { return height_; }
    uint32_t pixelformat() const { return pixelformat_; }
    int fps()        const { return fps_; }
    bool is_open()   const { return fd_ >= 0; }

private:
    int fd_;
    int width_, height_, fps_;
    uint32_t pixelformat_;
    unsigned buf_count_;
    struct Buffer { void* start; size_t length; };
    Buffer* buffers_;
    bool streaming_;

    // Capture thread state
    std::thread capture_thread_;
    std::atomic<bool> capture_running_;

    // Single-slot frame handoff (capture thread → main thread)
    std::atomic<bool> frame_ready_;
    uint8_t* pending_data_;
    size_t   pending_len_;
    int      pending_idx_;

    // Runs in background thread — blocking DQBUF is OK here
    void captureLoop() {
        int frames = 0;
        while (capture_running_) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            // Blocking wait for next frame
            if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EINTR) continue;       // signal, check capture_running_
                if (errno == EIO || errno == ENODEV) break;  // stream stopped
                fprintf(stderr, "[V4L2] DQBUF error: %s\n", strerror(errno));
                break;
            }

            // Spin until main thread consumes previous frame
            while (frame_ready_ && capture_running_) {
                usleep(500);
            }
            if (!capture_running_) {
                // Re-queue buffer before exit
                ioctl(fd_, VIDIOC_QBUF, &buf);
                break;
            }

            pending_data_ = (uint8_t*)buffers_[buf.index].start;
            pending_len_  = buf.bytesused;
            pending_idx_  = buf.index;
            frame_ready_  = true;
            frames++;

            if (frames <= 5) {
                fprintf(stderr, "[V4L2] captured frame#%d (%u bytes)\n",
                        frames, buf.bytesused);
            }
        }
        fprintf(stderr, "[V4L2] Capture thread exiting (%d frames)\n", frames);
    }
};

#endif // V4L2_CAPTURE_HPP
