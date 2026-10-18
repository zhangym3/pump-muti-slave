"""Package this project's role only; never flash a device."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil

ROLE = 'valve_slave_c3'
BOARD = 'esp32-c3-devkitm-1'
CHIP = 'esp32c3'
BOOT_OFFSET = '0x0'
root = Path(__file__).resolve().parents[1]
dist = root / "dist"
build = root / ".pio" / "build" / ROLE
images = {"bootloader.bin": BOOT_OFFSET, "partitions.bin": "0x8000", "firmware.bin": "0x20000"}
sources = sorted([p for folder in ("src", "include") for p in (root / folder).glob("*") if p.is_file()] +
                 [p for p in root.glob("sdkconfig*") if p.is_file() and not p.name.endswith(".old")] +
                 [root / n for n in ("platformio.ini", "partitions.csv", "CMakeLists.txt", "tools/package_firmware.py")])
for name in images:
    if not (build / name).is_file():
        raise SystemExit(f"Missing {ROLE}/{name}; build this project first")
app = build / "firmware.bin"
# Packaging script changes alone do not require recompiling firmware.
if any(p.stat().st_mtime_ns > app.stat().st_mtime_ns for p in sources if p.name != "package_firmware.py"):
    raise SystemExit("Firmware predates source/config; rebuild before packaging")
config = (build / "config" / "sdkconfig.h").read_text()
if f'#define CONFIG_IDF_TARGET "{CHIP}"' not in config:
    raise SystemExit("Build target does not match this project")
manifest = {"createdUtc": datetime.now(timezone.utc).isoformat(), "role": ROLE, "chip": CHIP,
            "board": BOARD, "flash": "4MB", "hardwareValidated": False,
            "note": "Application offset 0x20000. Preserve NVS. Match chip and role before flashing.",
            "files": [], "sources": {}}
for name, offset in images.items():
    target = dist / ROLE / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build / name, target)
    manifest["files"].append({"path": target.relative_to(dist).as_posix(), "offset": offset,
                              "bytes": target.stat().st_size, "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
for path in sources:
    manifest["sources"][path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
(dist / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Packaged {ROLE} ({CHIP}): 3 images in {dist}")
