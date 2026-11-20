#!/usr/bin/env python3
"""Create an ESP Web Tools manifest from ESP-IDF's generated flasher_args.json."""
import json
import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[1]
build = root / "firmware" / "build"
web_fw = root / "web" / "firmware"
args_path = build / "flasher_args.json"
if not args_path.exists():
    raise SystemExit(f"Missing ESP-IDF flash map: {args_path}")
data = json.loads(args_path.read_text(encoding="utf-8"))
flash_files = data.get("flash_files")
if not isinstance(flash_files, dict) or not flash_files:
    raise SystemExit("flasher_args.json has no flash_files map")
web_fw.mkdir(parents=True, exist_ok=True)
parts = []
for offset, raw_path in flash_files.items():
    source = Path(raw_path)
    if not source.is_absolute():
        source = build / source
    source = source.resolve()
    if not source.is_file():
        raise SystemExit(f"Firmware image not found: {source}")
    name = source.name
    shutil.copy2(source, web_fw / name)
    parts.append({"path": f"./{name}", "offset": int(str(offset), 0)})
parts.sort(key=lambda item: item["offset"])
manifest = {
    "name": "RF Tomography ESP32-S3",
    "version": "0.1.0",
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
}
(web_fw / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print(json.dumps(manifest, indent=2))
