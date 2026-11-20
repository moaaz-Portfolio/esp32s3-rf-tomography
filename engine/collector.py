#!/usr/bin/env python3
"""UDP CSI collector and experimental RF tomography visualizer."""
from __future__ import annotations

import argparse
import json
import socket
import time
from collections import defaultdict, deque
from pathlib import Path

import numpy as np

from tomography import load_config, reconstruct


class BaselineTracker:
    def __init__(self, alpha: float = 0.02, window: int = 30):
        self.alpha = alpha
        self.samples = defaultdict(lambda: deque(maxlen=window))
        self.baseline: dict[str, float] = {}

    def update(self, mac: str, csi: list[int]) -> float:
        values = np.asarray(csi, dtype=np.float64)
        if values.size < 8:
            return 0.0
        # Firmware exports interleaved signed I/Q bytes; calculate per-pair magnitude.
        iq = values[: values.size - values.size % 2].reshape(-1, 2)
        magnitude = np.sqrt(iq[:, 0] ** 2 + iq[:, 1] ** 2)
        feature = float(np.median(magnitude))
        history = self.samples[mac]
        history.append(feature)
        if mac not in self.baseline:
            self.baseline[mac] = feature
        baseline = self.baseline[mac]
        delta = abs(feature - baseline) / max(baseline, 1.0)
        # Robust slow adaptation: retain the empty-room baseline without following spikes.
        if delta < 0.12:
            self.baseline[mac] = (1.0 - self.alpha) * baseline + self.alpha * feature
        return float(np.clip(delta, 0.0, 1.0))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5005)
    parser.add_argument("--config", default=str(Path(__file__).with_name("room.json")))
    parser.add_argument("--output", default="rf_heatmap.png")
    parser.add_argument("--min-links", type=int, default=2)
    args = parser.parse_args()

    cfg = load_config(args.config)
    tracker = BaselineTracker()
    latest: dict[str, float] = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)
    print(f"Listening for ESP32-S3 CSI packets on {args.bind}:{args.port}")
    print("Edit engine/room.json with measured transmitter coordinates and MAC addresses.")

    last_draw = 0.0
    while True:
        try:
            payload, address = sock.recvfrom(8192)
        except socket.timeout:
            continue
        try:
            packet = json.loads(payload.decode("utf-8"))
            if packet.get("type") != "csi":
                continue
            mac = str(packet["mac"]).upper()
            if mac not in cfg["links"]:
                continue
            change = tracker.update(mac, packet.get("csi", []))
            latest[mac] = change
            print(f"CSI {mac} RSSI={packet.get('rssi')} dBm change={change:.3f} from={address}")
        except (ValueError, KeyError, TypeError, UnicodeDecodeError) as exc:
            print(f"Discarded malformed packet: {exc}")
            continue

        now = time.monotonic()
        if len(latest) >= args.min_links and now - last_draw >= 1.0:
            image = reconstruct(cfg, latest)
            try:
                import matplotlib
                matplotlib.use("Agg")
                import matplotlib.pyplot as plt
                fig, ax = plt.subplots(figsize=(8, 6))
                extent = [0, cfg["room"]["width_m"], 0, cfg["room"]["height_m"]]
                ax.imshow(image, origin="lower", extent=extent, cmap="inferno",
                          interpolation="bilinear", aspect="equal", vmin=0, vmax=1)
                ax.set_title("Experimental RF change reconstruction")
                ax.set_xlabel("Room X (m)")
                ax.set_ylabel("Room Y (m)")
                fig.tight_layout()
                fig.savefig(args.output, dpi=140)
                plt.close(fig)
                print(f"Updated {args.output}; links={len(latest)}")
            except ImportError:
                print("Install matplotlib to save heatmap images.")
            last_draw = now


if __name__ == "__main__":
    main()
