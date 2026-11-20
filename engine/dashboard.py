#!/usr/bin/env python3
"""Local RF Tomography dashboard and UDP CSI receiver (Python standard library only)."""
from __future__ import annotations

import argparse
import json
import math
import socket
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {
    "started": time.time(),
    "packets": 0,
    "malformed": 0,
    "last_packet": 0.0,
    "devices": {},
    "rssi": deque(maxlen=180),
    "activity": deque(maxlen=180),
    "csi": deque(maxlen=180),
    "events": deque(maxlen=30),
}
LOCK = threading.Lock()

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#090d16"><title>RF TOMOGRAPHY // LIVE</title>
<style>
:root{color-scheme:dark;--bg:#090d16;--panel:#111a29;--line:#26364b;--muted:#8292aa;--cyan:#56e0d0;--orange:#ff9d5c;--red:#ff667a}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 15% 0%,#162b3d 0,transparent 38%),var(--bg);color:#eef4ff;font:14px Inter,Segoe UI,Arial,sans-serif}
main{max-width:1440px;margin:auto;padding:28px}.top{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:24px;flex-wrap:wrap}
.brand{display:flex;align-items:center;gap:13px}.mark{width:44px;height:44px;border:1px solid #3c7180;border-radius:13px;display:grid;place-items:center;color:var(--cyan);font-size:22px;background:#102431}
h1{font-size:19px;letter-spacing:2px;margin:0}.sub{color:var(--muted);font-size:12px;margin-top:5px;letter-spacing:1px}
.status{border:1px solid var(--line);border-radius:30px;padding:9px 14px;color:var(--muted);display:flex;gap:9px;align-items:center}
.dot{width:8px;height:8px;border-radius:50%;background:#67758a}.dot.on{background:#52dfad;box-shadow:0 0 13px #52dfad}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}.card{background:linear-gradient(145deg,#141f30,#0f1724);border:1px solid var(--line);border-radius:16px;padding:18px;min-width:0}
.metric .label{color:var(--muted);font-size:11px;letter-spacing:1.2px;text-transform:uppercase}.metric .value{font-size:29px;font-weight:650;margin:12px 0 4px;font-variant-numeric:tabular-nums}.hint{color:var(--muted);font-size:11px}
.layout{display:grid;grid-template-columns:1.5fr 1fr;gap:14px;margin-top:14px}.wide{grid-column:1/-1}.cardhead{display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;gap:12px}.cardhead h2{font-size:12px;letter-spacing:1.4px;margin:0}.tag{font-size:10px;color:var(--cyan);border:1px solid #28515a;border-radius:20px;padding:5px 8px}
canvas{width:100%;height:230px;display:block}.wave canvas{height:260px}.empty{height:230px;display:grid;place-items:center;color:var(--muted);text-align:center;border:1px dashed #2a3b51;border-radius:10px;padding:20px;line-height:1.8}
table{width:100%;border-collapse:collapse;font-size:12px}td,th{text-align:left;padding:11px 7px;border-bottom:1px solid #243247}th{color:var(--muted);font-weight:500}
.footer{color:#66778f;font-size:11px;text-align:center;margin:18px 0}.notice{margin-top:14px;padding:12px 14px;border-radius:10px;background:#241d18;border:1px solid #5a3b29;color:#e7b994;font-size:12px;line-height:1.7}
@media(max-width:900px){.grid{grid-template-columns:repeat(2,1fr)}.layout{grid-template-columns:1fr}}@media(max-width:520px){main{padding:16px}.grid{gap:8px}.metric .value{font-size:23px}.card{padding:14px}}
</style></head><body><main>
<header class="top"><div class="brand"><div class="mark">⌁</div><div><h1>RF TOMOGRAPHY</h1><div class="sub">ESP32-S3 CSI · LOCAL LIVE MONITOR</div></div></div>
<div class="status"><span class="dot" id="dot"></span><span id="status">WAITING FOR CSI</span></div></header>
<section class="grid">
<div class="card metric"><div class="label">CSI packets</div><div class="value" id="packets">0</div><div class="hint">Total valid packets received</div></div>
<div class="card metric"><div class="label">Signal strength</div><div class="value" id="rssi">—</div><div class="hint">Latest RSSI · dBm</div></div>
<div class="card metric"><div class="label">RF change</div><div class="value" id="activity">—</div><div class="hint">Relative CSI amplitude variation</div></div>
<div class="card metric"><div class="label">Transmitters</div><div class="value" id="devices">0</div><div class="hint">Distinct source MAC addresses</div></div>
</section>
<section class="layout">
<div class="card wave"><div class="cardhead"><h2>LIVE CSI AMPLITUDE</h2><span class="tag">RAW MEASUREMENT</span></div><canvas id="wave"></canvas></div>
<div class="card"><div class="cardhead"><h2>RF CHANGE HISTORY</h2><span class="tag">RELATIVE · 0–100%</span></div><canvas id="activityChart"></canvas></div>
<div class="card"><div class="cardhead"><h2>CONNECTED TRANSMITTERS</h2><span class="tag">MAC / RSSI</span></div><div id="deviceTable" class="hint">No CSI packets received yet.</div></div>
<div class="card"><div class="cardhead"><h2>RECEIVER DIAGNOSTICS</h2><span class="tag">LIVE</span></div><table><tbody>
<tr><td>UDP receiver</td><td id="udp">Starting…</td></tr><tr><td>Last packet</td><td id="last">Never</td></tr><tr><td>Malformed packets</td><td id="bad">0</td></tr><tr><td>Dashboard uptime</td><td id="uptime">0s</td></tr></tbody></table>
<div class="notice">This screen shows measured CSI data and a relative signal-change indicator. A calibrated multi-link room map is not available until transmitter positions and MAC addresses are measured and configured.</div></div>
</section><div class="footer">LOCAL PROCESSING · NO CLOUD · EXPERIMENTAL RF MEASUREMENTS</div></main>
<script>
const $=id=>document.getElementById(id);
function draw(id,values,color,max=1){const c=$(id),r=c.getBoundingClientRect(),d=window.devicePixelRatio||1;c.width=Math.max(1,r.width*d);c.height=Math.max(1,r.height*d);const x=c.getContext('2d');x.scale(d,d);const w=r.width,h=r.height;x.strokeStyle='#26364b';x.lineWidth=1;for(let i=1;i<4;i++){x.beginPath();x.moveTo(0,h*i/4);x.lineTo(w,h*i/4);x.stroke()}if(!values.length)return;x.strokeStyle=color;x.lineWidth=2;x.beginPath();values.forEach((v,i)=>{const px=i/(Math.max(1,values.length-1))*w,py=h-8-Math.min(1,Math.max(0,v/max))*(h-16);i?x.lineTo(px,py):x.moveTo(px,py)});x.stroke()}
async function update(){try{const s=await(await fetch('/api/state',{cache:'no-store'})).json();$('packets').textContent=s.packets.toLocaleString();$('rssi').textContent=s.latest_rssi==null?'—':s.latest_rssi+' dBm';$('activity').textContent=s.latest_activity==null?'—':Math.round(s.latest_activity*100)+'%';$('devices').textContent=s.device_count;$('bad').textContent=s.malformed;$('uptime').textContent=Math.floor(s.uptime)+'s';$('last').textContent=s.last_age==null?'Never':s.last_age.toFixed(1)+'s ago';$('udp').textContent='Listening · '+s.udp_port;$('status').textContent=s.online?'CSI STREAM ACTIVE':'WAITING FOR CSI';$('dot').className='dot'+(s.online?' on':'');draw('wave',s.csi,'#56e0d0',Math.max(20,...s.csi,1));draw('activityChart',s.activity,'#ff9d5c',1);
$('deviceTable').innerHTML=s.devices.length?'<table><thead><tr><th>MAC ADDRESS</th><th>RSSI</th><th>PACKETS</th></tr></thead><tbody>'+s.devices.map(d=>'<tr><td>'+d.mac+'</td><td>'+d.rssi+' dBm</td><td>'+d.packets+'</td></tr>').join('')+'</tbody></table>':'No CSI packets received yet.';}catch(e){$('status').textContent='DASHBOARD API ERROR';$('udp').textContent='Unavailable'}}
setInterval(update,1000);update();window.addEventListener('resize',update);
</script></body></html>"""


def discovery_receiver(bind: str, discovery_port: int, csi_port: int) -> None:
    """Answer ESP32 broadcast discovery requests with this laptop's source IP."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind, discovery_port))
    print(f"Discovery: listening on {bind}:{discovery_port}")
    response = f"RF_TOMO_HERE_V1:{csi_port}".encode("ascii")
    while True:
        payload, address = sock.recvfrom(256)
        if payload.strip() == b"RF_TOMO_DISCOVER_V1":
            sock.sendto(response, address)


def udp_receiver(bind: str, port: int) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, port))
    while True:
        payload, address = sock.recvfrom(8192)
        try:
            packet = json.loads(payload.decode("utf-8"))
            if packet.get("type") != "csi":
                continue
            mac = str(packet["mac"]).upper()
            values = packet.get("csi", [])
            if not isinstance(values, list) or len(values) < 2:
                raise ValueError("missing CSI samples")
            iq = values[: len(values) - len(values) % 2]
            mags = [math.hypot(float(iq[i]), float(iq[i + 1])) for i in range(0, len(iq), 2)]
            if not mags:
                raise ValueError("empty CSI magnitudes")
            with LOCK:
                device = STATE["devices"].setdefault(mac, {"packets": 0, "rssi": -100, "baseline": None})
                device["packets"] += 1
                device["rssi"] = int(packet.get("rssi", -100))
                feature = sorted(mags)[len(mags) // 2]
                baseline = device["baseline"]
                if baseline is None:
                    device["baseline"] = feature
                    change = 0.0
                else:
                    change = min(1.0, abs(feature - baseline) / max(baseline, 1.0))
                    if change < 0.12:
                        device["baseline"] = 0.98 * baseline + 0.02 * feature
                STATE["packets"] += 1
                STATE["last_packet"] = time.time()
                STATE["rssi"].append(device["rssi"])
                STATE["activity"].append(change)
                STATE["csi"].clear()
                STATE["csi"].extend(mags[:128])
        except (ValueError, KeyError, TypeError, UnicodeDecodeError, OverflowError):
            with LOCK:
                STATE["malformed"] += 1


class Handler(BaseHTTPRequestHandler):
    udp_port = 5005

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/?"):
            body = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
        elif self.path == "/api/state":
            with LOCK:
                now = time.time()
                last = STATE["last_packet"]
                data = {
                    "packets": STATE["packets"], "malformed": STATE["malformed"],
                    "online": bool(last and now - last < 5), "last_age": now - last if last else None,
                    "uptime": now - STATE["started"], "udp_port": self.udp_port,
                    "device_count": len(STATE["devices"]),
                    "latest_rssi": STATE["rssi"][-1] if STATE["rssi"] else None,
                    "latest_activity": STATE["activity"][-1] if STATE["activity"] else None,
                    "activity": list(STATE["activity"]), "csi": list(STATE["csi"]),
                    "devices": [{"mac": k, "packets": v["packets"], "rssi": v["rssi"]} for k, v in STATE["devices"].items()],
                }
            body = json.dumps(data).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        else:
            body = b"Not found"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-S3 RF Tomography live dashboard")
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind address")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--udp-bind", default="0.0.0.0")
    parser.add_argument("--udp-port", type=int, default=5005)
    args = parser.parse_args()
    threading.Thread(target=udp_receiver, args=(args.udp_bind, args.udp_port), daemon=True).start()
    threading.Thread(target=discovery_receiver,
                     args=(args.udp_bind, 5006, args.udp_port), daemon=True).start()
    Handler.udp_port = args.udp_port
    server = ThreadingHTTPServer((args.host, args.http_port), Handler)
    print(f"Dashboard: http://127.0.0.1:{args.http_port}")
    print(f"Network:   http://<LAPTOP-IP>:{args.http_port}")
    print(f"UDP CSI:   {args.udp_bind}:{args.udp_port}")
    print("UDP discovery: port 5006")
    print("Waiting for ESP32-S3 CSI packets...")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping dashboard.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
