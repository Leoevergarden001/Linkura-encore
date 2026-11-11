from __future__ import annotations

import hashlib
import json
import plistlib
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.linkura_offline.ios_client import (
    PROBE_INSTALL_NAME, inspect_ios_client, stage_ios_client, write_ios_inspection,
)
from tools.linkura_offline.ios_macho import IosError, MachO, insert_dylib
from tools.linkura_offline.ios_metadata import Metadata31, map_methods


def macho(*, filetype: int = 6, cryptid: int = 0, pointer_format: int = 6, platform: int = 2) -> bytes:
    data = bytearray(0x3000)
    commands = []
    for index, (name, vm, offset) in enumerate((("__TEXT", 0, 0), ("__DATA_CONST", 0x1000, 0x1000), ("__LINKEDIT", 0x2000, 0x2000))):
        sections = 1 if index == 0 else 0
        command = struct.pack("<II16sQQQQiiII", 0x19, 72 + sections * 80, name.encode(), vm, 0x1000, offset, 0x1000, 7, 5 if index == 0 else 3, sections, 0)
        if sections:
            command += struct.pack("<16s16sQQIIIIIIII", b"__text", b"__TEXT", 0x400, 0x800, 0x400, 2, 0, 0, 0x80000400, 0, 0, 0)
        commands.append(command)
    commands += [struct.pack("<6I", 0x32, 24, platform, 15 << 16, 18 << 16, 0),
                 struct.pack("<6I", 0x2C, 24, 0x400, 0x800, cryptid, 0),
                 struct.pack("<4I", 0x80000034, 16, 0x2000, 72),
                 struct.pack("<4I", 0x80000033, 16, 0x2100, 2)]
    body = b"".join(commands)
    data[:32] = struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, filetype, len(commands), len(body), 0, 0)
    data[32:32 + len(body)] = body
    data[0x400:0x410] = bytes.fromhex("fd7bbfa9fd030091fd7bc1a8c0035fd6")
    text = b"Assembly-CSharp.dll\0"
    data[0xC00:0xC00 + len(text)] = text
    struct.pack_into("<QQQ", data, 0x1000, 0xC00 | (4 << 51), 1, 0x1040 | (12 << 51))
    struct.pack_into("<Q", data, 0x1040, 0x400)
    fixups = struct.pack("<7I", 0, 32, 72, 72, 0, 1, 0) + b"\0" * 4
    fixups += struct.pack("<4I", 3, 0, 16, 0)
    fixups += struct.pack("<IHHQIHH", 24, 0x1000, pointer_format, 0x1000, 0, 1, 0)
    data[0x2000:0x2048] = fixups
    return bytes(data)


def metadata() -> bytes:
    data = bytearray(256)
    struct.pack_into("<II", data, 0, 0xFAB11BAF, 31)
    strings = bytearray(b"\0")
    indexes = {}
    for name in ("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "AddDefaultHeader"):
        indexes[name] = len(strings)
        strings.extend(name.encode() + b"\0")
    method = struct.pack("<IiiiiiIHHHH", indexes["AddDefaultHeader"], 0, 0, 0, 0, -1, 0x06000001, 6, 0, 0xFFFF, 2)
    type_record = bytearray(88)
    struct.pack_into("<II", type_record, 0, indexes["Configuration"], indexes["Org.OpenAPITools.Client"])
    struct.pack_into("<i", type_record, 36, 0)
    struct.pack_into("<H", type_record, 64, 1)
    image = struct.pack("<IiiIIiIIII", indexes["Assembly-CSharp.dll"], 0, 0, 1, 0, 0, 0, 1, 0, 0)
    for index, blob in ((2, strings), (5, method), (19, type_record), (20, image)):
        struct.pack_into("<II", data, 8 + index * 8, len(data), len(blob))
        data.extend(blob)
    return bytes(data)


def ipa(destination: Path, *, encrypted: bool = False, extra: dict[str, bytes] | None = None) -> None:
    app = "Payload/Game.app/"
    with zipfile.ZipFile(destination, "w") as archive:
        archive.writestr(app + "Info.plist", plistlib.dumps(dict(CFBundleIdentifier="com.oddno.lovelive", CFBundleExecutable="Game", CFBundleShortVersionString="5.0.1", CFBundleVersion="109072")))
        archive.writestr(app + "Game", macho(filetype=2))
        archive.writestr(app + "Frameworks/UnityFramework.framework/UnityFramework", macho(cryptid=int(encrypted)))
        archive.writestr(app + "Data/Managed/Metadata/global-metadata.dat", metadata())
        archive.writestr(app + "Data/raw-resource", b"preserve these bytes")
        archive.writestr(app + "_CodeSignature/CodeResources", b"old signature")
        archive.writestr(app + "embedded.mobileprovision", b"old provisioning")
        for name, data in (extra or {}).items():
            archive.writestr(name, data)


class IosBinaryTests(unittest.TestCase):
    def test_resolves_chained_pointer_and_method_token(self):
        image = MachO(macho())
        self.assertEqual(image.pointer(0x1000), 0xC00)
        self.assertEqual(image.pointer(0x1010), 0x1040)
        methods = map_methods(Metadata31(metadata()), image)
        self.assertEqual(len(methods), 1)
        self.assertEqual(methods[0]["imageRelativeAddress"], "0x400")
        self.assertEqual(methods[0]["token"], "0x06000001")
        self.assertEqual(methods[0]["parameterCount"], 2)
        self.assertFalse(methods[0]["runtimeValidated"])

    def test_rejects_unknown_pointer_encoding(self):
        with self.assertRaisesRegex(IosError, "pointer format"):
            MachO(macho(pointer_format=12)).pointers()

    def test_rejects_chain_escaping_page(self):
        data = bytearray(macho())
        struct.pack_into("<Q", data, 0x1000, 0xC00 | (0xFFF << 51))
        with self.assertRaisesRegex(IosError, "escapes"):
            MachO(bytes(data)).pointers()

    def test_rejects_non_fixup_nonzero_pointer(self):
        with self.assertRaisesRegex(IosError, "absent"):
            MachO(macho()).pointer(0xC00)

    def test_rejects_bad_metadata_ranges_and_version(self):
        data = bytearray(metadata())
        struct.pack_into("<I", data, 8 + 19 * 8, len(data) + 1)
        with self.assertRaises(IosError):
            Metadata31(bytes(data))
        struct.pack_into("<I", data, 4, 29)
        with self.assertRaisesRegex(IosError, "v31"):
            Metadata31(bytes(data))

    def test_load_command_preserves_instruction_addresses(self):
        original = macho(filetype=2)
        modified = insert_dylib(original, PROBE_INSTALL_NAME)
        self.assertIn(PROBE_INSTALL_NAME, MachO(modified).dependencies)
        self.assertEqual(len(original), len(modified))
        self.assertEqual(original[0x400:], modified[0x400:])
        with self.assertRaisesRegex(IosError, "already"):
            insert_dylib(modified, PROBE_INSTALL_NAME)

    def test_load_command_rejects_encryption_and_nonzero_padding(self):
        with self.assertRaisesRegex(IosError, "unencrypted"):
            insert_dylib(macho(filetype=2, cryptid=1), PROBE_INSTALL_NAME)
        data = bytearray(macho(filetype=2))
        data[MachO(bytes(data)).command_end] = 1
        with self.assertRaisesRegex(IosError, "padding"):
            insert_dylib(bytes(data), PROBE_INSTALL_NAME)

    def test_rejects_truncated_and_invalid_commands(self):
        with self.assertRaises(IosError):
            MachO(b"bad")
        data = bytearray(macho())
        struct.pack_into("<I", data, 36, 0xFFFF)
        with self.assertRaisesRegex(IosError, "load command"):
            MachO(bytes(data))


class IosPackageTests(unittest.TestCase):
    def test_inspection_records_static_mapping_and_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / "input.ipa", root / "report.json"
            ipa(source)
            report = write_ios_inspection(source, output)
            self.assertEqual(report["source"]["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertFalse(report["gates"]["loginVerified"])
            self.assertEqual(len(report["missingTargets"]), 6)
            self.assertEqual(json.loads(output.read_text())["methods"][0]["fileOffset"], "0x400")
            with self.assertRaisesRegex(IosError, "overwrite"):
                write_ios_inspection(source, output)

    def test_encrypted_framework_is_reported_but_cannot_be_staged(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "encrypted.ipa"
            ipa(source, encrypted=True)
            self.assertFalse(inspect_ios_client(source)["gates"]["encryptionMarkerClear"])
            with self.assertRaisesRegex(IosError, "encrypted executable"):
                stage_ios_client(source, root / "output.ipa", bundle_id="org.example.lab")
            self.assertFalse((root / "output.ipa").exists())

    def test_probe_stage_preserves_resources_and_original(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output, probe = root / "input.ipa", root / "output.ipa", root / "probe.dylib"
            ipa(source)
            before = source.read_bytes()
            probe.write_bytes(macho())
            report = stage_ios_client(source, output, bundle_id="org.example.lab", probe=probe)
            self.assertEqual(source.read_bytes(), before)
            self.assertTrue(report["signingRequired"])
            self.assertFalse(report["compatibilityPatched"])
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.read("Payload/Game.app/Data/raw-resource"), b"preserve these bytes")
                self.assertNotIn("Payload/Game.app/_CodeSignature/CodeResources", archive.namelist())
                self.assertNotIn("Payload/Game.app/embedded.mobileprovision", archive.namelist())
                self.assertIn(PROBE_INSTALL_NAME, MachO(archive.read("Payload/Game.app/Game")).dependencies)
                self.assertEqual(plistlib.loads(archive.read("Payload/Game.app/Info.plist"))["CFBundleIdentifier"], "org.example.lab")
            with self.assertRaisesRegex(IosError, "overwrite"):
                stage_ios_client(source, output, bundle_id="org.example.lab", probe=probe)

    def test_baseline_stage_does_not_add_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, output = root / "input.ipa", root / "output.ipa"
            ipa(source)
            stage_ios_client(source, output, bundle_id="org.example.lab")
            with zipfile.ZipFile(output) as archive:
                self.assertNotIn(PROBE_INSTALL_NAME, MachO(archive.read("Payload/Game.app/Game")).dependencies)

    def test_rejects_official_bundle_id_and_simulator_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, probe = root / "input.ipa", root / "sim.dylib"
            ipa(source)
            with self.assertRaisesRegex(IosError, "differ"):
                stage_ios_client(source, root / "output.ipa", bundle_id="com.oddno.lovelive")
            probe.write_bytes(macho(platform=7))
            with self.assertRaisesRegex(IosError, "iPhoneOS dylib"):
                stage_ios_client(source, root / "output.ipa", bundle_id="org.example.lab", probe=probe)

    def test_rejects_unsafe_archive_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, name in enumerate(("../../escape", "/absolute", "Payload/Game.app/../escape", "Payload\\escape")):
                source = root / f"bad{index}.ipa"
                ipa(source, extra={name: b"bad"})
                with self.assertRaisesRegex(IosError, "unsafe"):
                    inspect_ios_client(source)

    def test_rejects_symbolic_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "link.ipa"
            ipa(source)
            with zipfile.ZipFile(source, "a") as archive:
                link = zipfile.ZipInfo("Payload/Game.app/link")
                link.external_attr = 0o120777 << 16
                archive.writestr(link, b"../../escape")
            with self.assertRaisesRegex(IosError, "symlink"):
                inspect_ios_client(source)


if __name__ == "__main__":
    unittest.main()
