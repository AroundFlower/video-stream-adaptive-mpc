#!/usr/bin/env python3
"""
Host bridge - runs on DEVELOPMENT HOST
Reads framed TCP data from ADB forward → extracts raw UDP → sends to ffmpeg_receiver
Sends feedback UDP → TCP to ADB forward → device

Architecture:
  DATA: ADB forward tcp:1234→tcp:1236
    → host_bridge TCP:1234 [read framed] → UDP:1234 → ffmpeg_receiver

  FEEDBACK: ffmpeg_receiver UDP:1235
    → host_bridge UDP:1235 → TCP:1235 [send raw] → ADB forward tcp:1235→tcp:1235
"""
import socket
import threading
import signal
import sys
import subprocess
import struct
import time
import traceback

HOST_UDP_DATA = 1234          # 发送给 ffmpeg_receiver 的数据端口
HOST_UDP_FEEDBACK = 1235      # 来自 ffmpeg_receiver 的反馈端口
ADB_DATA_FORWARD_PORT = 1234  # ADB 数据通道转发端口 (host:1234 → device:1236)
ADB_FEEDBACK_FORWARD_PORT = 1235  # ADB 反馈通道转发端口 (host:1235 → device:1235)

MAX_BUFFERED_PKTS = 10000  # UDP 缓冲上限（防止内存耗尽）


def log(fmt, *args):
    """带时间戳的统一日志"""
    ts = time.strftime("%H:%M:%S")
    msg = fmt % args if args else fmt
    print(f"[{ts}] {msg}", flush=True)


def recv_exact(sock, n, timeout=30.0):
    """Read exactly n bytes from socket with timeout"""
    sock.settimeout(timeout)
    buf = b''
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        except socket.timeout:
            # 超时也是失败——连接可能已死
            return None
        except Exception:
            return None
    return buf


def data_channel():
    """Connect to ADB forward :1234, read framed data, send raw UDP to ffmpeg_receiver"""
    udp_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    total_pkts = 0
    wait_count = 0
    stale_retries = 0

    while True:
        try:
            tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tcp.settimeout(30.0)
            tcp.connect(('127.0.0.1', ADB_DATA_FORWARD_PORT))

            # Wait for first frame header (4 bytes) with blocking read
            # If ADB can't reach device:1236, connection will be closed immediately
            # If device bridge is ready, we'll get the first 4 bytes
            try:
                hdr = recv_exact(tcp, 4)
            except Exception:
                hdr = None

            if hdr is None:
                # ADB connection closed immediately - device bridge not ready yet
                tcp.close()
                wait_count += 1
                if wait_count % 10 == 0:
                    log("[DATA] Waiting for device bridge... (attempt %d)", wait_count)
                time.sleep(1)
                continue

            # Got first frame! Connection is live
            wait_count = 0
            stale_retries = 0
            log("[DATA] ADB tunnel LIVE - receiving data from device")

            while True:
                pkt_len = struct.unpack('>I', hdr)[0]
                if pkt_len > 65535 or pkt_len == 0:
                    # Invalid frame, try resync
                    log("[DATA] Invalid frame length %d, resyncing...", pkt_len)
                    hdr = recv_exact(tcp, 4)
                    if hdr is None:
                        break
                    continue

                pkt = recv_exact(tcp, pkt_len)
                if pkt is None:
                    break

                udp_out.sendto(pkt, ('127.0.0.1', HOST_UDP_DATA))
                total_pkts += 1
                if total_pkts % 100 == 0:
                    log("[DATA] %d pkts forwarded", total_pkts)

                hdr = recv_exact(tcp, 4)
                if hdr is None:
                    break

            log("[DATA] ADB tunnel closed (%d pkts transferred)", total_pkts)
            tcp.close()
            time.sleep(1)

        except ConnectionRefusedError:
            wait_count += 1
            if wait_count % 10 == 0:
                log("[DATA] ADB forward not ready yet... (attempt %d)", wait_count)
            time.sleep(1)
        except (OSError, EOFError) as e:
            wait_count += 1
            if wait_count % 10 == 0:
                log("[DATA] Connection error: %s, retrying...", e)
            time.sleep(1)
        except Exception as e:
            log("[DATA] Unexpected error: %s", e)
            traceback.print_exc()
            time.sleep(2)


def feedback_channel():
    """Listen UDP:1235 for feedback from ffmpeg_receiver, forward via persistent TCP to ADB"""
    udp_in = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_in.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_in.bind(('127.0.0.1', HOST_UDP_FEEDBACK))
    log("[FEEDBACK] Listening UDP:%d for receiver feedback", HOST_UDP_FEEDBACK)

    total_fwd = 0
    tcp = None
    idle_since = time.time()

    while True:
        try:
            data, addr = udp_in.recvfrom(65535)
        except Exception as e:
            log("[FEEDBACK] recvfrom error: %s", e)
            traceback.print_exc()
            continue

        # Persistent TCP connection — avoids close()-before-data-arrives race
        for retry in range(3):
            try:
                if tcp is None:
                    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    tcp.settimeout(30.0)
                    tcp.connect(('127.0.0.1', ADB_FEEDBACK_FORWARD_PORT))
                    log("[FEEDBACK] TCP connected to ADB forward :%d", ADB_FEEDBACK_FORWARD_PORT)

                tcp.sendall(data)
                total_fwd += 1
                idle_since = time.time()
                break
            except (BrokenPipeError, ConnectionResetError, OSError, socket.timeout) as e:
                if tcp:
                    try:
                        tcp.close()
                    except:
                        pass
                    tcp = None
                if retry < 2:
                    time.sleep(0.5)
                else:
                    log("[FEEDBACK] Send failed after 3 retries: %s", e)
            except Exception:
                traceback.print_exc()
                break

        # Keepalive: close idle connection after 60s
        if tcp and time.time() - idle_since > 60:
            try:
                tcp.close()
            except:
                pass
            tcp = None


def cleanup(sig, frame):
    log("Shutting down...")
    subprocess.run(['adb', 'forward', '--remove', 'tcp:1234'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['adb', 'forward', '--remove', 'tcp:1235'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.exit(0)


def setup_adb_forwards():
    """Setup ADB port forwards"""
    # Clean stale forwards first
    subprocess.run(['adb', 'forward', '--remove', 'tcp:1234'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['adb', 'forward', '--remove', 'tcp:1235'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Data: host:1234 → device:1236
    subprocess.run(['adb', 'forward', 'tcp:1234', 'tcp:1236'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log("[ADB] forward tcp:1234 → tcp:1236 (data)")

    # Feedback: host:1235 → device:1235
    subprocess.run(['adb', 'forward', 'tcp:1235', 'tcp:1235'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log("[ADB] forward tcp:1235 → tcp:1235 (feedback)")


if __name__ == "__main__":
    signal.signal(signal.SIGINT, cleanup)
    print("=" * 55)
    print(" Host Bridge v6 - Debug Logs + Retry + Traceback")
    print("=" * 55)
    print()

    setup_adb_forwards()
    print()

    t1 = threading.Thread(target=data_channel, daemon=True)
    t2 = threading.Thread(target=feedback_channel, daemon=True)
    t1.start()
    t2.start()

    print("Bridge running. Will retry until device bridge connects.")
    print("Press Ctrl+C to stop.")
    print()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        cleanup(None, None)