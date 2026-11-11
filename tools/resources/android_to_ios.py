#!/usr/bin/env python3
"""Convert an Android asset cache into an iOS data container for linkura-offline.

The iOS client of the same game reads the same cache layout (D/, M/) and the
same storage key, but Unity refuses asset bundles built for another platform,
and Android bundles carry no Metal shaders. This tool copies the cache and
fixes every UnityFS bundle:

* without --ipa: retarget the build target (Android 13 -> iOS 9) only;
* with --ipa: also replace bundled shaders with the Metal shaders of the
  iOS player (Data/data.unity3d in your IPA), or a stand-in, so materials do
  not render magenta (see unityfs_shader_swap.py). Slower: every bundle is
  parsed with UnityPy.

Other files (raw audio/video/text, the M/ databases) are copied unchanged.
The output is a ``container/`` tree that tools/ios_device_setup.py installs:

    container/Documents/D/...  container/Documents/M/...
    container/Documents/LinkuraOffline/storage-key.json

    python3 tools/resources/android_to_ios.py --cache <dir with D/ and M/> \\
        --storage-key <linkura-local/storage-key.json> --ipa <decrypted 5.0.1 IPA> --output <out dir>

Re-running skips files already converted (same name and a non-empty output).
Needs: ``pip install lz4`` and, with --ipa, ``pip install UnityPy``.
"""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import time
import zipfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from unityfs_retarget import retarget  # noqa: E402


def convert_cache(cache: Path, output: Path, storage_key: Path | None, ipa: Path | None) -> Counter:
    documents = output / "container" / "Documents"
    stats: Counter = Counter()
    swap = player = None
    if ipa:
        from unityfs_shader_swap import load_player_shaders, swap as swap_shaders
        with zipfile.ZipFile(ipa) as archive, tempfile.TemporaryDirectory() as tmp:
            member = next(n for n in archive.namelist() if n.endswith(".app/Data/data.unity3d"))
            archive.extract(member, tmp)
            player = load_player_shaders(Path(tmp) / member)
        swap = swap_shaders
        print(f"loaded {len(player)} Metal shaders from the IPA", flush=True)

    files = sorted(p for p in (cache / "D").rglob("*") if p.is_file() and not p.name.startswith("._"))
    started = time.time()
    for index, src in enumerate(files, 1):
        dst = documents / "D" / src.relative_to(cache / "D")
        if dst.is_file() and dst.stat().st_size > 0:
            stats["skipped"] += 1
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        blob = src.read_bytes()
        if blob[2:9] == b"UnityFS":
            try:
                if swap:
                    blob, shader_stats = swap(blob, player)
                    stats.update({f"shader_{k}": v for k, v in shader_stats.items()})
                else:
                    blob = retarget(blob)[0]
                stats["bundles"] += 1
            except Exception as error:  # keep going; the file is copied as-is
                stats["errors"] += 1
                print(f"ERR {src.name}: {error}", flush=True)
        else:
            stats["plain"] += 1
        dst.write_bytes(blob)
        if index % 2000 == 0:
            print(f"{index}/{len(files)} {time.time() - started:.0f}s {dict(stats)}", flush=True)

    if (cache / "M").is_dir():
        shutil.copytree(cache / "M", documents / "M", dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("._*"))
    if storage_key:
        (documents / "LinkuraOffline").mkdir(parents=True, exist_ok=True)
        shutil.copy2(storage_key, documents / "LinkuraOffline" / "storage-key.json")
    return stats


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=Path, required=True, help="Android cache dir holding D/ and M/")
    parser.add_argument("--storage-key", type=Path, help="storage-key.json of that cache (resource pack format)")
    parser.add_argument("--ipa", type=Path, help="decrypted iOS IPA; enables the Metal shader swap")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    stats = convert_cache(args.cache, args.output, args.storage_key, args.ipa)
    print(f"done: {dict(stats)} -> {args.output / 'container'}")
    if stats["errors"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
