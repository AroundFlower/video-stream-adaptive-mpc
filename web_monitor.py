#!/usr/bin/env python3
"""
Web 监控界面 — 实时展示视频传输统计信息
启动后自动监听 UDP:1237 接收统计，HTTP:8080 提供仪表盘界面

用法:
    python3 web_monitor.py              # 默认端口 8080
    python3 web_monitor.py --port 9090  # 自定义HTTP端口
"""

import socket
import threading
import json
import time
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

# === 配置 ===
HTTP_PORT = 8080
UDP_STATS_PORT = 1237

# === 全局状态（线程安全） ===
stats_lock = threading.Lock()
latest_stats = {
    "packets": 0,
    "bytes": 0,
    "kbps": 0,
    "loss": 0.0,
    "first_seq": 0,
    "last_seq": 0,
    "holes": 0,
    "elapsed_ms": 0
}
last_update_time = 0  # 上次收到统计的时间戳


# ================================================================
# HTML 仪表盘页面（内嵌，无外部依赖）
# ================================================================
DASHBOARD_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>视频传输监控</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: 'SF Mono', 'Consolas', 'Monaco', monospace;
    background: #0d1117; color: #c9d1d9;
    min-height: 100vh; padding: 20px;
  }
  .header {
    display: flex; align-items: center; justify-content: space-between;
    margin-bottom: 24px; padding-bottom: 16px;
    border-bottom: 1px solid #21262d;
  }
  .header h1 { font-size: 20px; color: #58a6ff; }
  .status {
    display: flex; align-items: center; gap: 8px;
    font-size: 13px; color: #8b949e;
  }
  .status-dot {
    width: 10px; height: 10px; border-radius: 50%;
    background: #f85149; /* 默认红色=无数据 */
    box-shadow: 0 0 6px currentColor;
  }
  .status-dot.live { background: #3fb950; }
  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 16px; margin-bottom: 24px;
  }
  .card {
    background: #161b22; border: 1px solid #21262d;
    border-radius: 8px; padding: 16px;
  }
  .card .label { font-size: 12px; color: #8b949e; margin-bottom: 6px; }
  .card .value { font-size: 28px; font-weight: bold; }
  .card .unit  { font-size: 14px; color: #8b949e; margin-left: 4px; }
  .card.packets .value { color: #58a6ff; }
  .card.data    .value { color: #a371f7; }
  .card.bitrate .value { color: #f0883e; }
  .card.loss    .value { color: #3fb950; }
  .card.loss.warn  .value { color: #d2991d; }
  .card.loss.danger .value { color: #f85149; }
  .card.time    .value { color: #8b949e; }
  .detail {
    background: #161b22; border: 1px solid #21262d;
    border-radius: 8px; padding: 16px;
  }
  .detail h2 { font-size: 14px; color: #8b949e; margin-bottom: 12px; }
  .detail-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
    gap: 12px;
  }
  .detail-item .label { font-size: 11px; color: #484f58; }
  .detail-item .value { font-size: 16px; font-weight: bold; color: #c9d1d9; }
  .footer {
    margin-top: 24px; font-size: 11px; color: #484f58;
    text-align: center;
  }
  @media (max-width: 500px) {
    body { padding: 12px; }
    .cards { grid-template-columns: repeat(2, 1fr); gap: 8px; }
    .card .value { font-size: 22px; }
  }
</style>
</head>
<body>

<div class="header">
  <h1>视频传输监控</h1>
  <div class="status">
    <div class="status-dot" id="statusDot"></div>
    <span id="statusText">等待数据...</span>
  </div>
</div>

<div class="cards">
  <div class="card packets">
    <div class="label">接收包数</div>
    <span class="value" id="packets">0</span>
  </div>
  <div class="card data">
    <div class="label">数据量</div>
    <span class="value" id="dataKB">0</span><span class="unit">KB</span>
  </div>
  <div class="card bitrate">
    <div class="label">平均码率</div>
    <span class="value" id="kbps">0</span><span class="unit">Kbps</span>
  </div>
  <div class="card loss" id="lossCard">
    <div class="label">丢包率</div>
    <span class="value" id="loss">0.0</span><span class="unit">%</span>
  </div>
  <div class="card time">
    <div class="label">传输耗时</div>
    <span class="value" id="elapsed">0</span><span class="unit">s</span>
  </div>
</div>

<div class="detail">
  <h2>序列号详情</h2>
  <div class="detail-grid">
    <div class="detail-item">
      <div class="label">首个序列号</div>
      <div class="value" id="firstSeq">-</div>
    </div>
    <div class="detail-item">
      <div class="label">最后序列号</div>
      <div class="value" id="lastSeq">-</div>
    </div>
    <div class="detail-item">
      <div class="label">空洞数</div>
      <div class="value" id="holes">-</div>
    </div>
    <div class="detail-item">
      <div class="label">预期包数</div>
      <div class="value" id="expected">-</div>
    </div>
  </div>
</div>

<div class="footer" id="footer">自动刷新中 · 每秒更新</div>

<script>
// === 自动刷新逻辑 ===
var lastUpdateTs = 0;
var staleTimer = null;

function update() {
  fetch('/api/stats')
    .then(function(r) { return r.json(); })
    .then(function(s) {
      // 更新卡片值
      document.getElementById('packets').textContent = s.packets;
      document.getElementById('dataKB').textContent = Math.round(s.bytes / 1024);
      document.getElementById('kbps').textContent = s.kbps;
      document.getElementById('loss').textContent = s.loss.toFixed(1);
      document.getElementById('elapsed').textContent = (s.elapsed_ms / 1000).toFixed(1);

      // 序列号详情
      if (s.first_seq > 0) {
        document.getElementById('firstSeq').textContent = s.first_seq;
        document.getElementById('lastSeq').textContent = s.last_seq;
        document.getElementById('holes').textContent = s.holes;
        document.getElementById('expected').textContent = s.last_seq - s.first_seq + 1;
      }

      // 丢包率颜色编码
      var lossCard = document.getElementById('lossCard');
      lossCard.classList.remove('warn', 'danger');
      if (s.loss > 3.0) {
        lossCard.classList.add('danger');
      } else if (s.loss > 0.5) {
        lossCard.classList.add('warn');
      }

      // 连接状态指示器
      document.getElementById('statusDot').classList.add('live');
      document.getElementById('statusText').textContent = '接收中';
      lastUpdateTs = Date.now();

      // 清除超时计时器
      if (staleTimer) { clearTimeout(staleTimer); staleTimer = null; }
      // 3秒内无新数据则标记离线
      staleTimer = setTimeout(function() {
        document.getElementById('statusDot').classList.remove('live');
        document.getElementById('statusText').textContent = '无数据 (传输可能已结束)';
      }, 3000);
    })
    .catch(function() {
      document.getElementById('statusDot').classList.remove('live');
      document.getElementById('statusText').textContent = '连接失败';
    });
}

// 每秒刷新
setInterval(update, 1000);
update();  // 立即首次加载
</script>

</body>
</html>"""


# ================================================================
# UDP 统计监听线程
# ================================================================
def udp_listener():
    """监听 UDP:1237，接收 receiver 发来的 JSON 统计"""
    global latest_stats, last_update_time

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', UDP_STATS_PORT))
    sock.settimeout(2.0)  # 2秒超时，允许线程响应退出

    print(f"[Monitor] UDP 监听 :{UDP_STATS_PORT} (等待 receiver 统计...)")

    while True:
        try:
            data, addr = sock.recvfrom(2048)
            if not data:
                continue
            stats = json.loads(data.decode('utf-8'))
            with stats_lock:
                latest_stats.update(stats)
                last_update_time = time.time()
        except socket.timeout:
            continue
        except json.JSONDecodeError:
            continue
        except Exception as e:
            print(f"[Monitor] UDP 接收异常: {e}")
            continue


# ================================================================
# HTTP API Handler
# ================================================================
class MonitorHandler(BaseHTTPRequestHandler):
    """处理 HTTP 请求：/ 返回仪表盘，/api/stats 返回 JSON"""

    def do_GET(self):
        if self.path == '/api/stats':
            # API: 返回最新统计 JSON
            with stats_lock:
                body = json.dumps(latest_stats).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(body)

        elif self.path == '/' or self.path == '/index.html':
            # 仪表盘 HTML
            body = DASHBOARD_HTML.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        """抑制 HTTP 请求日志（UDP 统计已包含状态）"""
        pass


# ================================================================
# 主入口
# ================================================================
def main():
    global HTTP_PORT

    # 解析命令行参数
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == '--port' and i + 1 < len(args):
            HTTP_PORT = int(args[i + 1])
            i += 2
        else:
            i += 1

    print("=" * 55)
    print(" Web 视频传输监控")
    print("=" * 55)
    print(f" HTTP 仪表盘: http://0.0.0.0:{HTTP_PORT}")
    print(f" UDP 统计源: 127.0.0.1:{UDP_STATS_PORT}")
    print("=" * 55)
    print()

    # 启动 UDP 监听线程
    t_udp = threading.Thread(target=udp_listener, daemon=True)
    t_udp.start()

    # 启动 HTTP 服务（主线程阻塞）
    server = HTTPServer(('0.0.0.0', HTTP_PORT), MonitorHandler)
    try:
        print(f"[Monitor] HTTP 服务已启动，浏览器打开 http://localhost:{HTTP_PORT}")
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Monitor] 正在关闭...")
        server.shutdown()


if __name__ == "__main__":
    main()
