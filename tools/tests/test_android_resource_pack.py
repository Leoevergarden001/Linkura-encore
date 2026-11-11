import io
import json
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from tools.linkura_offline.android_resource_pack import (
    DEVICE_FILES,
    ResourcePackError,
    _cache_member,
    build_resource_pack,
    install_resource_pack,
)
from tools.linkura_offline.android_storage_key import STORAGE_KEY_NAMES

KEY = STORAGE_KEY_NAMES[0]


def _tar(path: Path, members: dict[str, bytes]) -> Path:
    with tarfile.open(path, "w") as archive:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return path


PREFS = f"""<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="y%2BBv%2Bi7aGj9up6h6jXWU8u7b6F8%3D">player</string>
    <string name="{KEY}">key</string>
</map>
""".encode()


class CacheMemberTest(unittest.TestCase):
    def test_filters(self):
        self.assertEqual(str(_cache_member("./D/ab/abcd")), "D/ab/abcd")
        self.assertEqual(str(_cache_member("M/xyz")), "M/xyz")
        self.assertIsNone(_cache_member("./il2cpp/Metadata/x"))
        self.assertIsNone(_cache_member("./D/../etc"))
        self.assertIsNone(_cache_member("./D/ab/._abcd"))


class BuildTest(unittest.TestCase):
    def test_build_merges_delta_and_exports_key_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            snap = _tar(t / "snap.tar", {"./D/ab/ab1": b"1", "./M/m1": b"old", "./Unity/x": b"u"})
            delta = _tar(t / "delta.tar", {"D/cd/cd2": b"22", "M/m1": b"new"})
            private = _tar(t / "priv.tar", {"./shared_prefs/com.oddno.lovelive.v2.playerprefs.xml": PREFS})
            out = t / "pack"
            summary = build_resource_pack(snap, private, out, [delta], fresh_key=False)
            self.assertEqual((summary.d_files, summary.m_files), (2, 1))
            self.assertEqual((out / "files/M/m1").read_bytes(), b"new")
            self.assertFalse((out / "files/Unity").exists())
            key = json.loads((out / "files/linkura-local/storage-key.json").read_text())
            self.assertEqual(key["entries"], {KEY: "key"})
            manifest = json.loads((out / "MANIFEST.json").read_text())
            self.assertEqual(manifest["d_files"], 2)
            with self.assertRaises(ResourcePackError):
                build_resource_pack(snap, private, out, [delta], fresh_key=False)


class InstallTest(unittest.TestCase):
    def test_install_launches_once_then_pushes(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            snap = _tar(t / "snap.tar", {"./D/ab/ab1": b"1", "./M/m1": b"m"})
            private = _tar(t / "priv.tar", {"./shared_prefs/com.oddno.lovelive.v2.playerprefs.xml": PREFS})
            pack = t / "pack"
            build_resource_pack(snap, private, pack, fresh_key=False)
            calls = []
            state = {"dirs": False}

            def runner(cmd):
                calls.append(cmd)
                args = cmd[3:]
                out = ""
                if args[:3] == ["shell", "pm", "list"]:
                    out = "package:com.oddno.lovelive\n"
                elif args[0] == "shell" and "ls -d" in args[-1]:
                    out = f"{DEVICE_FILES}/D/77\n" if state["dirs"] else "missing\n"
                elif args[:2] == ["shell", "am"] and args[2] == "start":
                    state["dirs"] = True
                elif args[0] == "shell" and "wc -l" in args[-1]:
                    out = "1\n"
                return subprocess.CompletedProcess(cmd, 0, out)

            install_resource_pack(pack, "emu", runner=runner, first_launch_wait=0, log=lambda _m: None)
            pushes = [c[4] for c in calls if c[3] == "push"]
            self.assertEqual([p.rsplit("/", 2)[-2:] for p in pushes],
                             [["M", "."], ["linkura-local", "."], ["D", "."]])
            self.assertTrue(any(c[3:6] == ["shell", "am", "start"] for c in calls))


if __name__ == "__main__":
    unittest.main()
