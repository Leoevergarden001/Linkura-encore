#!/usr/bin/env python3
"""Small reverse-engineering helpers for the iOS UnityFramework (IL2CPP).

These are the tools used to find the iOS patch sites (see docs/en/patching-ios.md).
They work on files you extract from your own IPA:

    python3 -m tools.linkura_offline ios-symbols --ipa <ipa> --output sym.json
    unzip -j <ipa> 'Payload/*.app/Frameworks/UnityFramework.framework/UnityFramework' -d .

    python3 tools/re/il2cpp_re.py -f UnityFramework -s sym.json find 'ChapterPoint.*OnPointerClick'
    python3 tools/re/il2cpp_re.py -f UnityFramework -s sym.json dis 0x2eb0c70 0x200
    python3 tools/re/il2cpp_re.py -f UnityFramework -s sym.json callers 'CommonApiCache::get_CurrentChapterRank'
    python3 tools/re/il2cpp_re.py -f UnityFramework bytes 0x2eb0fe8 4

Addresses are the framework's virtual addresses; for the 5.0.1 build the
__TEXT segment starts at file offset 0, so they are also file offsets.
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import subprocess
import sys
from pathlib import Path


class Symbols:
    def __init__(self, path: Path | None):
        self.entries = sorted(json.loads(path.read_text())) if path else []
        self.addrs = [a for a, _ in self.entries]

    def name(self, address: int) -> str:
        i = bisect.bisect_right(self.addrs, address) - 1
        if i < 0:
            return "?"
        base, name = self.entries[i]
        return name if base == address else f"{name}+{address - base:#x}"

    def matching(self, pattern: str) -> list[tuple[int, str]]:
        rx = re.compile(pattern)
        return [(a, n) for a, n in self.entries if rx.search(n)]


def objdump() -> list[str]:
    for candidate in (["xcrun", "objdump"], ["llvm-objdump"], ["objdump"]):
        try:
            subprocess.run([*candidate, "--version"], capture_output=True, check=True)
            return candidate
        except (OSError, subprocess.CalledProcessError):
            continue
    sys.exit("objdump not found (install Xcode command line tools or LLVM)")


def cmd_find(args, syms: Symbols) -> None:
    for address, name in syms.matching(args.pattern):
        print(f"{address:#x} {name}")


def cmd_dis(args, syms: Symbols) -> None:
    start, length = int(args.address, 0), int(args.length, 0)
    out = subprocess.run([*objdump(), "-d", "--no-show-raw-insn", f"--start-address={start:#x}",
                          f"--stop-address={start + length:#x}", str(args.framework)],
                         capture_output=True, text=True, check=True).stdout
    branch = re.compile(r"\b(bl|b|b\.\w+|cbz|cbnz|tbz|tbnz)\s.*?0x([0-9a-f]+)")
    for line in out.splitlines():
        if not re.match(r"\s*[0-9a-f]+:", line):
            continue
        m = branch.search(line)
        note = f"  ; -> {syms.name(int(m.group(2), 16))}" if m and syms.entries else ""
        print(line.rstrip() + note)


def bl_targets(data: bytes):
    """Yield (call_site, target) for every AArch64 BL in the image."""
    try:
        import numpy as np
        words = np.frombuffer(data[: len(data) // 4 * 4], dtype="<u4")
        sites = np.nonzero((words & 0xFC000000) == 0x94000000)[0]
        imm = (words[sites] & 0x03FFFFFF).astype(np.int64)
        imm[imm >= 1 << 25] -= 1 << 26
        for site, target in zip((sites * 4).tolist(), (sites.astype(np.int64) * 4 + imm * 4).tolist()):
            yield site, target
    except ImportError:
        for site in range(0, len(data) - 3, 4):
            word = struct.unpack_from("<I", data, site)[0]
            if word & 0xFC000000 == 0x94000000:
                imm = word & 0x03FFFFFF
                if imm >= 1 << 25:
                    imm -= 1 << 26
                yield site, site + imm * 4


def cmd_callers(args, syms: Symbols) -> None:
    wanted = {a: n for a, n in syms.matching(args.pattern)}
    if not wanted:
        sys.exit("no symbol matches")
    hits: dict[int, list[int]] = {a: [] for a in wanted}
    for site, target in bl_targets(args.framework.read_bytes()):
        if target in hits:
            hits[target].append(site)
    for address, name in wanted.items():
        print(f"{address:#x} {name}")
        for site in hits[address]:
            print(f"    {site:#x} in {syms.name(site)}")


def cmd_bytes(args, syms: Symbols) -> None:
    start, count = int(args.address, 0), int(args.count, 0)
    data = args.framework.read_bytes()[start:start + count * 4]
    for i in range(0, len(data), 4):
        print(f"{start + i:#x}: {data[i:i + 4].hex()}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-f", "--framework", type=Path, required=True, help="UnityFramework binary")
    parser.add_argument("-s", "--symbols", type=Path, help="ios-symbols output (JSON)")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("find", help="list methods whose name matches a regex"); p.add_argument("pattern")
    p = sub.add_parser("dis", help="disassemble with call targets named"); p.add_argument("address"); p.add_argument("length", nargs="?", default="0x100")
    p = sub.add_parser("callers", help="call sites of methods matching a regex"); p.add_argument("pattern")
    p = sub.add_parser("bytes", help="raw instruction words (for patch tables)"); p.add_argument("address"); p.add_argument("count", nargs="?", default="1")
    args = parser.parse_args()
    {"find": cmd_find, "dis": cmd_dis, "callers": cmd_callers, "bytes": cmd_bytes}[args.cmd](args, Symbols(args.symbols))


if __name__ == "__main__":
    main()
