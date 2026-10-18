#!/usr/bin/env python3
"""KortaMod USB installer for the Chromatic v4.2 MCU."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time
from datetime import datetime

FLASH_BYTES = 4 * 1024 * 1024
APP_OFFSET = 0x10000
APP_BYTES = 1024 * 1024
PART_OFFSET = 0x8000
USB_ID = (0x374E, 0x0101)
APP_SHA256 = "f0edaf20b48f68974145f9bbdaf37b741e66aaf616918f684921f454f28192c1"
PART_SHA256 = "7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820"
# KortaMod layout: stock up to factory, plus otadata, an update slot and games.
KORTA_PART_SHA256 = "89b948ea4b4733b78a7d100d2e622efef37d854036071134ef05e1b36d2c30aa"
OTADATA = (0x110000, 0x2000)
GAMES = (0x220000, 0x80000)
VERSION_RE = re.compile(r"^0\.13\.4(-korta[3-9]|-korta[1-9][0-9])?$")
# Return to stock: ModRetro's own v4.2 MCU release, a full image from 0x0. Its app
# (at 0x10000) is byte-for-byte the factory app of a stock Chromatic v4.2.
STOCK_URL = "https://github.com/ModRetro/oss-chromatic-console-mcu/releases/download/v4.2/v0.13.4.bin"
STOCK_FILE_SHA256 = "f936c98b7d5e07299c0e0441d1f99dd88c4be875188e5cda14fc66a3f4ca84a8"
STOCK_APP_SHA256 = "006fb703269746e9ae5392cb45b40715a466f4f266d2a711c09a6db28f572c50"
STOCK_FILE_MAX = 2 * 1024 * 1024
KORTA_AREA = (0x110000, 0x190000)   # otadata, the update slot and games: unused by stock


def resource(name: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / "firmware" / name


def verify_package() -> tuple[Path, Path]:
    app, part, korta = resource("mcu_fw.bin"), resource("partition-table.bin"), resource("partition-table-korta.bin")
    for path, expected in ((app, APP_SHA256), (part, PART_SHA256), (korta, KORTA_PART_SHA256)):
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"Firmware package is missing or damaged: {path.name}")
    if not 0x50 <= app.stat().st_size <= APP_BYTES:
        raise RuntimeError("Application does not fit the stock factory partition")
    app_version(app.read_bytes())
    return app, part


def app_version(data: bytes) -> str:
    if len(data) < 0x50 or data[0] != 0xE9 or data[0x20:0x24] != b"2T\xcd\xab":
        raise RuntimeError("No compatible ESP32 application at 0x10000")
    return data[0x30:0x50].split(b"\0", 1)[0].decode("ascii", "replace")


def layout(part: bytes) -> str:
    """'stock' or 'korta' for the partition table read at 0x8000."""
    for name, filename, expected in (("stock", "partition-table.bin", PART_SHA256),
                                     ("korta", "partition-table-korta.bin", KORTA_PART_SHA256)):
        reference = resource(filename).read_bytes()
        if hashlib.sha256(reference).hexdigest() != expected:
            raise RuntimeError("Partition compatibility reference is damaged")
        if part[: len(reference)] == reference:
            return name
    raise RuntimeError("Partition layout differs from stock Chromatic v4.2; installation stopped")


def check_image(header: bytes, app_head: bytes, part: bytes) -> str:
    if len(header) < 0x20 or header[0] != 0xE9:
        raise RuntimeError("The connected MCU does not have the expected ESP32 bootloader")
    layout(part)
    version = app_version(app_head)
    if not VERSION_RE.match(version):
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


def esptool_command(port: str, *args: str) -> str:
    import esptool

    class ProgressOutput(io.TextIOBase):
        def __init__(self):
            self.log = ""
            self.shown = -1

        def write(self, message):
            self.log += message
            if len(self.log) > 200000:
                self.log = self.log[-100000:]
            for value in re.findall(r"\((\d+)\s*%\)", message):
                step = int(value) // 10
                if step > self.shown:
                    self.shown = step
                    print(f"  {min(step * 10, 100)}%", file=sys.__stdout__, flush=True)
            return len(message)

        def flush(self):
            pass

    output = ProgressOutput()
    try:
        with redirect_stdout(output):
            esptool.main(["--chip", "esp32", "--port", port, "--baud", "460800", *args])
    except SystemExit as error:
        if error.code not in (None, 0):
            raise RuntimeError(f"USB flash operation failed. {output.log[-500:]}") from error
    return output.log


def device_mac(port: str) -> str:
    output = esptool_command(port, "read_mac")
    match = re.search(r"MAC:\s*([0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})", output)
    if not match:
        raise RuntimeError("Could not read the Chromatic identity; nothing has been written")
    return match.group(1).lower()


def read_region(port: str, offset: int, size: int) -> bytes:
    with tempfile.TemporaryDirectory(prefix="kortamod-check-") as directory:
        output = Path(directory) / "region.bin"
        esptool_command(port, "read_flash", hex(offset), hex(size), str(output))
        data = output.read_bytes()
        if len(data) != size:
            raise RuntimeError("Flash read was incomplete")
        return data


def inspect(port: str) -> str:
    print("1/4 Checking your Chromatic...", flush=True)
    initial = read_region(port, 0x1000, 0x10000)
    boot = initial[:0x1000]
    part = initial[PART_OFFSET - 0x1000:PART_OFFSET]
    app = initial[APP_OFFSET - 0x1000:APP_OFFSET]
    version = check_image(boot, app, part)
    print(f"Compatible Chromatic MCU found: {version}", flush=True)
    return version


def backup_directory() -> Path:
    location = Path.home() / "Documents" / "KortaMod Backups"
    location.mkdir(mode=0o700, parents=True, exist_ok=True)
    if os.name != "nt":
        location.chmod(0o700)
    return location


def backup(port: str) -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    destination = backup_directory() / f"chromatic-mcu-{stamp}.bin"
    if destination.exists():
        raise RuntimeError("Backup filename already exists; retry in one second")
    print("2/4 Backing up your Chromatic. This may take a few minutes...", flush=True)
    identity = device_mac(port)
    metadata = destination.with_suffix(".json")
    # Exclusive reservations avoid overwriting a backup from another process.
    handle = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(handle)
    try:
        handle = os.open(metadata, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(handle)
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    try:
        destination.chmod(0o600)
        metadata.chmod(0o600)
        esptool_command(port, "read_flash", "0x0", hex(FLASH_BYTES), str(destination))
        data = destination.read_bytes()
        if len(data) != FLASH_BYTES:
            raise RuntimeError("Backup was incomplete; nothing has been flashed")
        check_image(data[0x1000:0x2000], data[APP_OFFSET:APP_OFFSET + 0x1000], data[PART_OFFSET:PART_OFFSET + 0x1000])
        with metadata.open("w", encoding="utf-8") as output:
            json.dump({"mac": identity, "sha256": hashlib.sha256(data).hexdigest()}, output)
            output.flush()
            os.fsync(output.fileno())
        metadata.chmod(0o600)
        with destination.open("rb+") as output:
            os.fsync(output.fileno())
    except BaseException:
        destination.unlink(missing_ok=True)
        metadata.unlink(missing_ok=True)
        raise
    print(f"Backup saved: {destination}", flush=True)
    print("Keep it private: it may contain Wi-Fi credentials.", flush=True)
    return destination


def sync_clock(port: str) -> None:
    import serial

    offset = datetime.now().astimezone().utcoffset()
    if offset is None:
        raise RuntimeError("The computer's local time zone is unavailable")
    minutes = round(offset.total_seconds() / 60)
    if not -720 <= minutes <= 840:
        raise RuntimeError("The computer's UTC offset is outside the supported range")
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=2,
                       dsrdtr=False, rtscts=False) as console:
        console.dtr = False
        console.rts = False
        received = bytearray()
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline and b"mcu>" not in received:
            received.extend(console.read(4096))
            if len(received) > 16384:
                del received[:-8192]
        if b"mcu>" not in received:
            raise RuntimeError("The Chromatic console did not start; clock sync failed")
        stamp = int(time.time())
        console.write(f"korta time {stamp} {minutes}\r\n".encode("ascii"))
        time.sleep(0.3)
        console.write(b"korta\r\n")
        received.clear()
        deadline = time.monotonic() + 5
        pattern = re.compile(rb"korta time (\d+) known 1 zone_minutes ([+-]?\d+)")
        while time.monotonic() < deadline:
            received.extend(console.read(4096))
            matches = pattern.findall(received)
            if any(abs(int(epoch) - int(time.time())) <= 10 and int(zone) == minutes
                   for epoch, zone in matches):
                print(f"Clock synced to your computer (UTC{minutes / 60:+g}).", flush=True)
                return
            if len(received) > 16384:
                del received[:-8192]
    raise RuntimeError("Clock sync was not confirmed by the Chromatic")


def backup_layout(saved: Path) -> str:
    with saved.open("rb") as image:
        image.seek(PART_OFFSET)
        return layout(image.read(0x1000))


def install(selected: str | None) -> Path:
    app, _ = verify_package()
    table = resource("partition-table-korta.bin")
    port = require_port(selected)
    inspect(port)
    saved = backup(port)
    migrate = backup_layout(saved) == "stock"
    print("3/4 Installing KortaMod...", flush=True)
    # Clear otadata first so the bootloader runs the app written below, not an
    # update left in the staging slot. On a stock layout these regions are
    # unused, and the games area is cleared once so it starts empty.
    esptool_command(port, "erase_region", hex(OTADATA[0]), hex(OTADATA[1]))
    if migrate:
        print("Adding space for updates and games (your settings stay)...", flush=True)
        esptool_command(port, "erase_region", hex(GAMES[0]), hex(GAMES[1]))
        esptool_command(port, "write_flash", hex(PART_OFFSET), str(table), hex(APP_OFFSET), str(app))
    else:
        esptool_command(port, "write_flash", hex(APP_OFFSET), str(app))
    print("4/4 Verifying the installation...", flush=True)
    if migrate:
        esptool_command(port, "verify_flash", hex(PART_OFFSET), str(table), hex(APP_OFFSET), str(app))
    else:
        esptool_command(port, "verify_flash", hex(APP_OFFSET), str(app))
    print("Syncing the clock to your computer...", flush=True)
    sync_clock(port)
    print(f"Installed. Your restore backup is {saved}", flush=True)
    return saved


def download(url: str) -> bytes:
    import urllib.request

    import ssl

    try:
        import certifi  # bundled with the installer: frozen apps may not find the system's CAs
        context = ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        context = ssl.create_default_context()
    request = urllib.request.Request(url, headers={"User-Agent": "KortaMod-Installer"})
    with urllib.request.urlopen(request, timeout=60, context=context) as response:
        data = response.read(STOCK_FILE_MAX + 1)
    if len(data) > STOCK_FILE_MAX:
        raise RuntimeError("The official firmware download is larger than expected; nothing was written")
    return data


def stock_app(fetch=download) -> bytes:
    """ModRetro's official v0.13.4 app, checked against pinned hashes."""
    print("Downloading ModRetro's official firmware (v4.2, MCU 0.13.4)...", flush=True)
    try:
        image = fetch(STOCK_URL)
    except OSError as error:
        raise RuntimeError(f"Could not download the official firmware ({error}); nothing was written") from error
    if hashlib.sha256(image).hexdigest() != STOCK_FILE_SHA256:
        raise RuntimeError("The official firmware download did not match its known checksum; nothing was written")
    table = resource("partition-table.bin").read_bytes()
    if hashlib.sha256(table).hexdigest() != PART_SHA256 or image[PART_OFFSET:PART_OFFSET + len(table)] != table:
        raise RuntimeError("The official firmware uses an unexpected layout; nothing was written")
    app = image[APP_OFFSET:]
    if hashlib.sha256(app).hexdigest() != STOCK_APP_SHA256 or app_version(app) != "0.13.4":
        raise RuntimeError("The official firmware app did not match its known checksum; nothing was written")
    return app


def return_to_stock(selected: str | None, fetch=download) -> Path:
    """Official ModRetro firmware and layout; settings (NVS) stay, a backup is made first."""
    app = stock_app(fetch)
    table = resource("partition-table.bin")
    port = require_port(selected)
    inspect(port)
    saved = backup(port)
    print("3/4 Returning to ModRetro's official firmware...", flush=True)
    with tempfile.TemporaryDirectory(prefix="kortamod-stock-") as directory:
        app_file = Path(directory) / "stock-app.bin"
        app_file.write_bytes(app)
        # The app partition is cleared whole so nothing of KortaMod remains past
        # the official image, and KortaMod's extra areas go back to empty flash.
        esptool_command(port, "erase_region", hex(KORTA_AREA[0]), hex(KORTA_AREA[1]))
        esptool_command(port, "erase_region", hex(APP_OFFSET), hex(APP_BYTES))
        esptool_command(port, "write_flash", hex(PART_OFFSET), str(table), hex(APP_OFFSET), str(app_file))
        print("4/4 Verifying...", flush=True)
        esptool_command(port, "verify_flash", hex(PART_OFFSET), str(table), hex(APP_OFFSET), str(app_file))
    print("Done: your Chromatic runs ModRetro's official firmware again. ModRetro's Update Tool can now "
          f"install newer official releases. Your backup is {saved}", flush=True)
    return saved


def restore(selected: str | None, filename: Path, allow_legacy: bool = False) -> None:
    port = require_port(selected)
    image = filename.expanduser().resolve()
    if not image.is_file() or image.stat().st_size != FLASH_BYTES:
        raise RuntimeError("Restore requires a complete 4 MiB backup from this same Chromatic")
    data = image.read_bytes()
    metadata = image.with_suffix(".json")
    if metadata.is_file():
        try:
            record = json.loads(metadata.read_text(encoding="utf-8"))
        except (ValueError, UnicodeError) as error:
            raise RuntimeError("Backup identity file is damaged; restore stopped") from error
        if not isinstance(record, dict) or record.get("sha256") != hashlib.sha256(data).hexdigest():
            raise RuntimeError("Backup checksum does not match; restore stopped")
        if record.get("mac") != device_mac(port):
            raise RuntimeError("Backup belongs to a different Chromatic; restore stopped")
    elif not allow_legacy:
        raise RuntimeError("Backup identity file is missing. For a backup made before v0.1.2, use --allow-legacy-backup only on its original Chromatic.")
    check_image(data[0x1000:0x2000], data[APP_OFFSET:APP_OFFSET + 0x1000], data[PART_OFFSET:PART_OFFSET + 0x1000])
    print("Restoring the selected full flash image to the connected Chromatic...", flush=True)
    esptool_command(port, "write_flash", "0x0", str(image))
    esptool_command(port, "verify_flash", "0x0", str(image))
    print("Restore complete.", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Chromatic serial port; automatic when one is connected")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="Check compatibility without installing")
    mode.add_argument("--sync-clock", action="store_true", help="Sync the Chromatic clock to this computer")
    mode.add_argument("--self-test", action="store_true", help="Check bundled firmware without a device")
    mode.add_argument("--restore", type=Path, metavar="BACKUP", help="Restore your own full flash backup")
    mode.add_argument("--stock", action="store_true", help="Return to ModRetro's official firmware (downloads it; settings stay)")
    parser.add_argument("--allow-legacy-backup", action="store_true", help="Allow an older backup without device identity metadata")
    args = parser.parse_args()
    try:
        if args.self_test:
            app, _ = verify_package()
            print(f"Package verified: MCU {app_version(app.read_bytes())}")
        elif args.restore:
            restore(args.port, args.restore, args.allow_legacy_backup)
        elif args.stock:
            return_to_stock(args.port)
        elif args.sync_clock:
            sync_clock(require_port(args.port))
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
