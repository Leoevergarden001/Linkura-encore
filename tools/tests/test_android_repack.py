from __future__ import annotations

import json
import hashlib
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Sequence

from tools.linkura_offline.android_repack import (
    AndroidRepackError,
    repack_android_client,
)


def make_apk(path: Path, extra: dict[str, bytes] | None = None) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("AndroidManifest.xml", b"manifest")
        for name, content in (extra or {}).items():
            archive.writestr(name, content)


class FakeLSPatch:
    def __init__(self, *, produce_output: bool = True) -> None:
        self.commands: list[list[str]] = []
        self.produce_output = produce_output

    def __call__(self, command: Sequence[str]) -> subprocess.CompletedProcess[str]:
        values = list(command)
        self.commands.append(values)
        if self.produce_output:
            module = Path(values[values.index("-m") + 1])
            output_dir = Path(values[values.index("-o") + 1])
            origin = Path(values[-1])
            make_apk(
                output_dir / "origin-435-lspatched.apk",
                {
                    "assets/lspatch/config.json": json.dumps(
                        {"useManager": False, "debuggable": "-d" in values}
                    ).encode(),
                    "assets/lspatch/origin.apk": origin.read_bytes(),
                    "assets/lspatch/modules/localify.apk": module.read_bytes(),
                },
            )
        return subprocess.CompletedProcess(values, 0, "Done", None)


class AndroidRepackTests(unittest.TestCase):
    def test_extracts_clean_origin_before_repatching(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            clean = root / "clean.apk"
            module = root / "module.apk"
            source = root / "old-patched.apk"
            patcher = root / "lspatch.jar"
            output = root / "new.apk"
            make_apk(clean, {"assets/game.dat": b"game"})
            make_apk(module, {"classes.dex": b"module"})
            make_apk(
                source,
                {
                    "assets/lspatch/config.json": b"{}",
                    "assets/lspatch/origin.apk": clean.read_bytes(),
                    "assets/lspatch/modules/old.apk": b"old-module",
                },
            )
            patcher.write_bytes(b"jar")
            runner = FakeLSPatch()

            result = repack_android_client(
                source_apk=source,
                module_apk=module,
                lspatch_jar=patcher,
                output_apk=output,
                runner=runner,
            )

            self.assertTrue(result.source_contained_origin)
            self.assertTrue(output.is_file())
            self.assertEqual(
                result.origin_sha256,
                hashlib.sha256(clean.read_bytes()).hexdigest(),
            )
            self.assertIn("-d", runner.commands[0])
            record = json.loads(result.manifest.read_text())
            self.assertEqual(record["artifact"]["sha256"], result.output_sha256)
            self.assertEqual(
                record["module"]["embeddedPath"],
                "assets/lspatch/modules/localify.apk",
            )

    def test_accepts_a_clean_source_and_can_disable_debuggable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "clean.apk"
            module = root / "module.apk"
            patcher = root / "lspatch.jar"
            make_apk(source)
            make_apk(module)
            patcher.touch()
            runner = FakeLSPatch()

            result = repack_android_client(
                source_apk=source,
                module_apk=module,
                lspatch_jar=patcher,
                output_apk=root / "output.apk",
                debuggable=False,
                runner=runner,
            )

            self.assertFalse(result.source_contained_origin)
            self.assertNotIn("-d", runner.commands[0])

    def test_refuses_existing_output_without_force(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "clean.apk"
            module = root / "module.apk"
            patcher = root / "lspatch.jar"
            output = root / "output.apk"
            make_apk(source)
            make_apk(module)
            patcher.touch()
            output.touch()
            with self.assertRaisesRegex(AndroidRepackError, "already exists"):
                repack_android_client(
                    source_apk=source,
                    module_apk=module,
                    lspatch_jar=patcher,
                    output_apk=output,
                    runner=FakeLSPatch(),
                )

    def test_requires_exactly_one_lspatch_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "clean.apk"
            module = root / "module.apk"
            patcher = root / "lspatch.jar"
            make_apk(source)
            make_apk(module)
            patcher.touch()
            with self.assertRaisesRegex(AndroidRepackError, "exactly one APK"):
                repack_android_client(
                    source_apk=source,
                    module_apk=module,
                    lspatch_jar=patcher,
                    output_apk=root / "output.apk",
                    runner=FakeLSPatch(produce_output=False),
                )


if __name__ == "__main__":
    unittest.main()
