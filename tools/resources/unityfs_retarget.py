"""Retarget Unity asset bundles from one build target to another (Android -> iOS).

Unity refuses bundles whose serialized files carry a different build target
("File's Build target is: 13"). The target is one int32 after the Unity
version string in each serialized file's header. This tool changes it without
touching the rest of the bundle: only the LZ4 chunk that holds the header is
decompressed and stored uncompressed; all other chunks are copied as-is, and
the block table is rewritten (uncompressed).

Game cache files have a 2-byte prefix before "UnityFS"; it is preserved.
Textures, meshes and audio are portable between the two platforms. Shader
programs are not (GLES/Vulkan vs Metal), so materials using bundled shaders
may still render wrong.

Requires the `lz4` Python package.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import lz4.block

ANDROID, IOS = 13, 9


class RetargetError(ValueError):
    pass


def _cstring(data: bytes, offset: int) -> tuple[bytes, int]:
    end = data.index(b"\0", offset)
    return data[offset:end], end + 1


def _decompress(kind: int, data: bytes, size: int) -> bytes:
    if kind == 0:
        return data
    if kind in (2, 3):
        return lz4.block.decompress(data, uncompressed_size=size)
    raise RetargetError(f"unsupported compression {kind}")


def retarget(data: bytes, source: int = ANDROID, target: int = IOS) -> tuple[bytes, int]:
    """Return (new bytes, number of serialized files patched)."""
    prefix = data.find(b"UnityFS\0", 0, 16)
    if prefix < 0:
        raise RetargetError("not a UnityFS bundle")
    body = data[prefix:]
    pos = 8
    version = struct.unpack_from(">I", body, pos)[0]
    pos += 4
    _, pos = _cstring(body, pos)
    _, pos = _cstring(body, pos)
    header_strings_end = pos
    total_size, c_info, u_info, flags = struct.unpack_from(">qIII", body, pos)
    pos += 20
    if version >= 7:
        pos = (pos + 15) & ~15
    if flags & 0x80:
        info_raw = body[len(body) - c_info:]
        data_start = pos
    else:
        info_raw = body[pos:pos + c_info]
        data_start = pos + c_info
        if flags & 0x200:
            data_start = (data_start + 15) & ~15
    info = _decompress(flags & 0x3F, info_raw, u_info)
    block_count = struct.unpack_from(">i", info, 16)[0]
    blocks = [struct.unpack_from(">IIH", info, 20 + i * 10) for i in range(block_count)]
    p = 20 + block_count * 10
    node_count = struct.unpack_from(">i", info, p)[0]
    nodes_start = p
    p += 4
    nodes = []
    for _ in range(node_count):
        offset, size, node_flags = struct.unpack_from(">qqI", info, p)
        _, p = _cstring(info, p + 20)
        nodes.append((offset, size, node_flags))

    # Split the stored data into chunks and locate each chunk's plain range.
    chunks = []
    cursor = data_start
    plain = 0
    for u_size, c_size, b_flags in blocks:
        chunks.append([body[cursor:cursor + c_size], u_size, b_flags, plain])
        cursor += c_size
        plain += u_size

    def chunk_for(offset: int) -> int:
        for index, (_, u_size, _, start) in enumerate(chunks):
            if start <= offset < start + u_size:
                return index
        raise RetargetError("offset outside bundle data")

    def read_plain(index: int) -> bytearray:
        raw, u_size, b_flags, _ = chunks[index]
        return bytearray(_decompress(b_flags & 0x3F, raw, u_size))

    patched = 0
    opened: dict[int, bytearray] = {}
    for offset, size, node_flags in nodes:
        if not node_flags & 4:  # not a serialized file (.resS/.resource)
            continue
        index = chunk_for(offset)
        buf = opened.setdefault(index, read_plain(index))
        local = offset - chunks[index][3]
        file_version = struct.unpack_from(">I", buf, local + 8)[0]
        meta = local + (48 if file_version >= 22 else 20)
        end = buf.find(b"\0", meta, meta + 64)
        if end < 0 or end + 5 > len(buf):
            raise RetargetError("serialized file header spans chunks")
        little = buf[local + 16] == 0
        fmt = "<i" if little else ">i"
        current = struct.unpack_from(fmt, buf, end + 1)[0]
        if current == target:
            continue
        if current != source:
            raise RetargetError(f"unexpected build target {current}")
        struct.pack_into(fmt, buf, end + 1, target)
        patched += 1
    if not patched:
        return data, 0
    for index, buf in opened.items():
        chunks[index][0] = bytes(buf)
        chunks[index][2] &= ~0x3F  # stored uncompressed

    new_info = bytearray(info[:20])
    for raw, u_size, b_flags, _ in chunks:
        new_info += struct.pack(">IIH", u_size, len(raw), b_flags)
    new_info += info[nodes_start:]
    new_flags = (flags & ~0x3F) & ~0x80  # block table uncompressed, placed after the header
    out = bytearray(body[:header_strings_end])
    size_at = len(out)
    out += struct.pack(">qIII", 0, len(new_info), len(new_info), new_flags)
    if version >= 7:
        out += b"\0" * (-len(out) % 16)
    out += new_info
    if new_flags & 0x200:
        out += b"\0" * (-len(out) % 16)
    for raw, *_ in chunks:
        out += raw
    struct.pack_into(">q", out, size_at, len(out))
    return data[:prefix] + bytes(out), patched


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: unityfs_retarget.py IN OUT")
        return 2
    new, count = retarget(Path(argv[1]).read_bytes())
    Path(argv[2]).write_bytes(new)
    print(f"patched {count} serialized file(s): {argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
