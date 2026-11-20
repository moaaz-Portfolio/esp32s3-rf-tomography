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

1. Install Python dependencies: `python -m pip install -r engine/requirements.txt`.
2. Start the collector: `python engine/collector.py --bind 0.0.0.0 --port 5005`.
3. Configure the collector IPv4 address and UDP port in firmware menuconfig if the defaults do not match your computer.
4. Flash the firmware, connect to `RF-TOMO-SETUP`, and configure router Wi-Fi through `http://192.168.4.1/`.

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
