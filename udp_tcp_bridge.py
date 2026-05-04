#!/usr/bin/env python3
"""
UDP↔TCP Bridge for ADB Port Forwarding
Run on the DEV BOARD (100ask imx6ull)

Architecture:
  DATA: ffmpeg_sender(UDP:1234) → bridge(recv UDP → framed TCP server :1236)
    → ADB forward tcp:1234→tcp:1236 → host bridge(deframe → UDP:1234)
    → ffmpeg_receiver

  FEEDBACK: ffmpeg_receiver(UDP:1235) → host bridge(UDP→TCP connect :1235)
    → ADB forward tcp:1235→tcp:1235 → bridge(TCP server :1235 → UDP:1235)
    → ffmpeg_sender
"""
import socket
import threading
import struct
import time
import select
import traceback

TCP_DATA_PORT = 1236
TCP_FEEDBACK_PORT = 1235
UDP_DATA_PORT = 1234
UDP_FEEDBACK_PORT = 1235

MAX_BUFFERED_PKTS = 5000  # UDP 缓冲上限 ~5K packets (~7MB for 1400B MTU)
STATUS_INTERVAL = 500     # 日志统计间隔


def log(fmt, *args):
    """带时间戳的统一日志"""
    ts = time.strftime("%H:%M:%S")
    msg = fmt % args if args else fmt
    print(f"[{ts}] {msg}", flush=True)


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        except:
            return None
    return buf


def data_channel():
    """UDP:1234 → framed TCP :1236 (ADB forward target)"""
    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind(('127.0.0.1', TCP_DATA_PORT))
    tcp_server.listen(5)
    tcp_server.setblocking(False)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(('127.0.0.1', UDP_DATA_PORT))
    udp_sock.setblocking(False)

    log("[DATA] UDP:%d → TCP server :%d", UDP_DATA_PORT, TCP_DATA_PORT)
    log("[DATA] Waiting for ADB forward connection from host...")

    tcp_conn = None
    udp_buffer = []
    pending_queue = []   # packets awaiting TCP retry after BlockingIOError
    total_pkts = 0

    while True:
        rlist = [tcp_server]
        if tcp_conn:
            rlist.append(tcp_conn)

        try:
            readable, _, errlist = select.select(rlist, [], rlist, 1.0)
        except:
            continue

        # Check for errors on TCP connection
        for s in errlist:
            if s is tcp_conn:
                log("[DATA] ADB tunnel error, reconnecting")
                try:
                    tcp_conn.close()
                except:
                    pass
                tcp_conn = None
                # Move pending retries back to buffer for reconnection
                pending_queue.clear()

        # Accept new TCP connection
        if tcp_server in readable:
            try:
                conn, addr = tcp_server.accept()
                log("[DATA] ADB tunnel connected from %s:%d", addr[0], addr[1])
                if tcp_conn:
                    try:
                        tcp_conn.close()
                    except:
                        pass
                tcp_conn = conn
                tcp_conn.setblocking(False)

                # Flush buffered packets (limit per burst to avoid TCP buffer overflow)
                flush_count = 0
                for buf_pkt in udp_buffer:
                    try:
                        frame = struct.pack('>I', len(buf_pkt)) + buf_pkt
                        tcp_conn.sendall(frame)
                        flush_count += 1
                    except Exception as e:
                        log("[DATA] Flush failed after %d packets: %s", flush_count, e)
                        break
                if flush_count > 0:
                    log("[DATA] Flushed %d buffered packets + %d new = %d total",
                        flush_count, total_pkts, flush_count + total_pkts)
                udp_buffer.clear()
                pending_queue.clear()  # stale retries from old connection
                total_pkts = 0
            except Exception as e:
                log("[DATA] Accept error: %s", e)

        # Read all available UDP packets
        while True:
            try:
                data, addr = udp_sock.recvfrom(65535)
            except BlockingIOError:
                break  # No more UDP data

            if not data:
                continue

            if tcp_conn:
                try:
                    frame = struct.pack('>I', len(data)) + data
                    # sendall wrapper for non-blocking socket
                    total_sent = 0
                    while total_sent < len(frame):
                        sent = tcp_conn.send(frame[total_sent:])
                        if sent <= 0:
                            raise OSError("send returned %d" % sent)
                        total_sent += sent
                    total_pkts += 1
                    if total_pkts % STATUS_INTERVAL == 0:
                        log("[DATA] %d pkts sent via ADB", total_pkts)
                except (BlockingIOError):
                    # TCP send buffer full — enqueue for retry, don't drop
                    pending_queue.append(data)
                    if len(pending_queue) > MAX_BUFFERED_PKTS:
                        dropped = len(pending_queue) - MAX_BUFFERED_PKTS
                        pending_queue = pending_queue[-MAX_BUFFERED_PKTS:]
                        log("[DATA] Pending overflow! Dropped %d oldest packets", dropped)
                except (BrokenPipeError, ConnectionResetError, OSError) as e:
                    log("[DATA] ADB tunnel lost (send error: %s), buffering...", e)
                    tcp_conn = None
                    pending_queue.clear()
                    udp_buffer.append(data)
                    if len(udp_buffer) > MAX_BUFFERED_PKTS:
                        dropped = len(udp_buffer) - MAX_BUFFERED_PKTS
                        udp_buffer = udp_buffer[-MAX_BUFFERED_PKTS:]
                        log("[DATA] Buffer overflow! Dropped %d oldest packets", dropped)
            else:
                udp_buffer.append(data)
                if len(udp_buffer) > MAX_BUFFERED_PKTS:
                    dropped = len(udp_buffer) - MAX_BUFFERED_PKTS
                    udp_buffer = udp_buffer[-MAX_BUFFERED_PKTS:]
                    log("[DATA] Buffer overflow! Dropped %d oldest packets", dropped)
                if len(udp_buffer) % (STATUS_INTERVAL // 10) == 0:
                    log("[DATA] %d pkts buffered (no TCP conn)", len(udp_buffer))

        # Retry pending packets from previous BlockingIOError drops
        if tcp_conn and pending_queue:
            retry_count = 0
            for data in list(pending_queue):
                try:
                    frame = struct.pack('>I', len(data)) + data
                    total_sent = 0
                    while total_sent < len(frame):
                        sent = tcp_conn.send(frame[total_sent:])
                        if sent <= 0:
                            raise OSError("send returned %d" % sent)
                        total_sent += sent
                    total_pkts += 1
                    pending_queue.remove(data)
                    retry_count += 1
                except (BlockingIOError):
                    break  # Still full, retry next loop
                except Exception:
                    break  # Connection broken, will be handled below
            if retry_count:
                log("[DATA] Retried %d pending packets (%d remain)", retry_count, len(pending_queue))

        # Check TCP connection health
        if tcp_conn and tcp_conn in readable:
            try:
                chunk = tcp_conn.recv(4096)
                if not chunk:
                    log("[DATA] ADB tunnel closed by peer")
                    tcp_conn = None
            except BlockingIOError:
                pass  # alive but no data
            except Exception as e:
                log("[DATA] ADB tunnel lost: %s", e)
                tcp_conn = None


def feedback_channel():
    """TCP server :1235 → UDP:1235"""
    tcp_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_server.bind(('127.0.0.1', TCP_FEEDBACK_PORT))
    tcp_server.listen(5)
    tcp_server.settimeout(3.0)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    log("[FEEDBACK] TCP server :%d → UDP:%d", TCP_FEEDBACK_PORT, UDP_FEEDBACK_PORT)

    while True:
        try:
            conn, addr = tcp_server.accept()
            log("[FEEDBACK] ADB tunnel connected from %s:%d", addr[0], addr[1])
            conn.settimeout(10.0)
            total_fb = 0
            while True:
                try:
                    data = conn.recv(65535)
                    if not data:
                        break
                    udp_sock.sendto(data, ('127.0.0.1', UDP_FEEDBACK_PORT))
                    total_fb += 1
                except socket.timeout:
                    continue
                except:
                    break
            log("[FEEDBACK] %d feedback packets forwarded, connection closed", total_fb)
            conn.close()
        except socket.timeout:
            continue
        except Exception as e:
            log("[FEEDBACK] error: %s", e)
            traceback.print_exc()
            time.sleep(1)


def main():
    print("=" * 55)
    print(" UDP↔TCP Bridge v5 - Buffered + Timestamps + Traceback")
    print("=" * 55)
    print()

    t1 = threading.Thread(target=data_channel, daemon=True)
    t2 = threading.Thread(target=feedback_channel, daemon=True)
    t1.start()
    t2.start()

    print("Bridge running. Press Ctrl+C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping...")

if __name__ == "__main__":
    main()