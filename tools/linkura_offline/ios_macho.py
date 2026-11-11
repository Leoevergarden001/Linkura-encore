"""Bounded, read-only arm64 Mach-O parsing and pre-sign load-command insertion.

Chained pointer layout reference: Apple's mach-o/fixup-chains.h.
This intentionally supports only thin little-endian arm64 images and dyld
pointer formats 2/6. Other formats fail explicitly rather than guessing addresses.
"""
from __future__ import annotations

import struct
from collections import defaultdict


class IosError(ValueError):
    pass


def region(data: bytes, offset: int, size: int) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise IosError(f"out-of-bounds binary region: {offset:#x}+{size:#x}")
    return data[offset:offset + size]


def unpack(data: bytes, fmt: str, offset: int) -> tuple:
    return struct.unpack(fmt, region(data, offset, struct.calcsize(fmt)))


def cstring(data: bytes, offset: int, limit: int | None = None) -> str:
    end_limit = len(data) if limit is None else limit
    if offset < 0 or end_limit < offset or end_limit > len(data):
        raise IosError("out-of-bounds binary string")
    end = data.find(b"\0", offset, end_limit)
    if end < 0:
        raise IosError("unterminated binary string")
    try:
        return data[offset:end].decode("utf-8")
    except UnicodeDecodeError as error:
        raise IosError("invalid UTF-8 binary string") from error


class MachO:
    def __init__(self, data: bytes):
        self.data = data
        magic, cpu, self.subtype, self.filetype, count, command_size, _, _ = unpack(data, "<8I", 0)
        if magic != 0xFEEDFACF or cpu != 0x0100000C:
            raise IosError("requires a thin little-endian arm64 Mach-O")
        self.command_end = 32 + command_size
        region(data, 32, command_size)
        self.commands: list[tuple[int, int, int]] = []
        self.segments: list[dict] = []
        self.sections: list[dict] = []
        self.encryption: list[dict] = []
        self.dependencies: list[str] = []
        self.platforms: list[int] = []
        self.export_range = None
        self.fixup_range = None
        position = 32
        for _ in range(count):
            cmd, size = unpack(data, "<II", position)
            if size < 8 or size % 8 or position + size > self.command_end:
                raise IosError("invalid Mach-O load command")
            command = region(data, position, size)
            self.commands.append((cmd, position, size))
            if cmd == 0x19:
                name = command[8:24].split(b"\0")[0].decode("ascii")
                vm, vm_size, offset, length, _, protection, sections, _ = unpack(command, "<QQQQiiII", 24)
                region(data, offset, length)
                self.segments.append(dict(name=name, vm=vm, vmSize=vm_size, offset=offset, size=length, protection=protection))
                for i in range(sections):
                    record = region(command, 72 + 80 * i, 80)
                    address, length, offset = unpack(record, "<QQI", 32)
                    flags = unpack(record, "<I", 64)[0]
                    if length and flags & 0xFF not in (1, 12, 18):
                        region(data, offset, length)
                    self.sections.append(dict(name=record[:16].split(b"\0")[0].decode("ascii"), vm=address, size=length, offset=offset, flags=flags))
            elif cmd == 0x2C:
                offset, length, cryptid = unpack(command, "<III", 8)
                region(data, offset, length)
                self.encryption.append(dict(cryptoff=offset, cryptsize=length, cryptid=cryptid))
            elif cmd in (0xC, 0x80000018, 0x8000001F, 0x80000023):
                self.dependencies.append(cstring(command, unpack(command, "<I", 8)[0]))
            elif cmd == 0x32:
                self.platforms.append(unpack(command, "<I", 8)[0])
            elif cmd in (0x22, 0x80000022):
                self.export_range = unpack(command, "<II", 40)
            elif cmd == 0x80000033:
                self.export_range = unpack(command, "<II", 8)
            elif cmd == 0x80000034:
                self.fixup_range = unpack(command, "<II", 8)
            position += size
        if position != self.command_end:
            raise IosError("Mach-O command count/size mismatch")
        self.base = next((s["vm"] for s in self.segments if s["offset"] == 0 and s["size"]), 0)
        self._pointers: dict[int, int | None] | None = None

    def vm_to_file(self, address: int, size: int = 1) -> int:
        for segment in self.segments:
            delta = address - segment["vm"]
            if 0 <= delta and delta + size <= segment["size"]:
                return segment["offset"] + delta
        raise IosError(f"virtual address is not file-backed: {address:#x}")

    def file_to_vm(self, offset: int) -> int:
        for segment in self.segments:
            if segment["offset"] <= offset < segment["offset"] + segment["size"]:
                return segment["vm"] + offset - segment["offset"]
        raise IosError(f"file offset is not mapped: {offset:#x}")

    def is_code(self, address: int) -> bool:
        return any(s["vm"] <= address < s["vm"] + s["size"] and s["flags"] & 0x80000400 for s in self.sections)

    def exports(self) -> list[str]:
        if not self.export_range or not self.export_range[1]:
            return []
        blob = region(self.data, *self.export_range)
        result: list[str] = []

        def uleb(position: int) -> tuple[int, int]:
            value = 0
            for shift in range(0, 64, 7):
                byte = region(blob, position, 1)[0]
                position += 1
                value |= (byte & 127) << shift
                if byte < 128:
                    return value, position
            raise IosError("invalid export ULEB128")

        def walk(position: int, prefix: str, ancestors: frozenset[int]):
            if position in ancestors or len(ancestors) > 128:
                raise IosError("cyclic/deep export trie")
            length, cursor = uleb(position)
            region(blob, cursor, length)
            if length:
                result.append(prefix)
            cursor += length
            count = region(blob, cursor, 1)[0]
            cursor += 1
            for _ in range(count):
                label = cstring(blob, cursor)
                cursor += len(label.encode()) + 1
                child, cursor = uleb(cursor)
                walk(child, prefix + label, ancestors | {position})

        walk(0, "", frozenset())
        return result

    def pointers(self) -> dict[int, int | None]:
        if self._pointers is not None:
            return self._pointers
        pointers: dict[int, int | None] = {}
        if not self.fixup_range:
            self._pointers = pointers
            return pointers
        blob = region(self.data, *self.fixup_range)
        version, starts, _, _, _, _, _ = unpack(blob, "<7I", 0)
        if version != 0:
            raise IosError(f"unsupported chained fixups version: {version}")
        count = unpack(blob, "<I", starts)[0]
        if count > len(self.segments):
            raise IosError("chained fixups segment count mismatch")
        offsets = unpack(blob, f"<{count}I", starts + 4)
        for index, relative in enumerate(offsets):
            if not relative:
                continue
            begin = starts + relative
            size, page_size, fmt, vm_offset, _, page_count = unpack(blob, "<IHHQIH", begin)
            record = region(blob, begin, size)
            if fmt not in (2, 6):
                raise IosError(f"unsupported chained pointer format: {fmt}")
            if not page_size:
                raise IosError("zero chained page size")
            segment = self.segments[index]
            if segment["vm"] - self.base != vm_offset:
                raise IosError("chained segment virtual offset mismatch")
            for page in range(page_count):
                first = unpack(record, "<H", 22 + page * 2)[0]
                if first == 0xFFFF:
                    continue
                # MULTI uses an overflow-list index within page_start[].
                firsts = [first]
                if first & 0x8000:
                    firsts = []
                    cursor = first & 0x7FFF
                    while True:
                        value = unpack(record, "<H", 22 + cursor * 2)[0]
                        firsts.append(value & 0x7FFF)
                        cursor += 1
                        if value & 0x8000:
                            break
                page_begin = segment["offset"] + page * page_size
                for first in firsts:
                    position = page_begin + first
                    while True:
                        if not page_begin <= position or position + 8 > min(page_begin + page_size, segment["offset"] + segment["size"]):
                            raise IosError("chained pointer escapes its page/segment")
                        if position in pointers:
                            raise IosError("overlapping chained pointer lists")
                        raw = unpack(self.data, "<Q", position)[0]
                        if raw >> 63:
                            pointers[position] = None  # imported symbol, not a local pointer
                        else:
                            if (raw >> 44) & 0x7F:
                                raise IosError("nonzero reserved chained-pointer bits")
                            target = (raw & ((1 << 36) - 1)) | (((raw >> 36) & 0xFF) << 56)
                            pointers[position] = target + (self.base if fmt == 6 else 0)
                        following = (raw >> 51) & 0xFFF
                        if not following:
                            break
                        position += following * 4
        self._pointers = pointers
        return pointers

    def pointer(self, offset: int) -> int | None:
        pointers = self.pointers()
        if offset in pointers:
            return pointers[offset]
        raw = unpack(self.data, "<Q", offset)[0]
        if self.fixup_range and raw:
            raise IosError("nonzero pointer is absent from chained fixups")
        return raw

    def references(self) -> dict[int, list[int]]:
        refs: dict[int, list[int]] = defaultdict(list)
        for offset, address in self.pointers().items():
            if address is not None:
                refs[address].append(offset)
        return refs

    def summary(self) -> dict:
        exported = self.exports()
        return dict(architecture="arm64", cpuSubtype=self.subtype, fileType=self.filetype,
                    platforms=self.platforms, encryption=self.encryption,
                    dependencies=self.dependencies, exportedSymbolCount=len(exported),
                    il2cppExports=[n for n in exported if n.lstrip("_").startswith("il2cpp_")],
                    hasChainedFixups=bool(self.fixup_range))


def insert_dylib(data: bytes, install_name: str) -> bytes:
    image = MachO(data)
    if image.filetype != 2 or any(e["cryptid"] for e in image.encryption):
        raise IosError("load command insertion requires an unencrypted executable")
    if install_name in image.dependencies:
        raise IosError("probe dependency is already present")
    if not install_name.startswith("@executable_path/Frameworks/") or "\0" in install_name:
        raise IosError("probe install name must be an executable-relative Frameworks path")
    name = install_name.encode() + b"\0"
    length = (24 + len(name) + 7) & ~7
    section_offsets = [s["offset"] for s in image.sections if s["offset"] and s["size"]]
    if not section_offsets:
        raise IosError("cannot determine Mach-O header padding")
    available = min(section_offsets) - image.command_end
    if length > available or any(region(data, image.command_end, length)):
        raise IosError("insufficient zero-filled Mach-O header padding; relocation is not supported")
    result = bytearray(data)
    command = struct.pack("<6I", 0xC, length, 24, 0, 0, 0) + name
    result[image.command_end:image.command_end + length] = command.ljust(length, b"\0")
    struct.pack_into("<II", result, 16, len(image.commands) + 1, image.command_end - 32 + length)
    if install_name not in MachO(bytes(result)).dependencies:
        raise IosError("injected dependency verification failed")
    return bytes(result)
