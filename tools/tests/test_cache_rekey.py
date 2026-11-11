import base64
import hashlib
import io
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import urllib.parse
from pathlib import Path

from tools.linkura_offline.android_resource_pack import build_resource_pack
from tools.linkura_offline.cache_rekey import (
    CATALOG_PREFS_KEY,
    RekeyError,
    decode_seed,
    encode_seed,
    rekey_catalog,
    sqlcipher_key,
    storage_name,
)

HAVE_SQLCIPHER = shutil.which("sqlcipher") is not None


def _sql(db: Path, seed: bytes, statements: str) -> str:
    script = f"PRAGMA key = \"x'{sqlcipher_key(seed)}'\";\n{statements}\n"
    return subprocess.run(["sqlcipher", "-batch", "-bail", str(db)], input=script, text=True,
                          capture_output=True, check=True).stdout


def _make_catalog(m_dir: Path, seed: bytes) -> Path:
    m_dir.mkdir(parents=True, exist_ok=True)
    db = m_dir / storage_name(seed)
    _sql(db, seed, "CREATE TABLE A(k TEXT); CREATE TABLE M(v TEXT); INSERT INTO A VALUES('asset');")
    return db


def _key_file(path: Path, seed: bytes) -> Path:
    path.write_text(json.dumps({"prefs": "ios", "entries": {CATALOG_PREFS_KEY: encode_seed(seed)}}))
    return path


class DerivationTest(unittest.TestCase):
    def test_name_and_key(self):
        seed = bytes(range(256))
        name = storage_name(seed)
        self.assertEqual(len(name), 26)
        self.assertEqual(name, base64.b32encode(hashlib.md5(seed).digest()).decode().rstrip("=").lower())
        self.assertEqual(sqlcipher_key(seed), hashlib.sha256(seed).hexdigest())

    def test_seed_round_trip_and_length(self):
        seed = os.urandom(256)
        encoded = encode_seed(seed)
        self.assertNotIn("+", encoded)
        self.assertEqual(urllib.parse.unquote(encoded), base64.b64encode(seed).decode())
        self.assertEqual(decode_seed(encoded), seed)
        with self.assertRaises(RekeyError):
            decode_seed(base64.b64encode(b"short").decode())


@unittest.skipUnless(HAVE_SQLCIPHER, "sqlcipher not installed")
class RekeyTest(unittest.TestCase):
    def test_rekey_writes_fresh_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            old = os.urandom(256)
            source = _make_catalog(t / "M", old)
            (t / "M" / "otherdb").write_bytes(b"player data")
            result = rekey_catalog(t / "M", _key_file(t / "key.json", old), t / "out")
            payload = json.loads(result.storage_key.read_text())
            self.assertEqual(list(payload["entries"]), [CATALOG_PREFS_KEY])
            new = decode_seed(payload["entries"][CATALOG_PREFS_KEY])
            self.assertNotEqual(new, old)
            self.assertEqual(result.database, t / "out" / "M" / storage_name(new))
            self.assertEqual([p.name for p in (t / "out" / "M").iterdir()], [storage_name(new)])
            self.assertIn("asset", _sql(result.database, new, "SELECT k FROM A;"))
            self.assertEqual(result.tables, 2)
            self.assertTrue(source.exists())  # the source cache is untouched
            with self.assertRaises(subprocess.CalledProcessError):
                _sql(result.database, old, "SELECT k FROM A;")
            with self.assertRaises(RekeyError):
                rekey_catalog(t / "M", t / "key.json", t / "out")

    def test_missing_catalog(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / "M").mkdir()
            with self.assertRaises(RekeyError):
                rekey_catalog(t / "M", _key_file(t / "key.json", os.urandom(256)), t / "out")

    def test_resource_pack_keeps_only_rekeyed_catalog(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            old = os.urandom(256)
            catalog = _make_catalog(t / "src" / "M", old)
            members = {"./D/ab/ab1": b"1", f"./M/{catalog.name}": catalog.read_bytes(), "./M/7rtmzr": b"identity"}
            prefs = (f"<map><string name=\"y%2BBv%2Bi7aGj9up6h6jXWU8u7b6F8%3D\">x</string>"
                     f"<string name=\"{CATALOG_PREFS_KEY}\">{encode_seed(old)}</string></map>").encode()
            tars = {}
            for name, content in (("snap", members),
                                  ("priv", {"./shared_prefs/com.oddno.lovelive.v2.playerprefs.xml": prefs})):
                tars[name] = t / f"{name}.tar"
                with tarfile.open(tars[name], "w") as archive:
                    for member, data in content.items():
                        info = tarfile.TarInfo(member)
                        info.size = len(data)
                        archive.addfile(info, io.BytesIO(data))
            summary = build_resource_pack(tars["snap"], tars["priv"], t / "pack")
            key = json.loads((t / "pack/files/linkura-local/storage-key.json").read_text())
            new = decode_seed(key["entries"][CATALOG_PREFS_KEY])
            self.assertEqual([p.name for p in (t / "pack/files/M").iterdir()], [storage_name(new)])
            self.assertEqual((summary.d_files, summary.m_files), (1, 1))
            self.assertTrue(json.loads((t / "pack/MANIFEST.json").read_text())["fresh_storage_key"])


if __name__ == "__main__":
    unittest.main()
