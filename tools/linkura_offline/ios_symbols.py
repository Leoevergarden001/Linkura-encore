"""Build an address -> managed method name table for an iOS IL2CPP image.

Uses the same static recovery as ios_metadata.map_methods (metadata v31
method tokens + each image's CodeGenModule method-pointer table), but for
every method, so native backtraces from LLDB can be symbolicated.
"""
from __future__ import annotations

import bisect
import json
from pathlib import Path

from .ios_macho import IosError, MachO, region, unpack
from .ios_metadata import Metadata31


def build_symbols(metadata_path: Path, framework_path: Path) -> list[tuple[int, str]]:
    metadata = Metadata31(metadata_path.read_bytes())
    image = MachO(framework_path.read_bytes())
    refs = image.references()
    symbols: list[tuple[int, str]] = []
    for index in range(len(metadata.images) // 40):
        name_index, _, start, count, *_ = unpack(metadata.images, "<IiiIIiIIII", index * 40)
        image_name = metadata.string(name_index)
        candidates = set()
        cursor = 0
        needle = image_name.encode() + b"\0"
        while (cursor := image.data.find(needle, cursor)) >= 0:
            try:
                address = image.file_to_vm(cursor)
                for position in refs.get(address, []):
                    pointer_count = unpack(image.data, "<Q", position + 8)[0]
                    table = image.pointer(position + 16)
                    if 0 < pointer_count <= len(metadata.methods) // 36 and table:
                        candidates.add((pointer_count, image.vm_to_file(table, pointer_count * 8)))
            except IosError:
                pass
            cursor += len(needle)
        if len(candidates) != 1:
            continue
        pointer_count, table = next(iter(candidates))
        for ti in range(start, start + count):
            record = region(metadata.types, ti * 88, 88)
            type_name, namespace = map(metadata.string, unpack(record, "<II", 0))
            method_start = unpack(record, "<i", 36)[0]
            method_count = unpack(record, "<H", 64)[0]
            for mi in range(method_start, method_start + method_count):
                method = region(metadata.methods, mi * 36, 36)
                name, _, _, _, _, _, token, *_ = unpack(method, "<IiiiiiIHHHH", 0)
                rid = token & 0xFFFFFF
                if not 0 < rid <= pointer_count:
                    continue
                address = image.pointer(table + (rid - 1) * 8)
                if address:
                    full = f"{namespace}.{type_name}" if namespace else type_name
                    symbols.append((address - image.base, f"{full}::{metadata.string(name)}"))
    symbols.sort()
    return symbols


def write_symbols(metadata_path: Path, framework_path: Path, output: Path) -> int:
    symbols = build_symbols(metadata_path, framework_path)
    output.write_text(json.dumps(symbols), encoding="utf-8")
    return len(symbols)


class Symbolicator:
    def __init__(self, table: Path):
        self.symbols = [(int(a), n) for a, n in json.loads(table.read_text(encoding="utf-8"))]
        self.addresses = [a for a, _ in self.symbols]

    def lookup(self, address: int) -> str:
        i = bisect.bisect_right(self.addresses, address) - 1
        if i < 0:
            return hex(address)
        base, name = self.symbols[i]
        return f"{name}+{address - base:#x}" if address - base < 0x20000 else hex(address)
