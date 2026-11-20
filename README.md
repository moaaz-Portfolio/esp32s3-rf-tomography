# ESP32-S3 RF Tomography

Experimental Wi-Fi CSI acquisition and RF occupancy reconstruction using one ESP32-S3 receiver and multiple existing Wi-Fi transmitters.

> Research prototype: this estimates changes in RF propagation and coarse occupancy. It does **not** reconstruct exact walls, furniture, or a photographic room map.

## Architecture

- `firmware/`: ESP-IDF firmware. Connects to a 2.4 GHz access point, receives CSI from Wi-Fi frames, and streams compact UDP JSON packets to a computer on the same LAN.
- `engine/`: Python receiver, per-transmitter baseline tracking, Fresnel-zone sensitivity matrix, non-negative sparse FISTA reconstruction, and heatmap output.
- `.github/workflows/`: ESP-IDF firmware build and Python tests.

## Hardware

- ESP32-S3 development board with Wi-Fi.
- 2.4 GHz Wi-Fi access point.
- One or more ordinary Wi-Fi clients that generate traffic. A single receiver only observes frames it can decode; it does not create independent spatial links by itself.
- Computer running the Python engine on the same LAN.

## Quick start

1. Configure Wi-Fi and collector settings through `idf.py menuconfig`.
2. Build with ESP-IDF 5.3.x: `idf.py -C firmware set-target esp32s3` then `idf.py -C firmware build`.
3. Install Python dependencies: `python -m pip install -r engine/requirements.txt`.
4. Start the collector: `python engine/collector.py --bind 0.0.0.0 --port 5005`.
5. Flash the firmware and monitor it. Set `CONFIG_TOMO_WIFI_SSID`, `CONFIG_TOMO_WIFI_PASSWORD`, `CONFIG_TOMO_COLLECTOR_IP`, and `CONFIG_TOMO_COLLECTOR_PORT` in menuconfig.

The GitHub Actions workflow builds the firmware and runs engine tests on pushes and pull requests. Firmware binaries are uploaded as workflow artifacts; tagged releases also publish a ZIP.

## Important limitations

- CSI availability and fields vary by ESP-IDF version, chip, frame type, and Wi-Fi environment.
- Promiscuous/CSI reception is not guaranteed for every transmitter or every packet.
- One receiver does not provide true multi-angle tomography unless multiple spatially distinct transmitter-receiver paths are actually observed and their positions are known.
- The initial implementation uses received CSI amplitude as a coarse change feature. Phase calibration, clock/CFO compensation, packet grouping, and site calibration remain research work.
- The heatmap is an experimental occupancy-change estimate, not a metric-accurate 3D reconstruction.

## Safety and privacy

Use only in environments and on networks where you have permission. Do not use this system to identify or track people without their informed consent.
