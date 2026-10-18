#!/usr/bin/env python3
"""Sign a KortaMod MCU image for over-the-air updates.

    sign_firmware.py mcu_fw.bin 0.13.4-korta8 [--out cloud/public/fw] [--key ~/.kortamod/ota-signing-key.pem]
                     [--base-url https://kortamod.app/fw] [--notes "What changed"]

Copies the image to <out>/<version>.bin and writes <out>/latest.json:
{version, size, sha256, signature, url, notes}. The signature is ECDSA P-256 over
the image's SHA-256 (DER, base64), which kortamod_ota.c checks with the public key
compiled into the firmware. The private key never goes into a repository.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

VERSION_RE = re.compile(r"^0\.13\.4-korta\d+$")
APP_MAGIC_OFFSET = 0x20
APP_VERSION_OFFSET = 0x30


def image_version(data: bytes) -> str:
    if len(data) < 0x50 or data[0] != 0xE9 or data[APP_MAGIC_OFFSET:APP_MAGIC_OFFSET + 4] != b"2T\xcd\xab":
        raise SystemExit("not an ESP32 application image")
    return data[APP_VERSION_OFFSET:APP_VERSION_OFFSET + 32].split(b"\0", 1)[0].decode("ascii", "replace")


def sign(data: bytes, key_path: Path) -> str:
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise SystemExit("the signing key must be ECDSA P-256")
    signature = key.sign(data, ec.ECDSA(hashes.SHA256()))
    key.public_key().verify(signature, data, ec.ECDSA(hashes.SHA256()))
    return base64.b64encode(signature).decode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("version")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[1] / "cloud" / "public" / "fw")
    parser.add_argument("--key", type=Path, default=Path("~/.kortamod/ota-signing-key.pem").expanduser())
    parser.add_argument("--base-url", default="https://kortamod.app/fw")
    parser.add_argument("--notes", default="")
    args = parser.parse_args()

    if not VERSION_RE.match(args.version):
        raise SystemExit(f"version {args.version!r} should look like 0.13.4-korta8")
    data = args.image.read_bytes()
    embedded = image_version(data)
    if embedded != args.version:
        raise SystemExit(f"the image says {embedded!r}, not {args.version!r}")
    if len(data) > 0x100000:
        raise SystemExit("the image does not fit the 1 MiB app slot")
    if (args.key.stat().st_mode & 0o077) != 0:
        raise SystemExit(f"{args.key} is readable by others; chmod 600 it")

    args.out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.image, args.out / f"{args.version}.bin")
    manifest = {
        "version": args.version,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "signature": sign(data, args.key),
        "url": f"{args.base_url.rstrip('/')}/{args.version}.bin",
        "notes": args.notes,
    }
    tmp = args.out / "latest.json.tmp"
    tmp.write_text(json.dumps(manifest, indent=2) + "\n")
    os.replace(tmp, args.out / "latest.json")
    json.dump(manifest, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
