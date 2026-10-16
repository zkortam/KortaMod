#!/usr/bin/env python3
"""KortaMod USB installer for the Chromatic v4.2 MCU."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys
import tempfile
from datetime import datetime

FLASH_BYTES = 4 * 1024 * 1024
APP_OFFSET = 0x10000
PART_OFFSET = 0x8000
USB_ID = (0x374E, 0x0101)
APP_SHA256 = "855460c8b49dafe0f93ad48025d43ec802cf0f7799afaa66ad01e792a4f9e822"
PART_SHA256 = "7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820"


def resource(name: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / "firmware" / name


def verify_package() -> tuple[Path, Path]:
    app, part = resource("mcu_fw.bin"), resource("partition-table.bin")
    for path, expected in ((app, APP_SHA256), (part, PART_SHA256)):
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"Firmware package is missing or damaged: {path.name}")
    return app, part


def app_version(data: bytes) -> str:
    if len(data) < 0x50 or data[0] != 0xE9 or data[0x20:0x24] != b"2T\xcd\xab":
        raise RuntimeError("No compatible ESP32 application at 0x10000")
    return data[0x30:0x50].split(b"\0", 1)[0].decode("ascii", "replace")


def check_image(header: bytes, app_head: bytes, part: bytes) -> str:
    if len(header) < 0x20 or header[0] != 0xE9:
        raise RuntimeError("The connected MCU does not have the expected ESP32 bootloader")
    expected_part = resource("partition-table.bin").read_bytes()
    if part[: len(expected_part)] != expected_part:
        raise RuntimeError("Partition layout differs from stock Chromatic v4.2; installation stopped")
    version = app_version(app_head)
    if version not in ("0.13.4", "0.13.4-korta3"):
        raise RuntimeError(f"MCU version {version!r} is untested; installation stopped")
    return version


def ports() -> list[str]:
    from serial.tools import list_ports

    return [p.device for p in list_ports.comports() if (p.vid, p.pid) == USB_ID]


def require_port(selected: str | None) -> str:
    matches = ports()
    if selected:
        if selected not in matches:
            raise RuntimeError("Port is not a detected ModRetro Chromatic USB serial interface")
        return selected
    if len(matches) != 1:
        raise RuntimeError(f"Expected one connected Chromatic, found {len(matches)}. Use --port if needed.")
    return matches[0]


def esptool_command(port: str, *args: str) -> None:
    import esptool

    esptool.main(["--chip", "esp32", "--port", port, "--baud", "460800", *args])


def read_region(port: str, offset: int, size: int) -> bytes:
    with tempfile.TemporaryDirectory(prefix="kortamod-check-") as directory:
        output = Path(directory) / "region.bin"
        esptool_command(port, "read_flash", hex(offset), hex(size), str(output))
        data = output.read_bytes()
        if len(data) != size:
            raise RuntimeError("Flash read was incomplete")
        return data


def inspect(port: str) -> str:
    boot = read_region(port, 0x1000, 0x1000)
    part = read_region(port, PART_OFFSET, 0x1000)
    app = read_region(port, APP_OFFSET, 0x1000)
    version = check_image(boot, app, part)
    print(f"Compatible Chromatic MCU found: {version}", flush=True)
    return version


def backup_directory() -> Path:
    location = Path.home() / "Documents" / "KortaMod Backups"
    location.mkdir(parents=True, exist_ok=True)
    return location


def backup(port: str) -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    destination = backup_directory() / f"chromatic-mcu-{stamp}.bin"
    if destination.exists():
        raise RuntimeError("Backup filename already exists; retry in one second")
    print(f"Saving your private 4 MiB backup to {destination}", flush=True)
    esptool_command(port, "read_flash", "0x0", hex(FLASH_BYTES), str(destination))
    if destination.stat().st_size != FLASH_BYTES:
        destination.unlink(missing_ok=True)
        raise RuntimeError("Backup was incomplete; nothing has been flashed")
    print(f"Backup SHA-256: {hashlib.sha256(destination.read_bytes()).hexdigest()}", flush=True)
    print("Keep this file private: it contains device settings and may contain Wi-Fi credentials.", flush=True)
    return destination


def install(selected: str | None) -> Path:
    app, _ = verify_package()
    port = require_port(selected)
    inspect(port)
    saved = backup(port)
    print("Writing KortaMod application and verifying flash...", flush=True)
    esptool_command(port, "write_flash", hex(APP_OFFSET), str(app))
    esptool_command(port, "verify_flash", hex(APP_OFFSET), str(app))
    print(f"Installed. Your restore backup is {saved}", flush=True)
    return saved


def restore(selected: str | None, filename: Path) -> None:
    port = require_port(selected)
    image = filename.expanduser().resolve()
    if not image.is_file() or image.stat().st_size != FLASH_BYTES:
        raise RuntimeError("Restore requires a complete 4 MiB backup from this same Chromatic")
    data = image.read_bytes()
    check_image(data[0x1000:0x2000], data[APP_OFFSET:APP_OFFSET + 0x1000], data[PART_OFFSET:PART_OFFSET + 0x1000])
    print("Restoring the selected full flash image to the connected Chromatic...", flush=True)
    esptool_command(port, "write_flash", "0x0", str(image))
    esptool_command(port, "verify_flash", "0x0", str(image))
    print("Restore complete.", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Chromatic serial port; automatic when one is connected")
    parser.add_argument("--check", action="store_true", help="Check compatibility without installing")
    parser.add_argument("--restore", type=Path, metavar="BACKUP", help="Restore your own full flash backup")
    args = parser.parse_args()
    try:
        if args.restore:
            restore(args.port, args.restore)
        elif args.check:
            verify_package()
            inspect(require_port(args.port))
        else:
            install(args.port)
        return 0
    except (RuntimeError, OSError, ValueError) as error:
        print(f"KortaMod: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
