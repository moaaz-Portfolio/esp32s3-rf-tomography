# ESP32-S3 RF Tomography

Experimental Wi-Fi CSI acquisition and RF occupancy reconstruction using one ESP32-S3 receiver and multiple existing Wi-Fi transmitters.

> Research prototype: this estimates changes in RF propagation and coarse occupancy. It does **not** reconstruct exact walls, furniture, or a photographic room map.

## Architecture

- `firmware/`: ESP-IDF firmware. Creates a protected Wi-Fi setup access point, serves a local configuration page, stores router credentials in NVS, connects to a 2.4 GHz access point, receives CSI from Wi-Fi frames, and streams compact UDP JSON packets to a computer on the same LAN.
- `engine/`: Python receiver, per-transmitter baseline tracking, Fresnel-zone sensitivity matrix, non-negative sparse FISTA reconstruction, and heatmap output.
- `.github/workflows/`: ESP-IDF firmware build and Python tests.

## Wi-Fi setup

1. Flash the firmware to the ESP32-S3.
2. From a phone or computer, connect to Wi-Fi network `RF-TOMO-SETUP`.
3. Setup password: `rf-tomo-setup`.
4. Open `http://192.168.4.1/` and enter the router's 2.4 GHz SSID and password.
5. The device saves credentials in NVS and connects while keeping the setup AP available.
6. Use the same page to reset saved Wi-Fi credentials and configure another router.

The setup AP is protected with WPA2. Keep the setup password private and change it in `firmware/main/main.c` before distributing devices.

## Hardware

- ESP32-S3 development board with Wi-Fi.
- 2.4 GHz Wi-Fi access point.
- One or more ordinary Wi-Fi clients that generate traffic. A single receiver only observes frames it can decode; it does not create independent spatial links by itself.
- Computer running the Python engine on the same LAN.

## Quick start

### Live dashboard on your laptop

1. Install Python 3.10+ on the laptop.
2. Open PowerShell in the project folder and run `python engine/dashboard.py` (no third-party Python packages are needed for the live monitor).
3. Open `http://127.0.0.1:8080` on the laptop. From another device on the same Wi-Fi, use `http://<LAPTOP-IP>:8080`.
4. The ESP32-S3 automatically discovers the dashboard on the same Wi-Fi subnet over UDP broadcast; no laptop IP needs to be hard-coded in the firmware.
5. Allow inbound UDP ports `5005` (CSI data) and `5006` (discovery), plus TCP port `8080` (dashboard) through the laptop firewall. On Windows PowerShell (Run as Administrator):
   ```powershell
   New-NetFirewallRule -DisplayName "RF Tomography Dashboard TCP 8080" -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow -Profile Any
   New-NetFirewallRule -DisplayName "RF Tomography CSI UDP 5005" -Direction Inbound -Protocol UDP -LocalPort 5005 -Action Allow -Profile Any
   New-NetFirewallRule -DisplayName "RF Tomography Discovery UDP 5006" -Direction Inbound -Protocol UDP -LocalPort 5006 -Action Allow -Profile Any
   ```
6. Keep the dashboard process running. Any device on the same LAN can open `http://<LAPTOP-IP>:8080`; CSI packets are sent to the laptop running the dashboard. The dashboard reports real incoming CSI packets; it does not fabricate data.

The initial live monitor plots CSI amplitude and a relative per-device change indicator. It does not claim to provide a calibrated room reconstruction. The sample `engine/room.json` contains placeholder MAC addresses and coordinates; replace them with measured transmitter MAC addresses and positions before using the tomography solver.

### Optional reconstruction collector

Install dependencies with `python -m pip install -r engine/requirements.txt`, then run `python engine/collector.py --bind 0.0.0.0 --port 5005` after calibrating `engine/room.json`.

The GitHub Actions workflow builds the firmware and runs engine tests on pushes and pull requests. Firmware binaries are uploaded as workflow artifacts; tagged releases also publish a ZIP.

## Important limitations

- CSI availability and fields vary by ESP-IDF version, chip, frame type, and Wi-Fi environment.
- CSI capture starts after the station obtains an IP address from the router.
- Promiscuous/CSI reception is not guaranteed for every transmitter or every packet.
- One receiver does not provide true multi-angle tomography unless multiple spatially distinct transmitter-receiver paths are actually observed and their positions are known.
- The initial implementation uses received CSI amplitude as a coarse change feature. Phase calibration, clock/CFO compensation, packet grouping, and site calibration remain research work.
- The heatmap is an experimental occupancy-change estimate, not a metric-accurate 3D reconstruction.

## Safety and privacy

Use only in environments and on networks where you have permission. Do not use this system to identify or track people without their informed consent.
