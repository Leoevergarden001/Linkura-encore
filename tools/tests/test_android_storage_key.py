import json
import tempfile
import unittest
from pathlib import Path

from tools.linkura_offline.android_storage_key import (
    STORAGE_KEY_NAMES,
    StorageKeyError,
    export_storage_key,
)

KEY = STORAGE_KEY_NAMES[0]

PREFS = f"""<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="y%2BBv%2Bi7aGj9up6h6jXWU8u7b6F8%3D">player-id-secret</string>
    <string name="{KEY}">storage%2Bkey%3D&amp;x</string>
    <int name="__UNITY_PLAYERPREFS_VERSION__" value="1" />
</map>
"""


class StorageKeyExportTest(unittest.TestCase):
    def test_exports_only_storage_key_entry_unescaped(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefs = Path(tmp, "prefs.xml")
            prefs.write_text(PREFS, encoding="utf-8")
            out = Path(tmp, "pack/linkura-local/storage-key.json")
            export_storage_key(prefs, out)
            payload = json.loads(out.read_text())
            self.assertEqual(payload["prefs"], "com.oddno.lovelive.v2.playerprefs")
            self.assertEqual(payload["entries"], {KEY: "storage%2Bkey%3D&x"})
            self.assertNotIn("player-id-secret", out.read_text())
            self.assertEqual(out.stat().st_mode & 0o777, 0o600)

    def test_missing_entry_and_existing_output_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefs = Path(tmp, "prefs.xml")
            prefs.write_text("<map><int name='a' value='1'/></map>", encoding="utf-8")
            with self.assertRaises(StorageKeyError):
                export_storage_key(prefs, Path(tmp, "out.json"))
            prefs.write_text(PREFS, encoding="utf-8")
            out = Path(tmp, "out.json")
            out.write_text("{}")
            with self.assertRaises(StorageKeyError):
                export_storage_key(prefs, out)
            export_storage_key(prefs, out, force=True)


if __name__ == "__main__":
    unittest.main()
