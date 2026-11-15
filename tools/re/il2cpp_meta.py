"""List types, fields and properties from an IL2CPP global-metadata.dat (v29-v31).

Reads only the metadata tables (names, field/property lists, enum constants);
it does not resolve field types or offsets, which live in the binary.

    il2cpp_meta.py global-metadata.dat types 'GpPrizeExchange.*'
    il2cpp_meta.py global-metadata.dat fields TPopupGpExchangeConfirm
    il2cpp_meta.py global-metadata.dat enum GachaExchangeType
    il2cpp_meta.py global-metadata.dat props-with 'SelectTicket'
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

TABLES = ["stringLiteral", "stringLiteralData", "string", "events", "properties", "methods",
          "parameterDefaultValues", "fieldDefaultValues", "fieldAndParameterDefaultValueData",
          "fieldMarshaledSizes", "parameters", "fields", "genericParameters",
          "genericParameterConstraints", "genericContainers", "nestedTypes", "interfaces",
          "vtableMethods", "interfaceOffsets", "typeDefinitions", "images", "assemblies"]
TYPE_DEF_SIZE = 88  # v31: 16 int32, 8 uint16, bitfield, token


class Metadata:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        sanity, self.version = struct.unpack_from("<Ii", self.data, 0)
        if sanity != 0xFAB11BAF:
            raise SystemExit(f"{path}: not an IL2CPP metadata file")
        header = struct.unpack_from(f"<{2 * len(TABLES)}i", self.data, 8)
        self.tables = {name: (header[2 * i], header[2 * i + 1]) for i, name in enumerate(TABLES)}
        offset, size = self.tables["typeDefinitions"]
        self.types = []
        for index in range(size // TYPE_DEF_SIZE):
            v = struct.unpack_from("<16i8H2I", self.data, offset + index * TYPE_DEF_SIZE)
            self.types.append({"name": self.string(v[0]), "namespace": self.string(v[1]),
                               "fieldStart": v[8], "propertyStart": v[11],
                               "propertyCount": v[17], "fieldCount": v[18]})

    def string(self, index: int) -> str:
        start = self.tables["string"][0] + index
        return self.data[start:self.data.index(b"\0", start)].decode()

    def fields(self, t: dict) -> list[tuple[int, str]]:
        base = self.tables["fields"][0]
        out = []
        for j in range(t["fieldCount"]):
            index = t["fieldStart"] + j
            out.append((index, self.string(struct.unpack_from("<3i", self.data, base + index * 12)[0])))
        return out

    def properties(self, t: dict) -> list[str]:
        base = self.tables["properties"][0]
        return [self.string(struct.unpack_from("<5i", self.data, base + (t["propertyStart"] + j) * 20)[0])
                for j in range(t["propertyCount"])]

    def enum_values(self, t: dict) -> dict[str, int]:
        """Enum constants. v31 stores them as zigzag-encoded compressed integers."""
        wanted = {index: name for index, name in self.fields(t)}
        offset, size = self.tables["fieldDefaultValues"]
        blob = self.tables["fieldAndParameterDefaultValueData"][0]
        values = {}
        for k in range(size // 12):
            field, _type, data_index = struct.unpack_from("<3i", self.data, offset + k * 12)
            if field in wanted:
                raw, _ = read_compressed(self.data, blob + data_index)
                values[wanted[field]] = (raw >> 1) ^ -(raw & 1)
        return values

    def find(self, pattern: str) -> list[dict]:
        return [t for t in self.types if re.fullmatch(pattern, t["name"])]


def read_compressed(data: bytes, offset: int) -> tuple[int, int]:
    first = data[offset]
    if first & 0x80 == 0:
        return first, 1
    if first & 0xC0 == 0x80:
        return ((first & 0x3F) << 8) | data[offset + 1], 2
    if first & 0xE0 == 0xC0:
        return ((first & 0x1F) << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3], 4
    return struct.unpack_from("<I", data, offset + 1)[0], 5


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("metadata", type=Path)
    ap.add_argument("command", choices=["types", "fields", "enum", "props-with"])
    ap.add_argument("pattern")
    args = ap.parse_args()
    meta = Metadata(args.metadata)
    if args.command == "props-with":
        for t in meta.types:
            props = meta.properties(t)
            if any(re.search(args.pattern, p) for p in props):
                print(f"{t['namespace']}.{t['name']}: {props}")
        return
    for t in meta.find(args.pattern):
        label = f"{t['namespace']}.{t['name']}" if t["namespace"] else t["name"]
        if args.command == "types":
            print(f"{label}: props {meta.properties(t)}")
        elif args.command == "fields":
            print(f"{label}: fields {[name for _, name in meta.fields(t)]}")
        else:
            print(f"{label}: {meta.enum_values(t)}")


if __name__ == "__main__":
    sys.exit(main())
