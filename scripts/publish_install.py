#!/usr/bin/env python3
"""Publishes the release firmware for the site's browser installer (/install).

Copies release/kortamod/firmware/{mcu_fw.bin, partition-table.bin,
partition-table-korta.bin} into cloud/public/fw/install/ and writes
cloud/public/fw/install.json with each file's size and SHA-256, plus the app
version read from its header. It also fetches ModRetro's official v4.2 image
(for "Return to stock"), checked against the hashes pinned in installer.py, and
serves it from the same place, since the browser can't fetch GitHub release
assets directly. Run it whenever the release firmware changes, then deploy the
site. It never touches fw/latest.json (updates over the air). Pass --notes once
per release with one short line on what's new.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "release" / "kortamod" / "firmware"
TARGET = ROOT / "cloud" / "public" / "fw"
FILES = {"app": "mcu_fw.bin", "stock": "partition-table.bin", "korta": "partition-table-korta.bin"}


def app_version(data: bytes) -> str:
    if len(data) < 0x50 or data[0] != 0xE9 or data[0x20:0x24] != b"2T\xcd\xab":
        raise SystemExit("mcu_fw.bin is not an ESP32 application image")
    return data[0x30:0x50].split(b"\0", 1)[0].decode("ascii")


def pinned(name: str) -> str:
    text = (ROOT / "release" / "kortamod" / "installer.py").read_text(encoding="utf-8")
    match = re.search(rf'^{name} = "([^"]+)"', text, re.M)
    if not match:
        raise SystemExit(f"installer.py has no {name}")
    return match.group(1)


def official(out: Path) -> dict:
    """ModRetro's v4.2 release image, verified like the installer's --stock."""
    url, file_sha, app_sha = pinned("STOCK_URL"), pinned("STOCK_FILE_SHA256"), pinned("STOCK_APP_SHA256")
    target = out / "official-v0.13.4.bin"
    data = target.read_bytes() if target.is_file() else b""
    if hashlib.sha256(data).hexdigest() != file_sha:
        request = urllib.request.Request(url, headers={"User-Agent": "KortaMod-publish"})
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read(4 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != file_sha or hashlib.sha256(data[0x10000:]).hexdigest() != app_sha:
        raise SystemExit("The official firmware does not match the hashes pinned in installer.py")
    target.write_bytes(data)
    return {"path": f"/fw/install/{target.name}", "size": len(data), "sha256": file_sha, "app_sha256": app_sha,
            "source": url}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--notes", help="one short line: what's new (kept from the last publish of the same version)")
    args = parser.parse_args()
    out = TARGET / "install"
    out.mkdir(parents=True, exist_ok=True)
    try:
        previous = json.loads((TARGET / "install.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        previous = {}
    manifest = {}
    for key, name in FILES.items():
        data = (SOURCE / name).read_bytes()
        shutil.copyfile(SOURCE / name, out / name)
        manifest[key] = {"path": f"/fw/install/{name}", "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    version = app_version((SOURCE / FILES["app"]).read_bytes())
    same = previous.get("version") == version
    notes = args.notes if args.notes is not None else (previous.get("notes", "") if same else "")
    # Top level: what a page needs to say "Latest: korta8"; the parts follow.
    manifest = {
        "version": version,
        "date": previous.get("date") if same and previous.get("date") and args.notes is None
                else datetime.date.today().isoformat(),
        "notes": notes,
        "size": manifest["app"]["size"],
        "sha256": manifest["app"]["sha256"],
        **manifest,
        "official": official(out),
    }
    (TARGET / "install.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Published KortaMod {manifest['version']} for the browser installer ({manifest['app']['size']} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
