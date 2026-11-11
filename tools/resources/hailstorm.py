#!/usr/bin/env python3
"""Inspect a Hailstorm asset cache against its catalog, and decrypt raw assets.

The game keeps downloaded assets in its cache dir ``D/<2 chars>/<RealName>``;
the catalog (Hailstorm ``catalog.json``) maps each logical label to its
RealName, size, type and seed. Everything here is offline and read-only on
the cache.

    # how much of the catalog a cache holds, per category
    python3 tools/resources/hailstorm.py coverage --catalog catalog.json --cache <dir with D/>

    # decrypt one raw (ResourceType 192) asset, e.g. a story script
    python3 tools/resources/hailstorm.py decrypt --catalog catalog.json --cache <dir> \\
        --label story_main_10250101.txt --output story.txt

The raw-asset key derivation follows vertesan/inspix-hailstorm
(manifest/asset.go, AGPL-3.0): AES-128-CBC with key/IV = SHA-256 of a fixed
prefix, the seed, CRC-64 (ECMA, non-reflected) of the label, CRC-32 of "raw"
and the varint size; the plaintext is an LZ4 frame.
Needs: ``pip install lz4 cryptography`` (decrypt only).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import zlib
from collections import Counter
from pathlib import Path

PREFIX = bytes.fromhex(
    "c34ea77df4976cd8907096fa47bb97e61852305892494e3692ba0c7eb434f022"
    "c549c96cf7ca0ee1b6ba7f203b6c76e8679699ce9c44af7b1cb000173a515938")
_POLY = 0x42F0E1EBA9EA3693
_TABLE = []
for _i in range(256):
    _c = _i << 56
    for _ in range(8):
        _c = ((_c << 1) ^ _POLY) if _c & (1 << 63) else (_c << 1)
    _TABLE.append(_c & 0xFFFFFFFFFFFFFFFF)


def crc64(data: bytes) -> int:
    c = 0
    for b in data:
        c = _TABLE[((c >> 56) ^ b) & 0xFF] ^ ((c << 8) & 0xFFFFFFFFFFFFFFFF)
    return c


def _uvarint(n: int) -> bytes:
    out = bytearray()
    while True:
        b, n = n & 0x7F, n >> 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def decrypt_raw(data: bytes, label: str, seed: int, size: int) -> bytes:
    import lz4.frame
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    material = (PREFIX + struct.pack(">Q", seed & 0x7FFFFFFFFFFFFFFF) + struct.pack(">Q", crc64(label.encode()))
                + struct.pack(">I", zlib.crc32(b"raw")) + _uvarint(size))
    kv = hashlib.sha256(material).digest()
    decryptor = Cipher(algorithms.AES(kv[:16]), modes.CBC(kv[16:])).decryptor()
    plain = decryptor.update(data) + decryptor.finalize()
    return lz4.frame.decompress(plain[:-plain[-1]])


def cache_path(cache: Path, real_name: str) -> Path:
    root = cache / "D" if (cache / "D").is_dir() else cache
    return root / real_name[:2] / real_name


def category(entry: dict) -> str:
    label = entry.get("StrLabelCrc") or ""
    cats = entry.get("StrCategoryCrcs") or []
    if label.startswith("vo_adv"):
        return "story voice"
    if label.startswith("mot_"):
        return "story motion"
    if "ppadv" in label:
        return "story scene"
    if label.startswith("story_main_"):
        return "story script"
    if "movie" in cats:
        return "movie:" + (cats[-1] if cats else "")
    return cats[0] if cats else (entry.get("StrTypeCrc") or "?")


def cmd_coverage(args) -> None:
    catalog = json.loads(args.catalog.read_text())
    total, have, missing_bytes = Counter(), Counter(), Counter()
    for entry in catalog:
        key = category(entry)
        total[key] += 1
        path = cache_path(args.cache, entry["RealName"])
        if path.is_file() and path.stat().st_size > 100:  # failed downloads leave 0-28 byte stubs
            have[key] += 1
        else:
            missing_bytes[key] += entry.get("Size", 0)
    print(f"{'category':32s} {'have':>6s} {'total':>6s} {'missing GB':>10s}")
    for key in sorted(total, key=lambda k: -missing_bytes[k]):
        print(f"{key[:32]:32s} {have[key]:6d} {total[key]:6d} {missing_bytes[key] / 1e9:10.2f}")
    print(f"{'ALL':32s} {sum(have.values()):6d} {sum(total.values()):6d} {sum(missing_bytes.values()) / 1e9:10.2f}")


def cmd_decrypt(args) -> None:
    catalog = json.loads(args.catalog.read_text())
    entry = next((e for e in catalog if e.get("StrLabelCrc") == args.label), None)
    if entry is None:
        raise SystemExit(f"{args.label} not in catalog")
    raw = cache_path(args.cache, entry["RealName"]).read_bytes()
    if entry.get("ResourceType") != 192:
        raise SystemExit(f"{args.label} is ResourceType {entry.get('ResourceType')}, not an encrypted raw asset")
    args.output.write_bytes(decrypt_raw(raw, entry["StrLabelCrc"], entry["Seed"], entry["Size"]))
    print(f"OK  {args.label} -> {args.output}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("coverage", help="catalog entries present in a cache, by category")
    p.add_argument("--catalog", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True, help="directory holding D/ (or D itself)")
    p = sub.add_parser("decrypt", help="decrypt one raw asset by label")
    p.add_argument("--catalog", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    {"coverage": cmd_coverage, "decrypt": cmd_decrypt}[args.cmd](args)


if __name__ == "__main__":
    main()
