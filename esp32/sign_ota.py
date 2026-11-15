#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Shield-1.0.0
# Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved.
# Required Notice: Copyright (c) 2026 Beanzilla Coffee Worldwide LLC. All rights reserved. Beanzilla, Beanzilla Coffee Worldwide, the Beanzilla Kaiju logo, and BulletSpy are trademarks of Beanzilla Coffee Worldwide LLC.
"""
BulletSpy S3 OTA Manifest & ECDSA P-256 Signature Generator.

Usage:
  # 1. Package & hash a compiled firmware.bin into release artifacts:
  python3 esp32/sign_ota.py \
      --firmware esp32/.pio/build/waveshare_esp32s3_rlcd42/firmware.bin \
      --version 1.1.0 \
      --out-dir dist

  # 2. (Optional) Generate an ECDSA P-256 keypair for cryptographic signing:
  python3 esp32/sign_ota.py --gen-key ota_signing_key.pem
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 0x0009


def verify_esp32s3_binary(bin_path: Path) -> None:
    data = bin_path.read_bytes()
    if len(data) < 65536:
        raise ValueError(f"{bin_path} is too small ({len(data)} bytes) to be a valid firmware image")
    magic = data[0]
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if magic != ESP_IMAGE_MAGIC:
        raise ValueError(f"Invalid ESP image magic: 0x{magic:02X} (expected 0xE9)")
    if chip_id != ESP32S3_CHIP_ID:
        raise ValueError(f"Wrong target chip_id: 0x{chip_id:04X} (expected 0x0009 for ESP32-S3)")


import re
import textwrap


def normalize_pem_key(raw_pem: str, header_type: str = "EC PRIVATE KEY") -> str:
    """Normalizes a PEM key even if pasted into GitHub Secrets as a single line or without headers."""
    b64_body = re.sub(r"-----BEGIN[^-]+-----", "", raw_pem)
    b64_body = re.sub(r"-----END[^-]+-----", "", b64_body)
    b64_body = re.sub(r"\s+", "", b64_body)
    wrapped = "\n".join(textwrap.wrap(b64_body, 64))
    return f"-----BEGIN {header_type}-----\n{wrapped}\n-----END {header_type}-----\n"


def sign_with_openssl_key(bin_path: Path, priv_pem: str) -> str:
    """Signs the SHA-256 digest of bin_path using OpenSSL ECDSA P-256 and returns hex DER signature."""
    key_file = bin_path.parent / "_tmp_ota_priv.pem"
    last_err = ""
    for hdr in ("EC PRIVATE KEY", "PRIVATE KEY"):
        try:
            key_file.write_text(normalize_pem_key(priv_pem, hdr), encoding="utf-8")
            res = subprocess.run(
                ["openssl", "dgst", "-sha256", "-sign", str(key_file), "-binary", str(bin_path)],
                check=False,
                capture_output=True,
            )
            if res.returncode == 0 and len(res.stdout) > 0:
                return res.stdout.hex()
            last_err = res.stderr.decode("utf-8", errors="replace").strip()
        finally:
            if key_file.exists():
                key_file.unlink()
    print(f"[WARN] OpenSSL signing failed ({last_err}); verify BEANZILLA_OTA_PRIVATE_KEY secret format.", file=sys.stderr)
    return ""


def generate_keypair(priv_out: Path) -> None:
    subprocess.run(
        ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(priv_out)],
        check=True,
    )
    res = subprocess.run(
        ["openssl", "ec", "-in", str(priv_out), "-pubout"],
        check=True,
        capture_output=True,
        text=True,
    )
    pub_pem = res.stdout.strip()
    print(f"Saved private key to: {priv_out} (ADD THIS TO GITHUB SECRET: BEANZILLA_OTA_PRIVATE_KEY)")
    print("\nPaste this Public Key into BEANZILLA_OTA_PUBKEY_PEM in esp32/include/ota_updater.h:\n")
    print(pub_pem)


def main() -> int:
    parser = argparse.ArgumentParser(description="BulletSpy S3 OTA Manifest & Signature Builder")
    parser.add_argument("--gen-key", type=Path, help="Generate a new ECDSA P-256 keypair and exit")
    parser.add_argument("--firmware", type=Path, help="Path to compiled firmware.bin")
    parser.add_argument("--version", type=str, default="1.1.0", help="Semantic version string")
    parser.add_argument("--repo", type=str, default="lpreimesberger/RoastMonster", help="GitHub owner/repo")
    parser.add_argument("--out-dir", type=Path, default=Path("dist"), help="Output directory for release assets")
    args = parser.parse_args()

    if args.gen_key:
        generate_keypair(args.gen_key)
        return 0

    if not args.firmware or not args.firmware.exists():
        print("Error: --firmware path is required and must exist", file=sys.stderr)
        return 1

    verify_esp32s3_binary(args.firmware)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    out_bin = args.out_dir / "bulletspy-esp32s3-rlcd42.bin"
    shutil.copy2(args.firmware, out_bin)

    sha256_hex = hashlib.sha256(out_bin.read_bytes()).hexdigest()
    sha_file = args.out_dir / "bulletspy-esp32s3-rlcd42.bin.sha256"
    sha_file.write_text(f"{sha256_hex}  bulletspy-esp32s3-rlcd42.bin\n", encoding="utf-8")

    priv_pem = os.environ.get("BEANZILLA_OTA_PRIVATE_KEY", "").strip()
    sig_hex = ""
    if priv_pem:
        sig_hex = sign_with_openssl_key(out_bin, priv_pem)

    clean_ver = args.version.lstrip("vV")
    tag = f"v{clean_ver}"
    manifest = {
        "name": "BulletSpy S3 (Waveshare ESP32-S3-RLCD-4.2)",
        "vendor": "Beanzilla Coffee Worldwide LLC",
        "license": "PolyForm-Shield-1.0.0",
        "version": clean_ver,
        "tag": tag,
        "chip_id": "0x0009 (ESP32-S3)",
        "size_bytes": out_bin.stat().st_size,
        "sha256": sha256_hex,
        "signature": sig_hex,
        "firmware_url": f"https://github.com/{args.repo}/releases/download/{tag}/bulletspy-esp32s3-rlcd42.bin",
    }

    manifest_path = args.out_dir / "ota_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"[OK] Verified ESP32-S3 header (0xE9 / 0x0009)")
    print(f"[OK] Binary:   {out_bin} ({out_bin.stat().st_size:,} bytes)")
    print(f"[OK] SHA-256:  {sha256_hex}")
    print(f"[OK] Manifest: {manifest_path} (signed={bool(sig_hex)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
