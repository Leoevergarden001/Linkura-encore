"""Export the local-storage key entry that pairs with an Android asset cache.

The 5.1.0 client names and encrypts its external cache (files/D and files/M)
with a per-install local-storage key kept in its encrypted Unity PlayerPrefs
(shared_prefs/com.oddno.lovelive.v2.playerprefs.xml). A fresh install creates
its own key and ignores an imported cache. The Localify module seeds the
entry written here (linkura-local/storage-key.json next to D/ and M/) before
Unity reads PlayerPrefs.

Only the storage-key entry is exported. Other entries in the same file hold
account identity (for example the player id) and must not travel with a
resource pack. The exported value still comes from a real install, so treat
the output as private: keep it inside the resource pack, never in git.
"""

from __future__ import annotations

import json
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

PREFS_NAME = "com.oddno.lovelive.v2.playerprefs"
# Hashed (and URL-encoded) PlayerPrefs name of the local-storage key. Found on
# 2026-11-08 by moving single entries from the archived install to a fresh
# one: only this entry made the imported cache load, and it did not carry the
# player id.
STORAGE_KEY_NAMES = ("JWbAatNSI1NpHhCdU%2BgJllY0BTU%3D",)


class StorageKeyError(RuntimeError):
    pass


@dataclass(frozen=True)
class StorageKeyExport:
    output: Path
    names: tuple[str, ...]


def read_storage_key_entries(
    prefs_xml: Path, names: tuple[str, ...] = STORAGE_KEY_NAMES
) -> dict[str, str]:
    try:
        root = ET.parse(prefs_xml).getroot()
    except (OSError, ET.ParseError) as error:
        raise StorageKeyError(f"cannot read PlayerPrefs XML {prefs_xml}: {error}") from error
    found: dict[str, str] = {}
    for element in root:
        name = element.get("name")
        if element.tag == "string" and name in names:
            found[name] = element.text or ""
    missing = [name for name in names if name not in found or not found[name]]
    if missing:
        raise StorageKeyError(f"PlayerPrefs XML lacks storage key entries: {', '.join(missing)}")
    return found


def export_storage_key(
    prefs_xml: Path,
    output: Path,
    names: tuple[str, ...] = STORAGE_KEY_NAMES,
    force: bool = False,
) -> StorageKeyExport:
    entries = read_storage_key_entries(prefs_xml, names)
    if output.exists() and not force:
        raise StorageKeyError(f"{output} exists; pass force to overwrite")
    output.parent.mkdir(parents=True, exist_ok=True)
    payload = {"prefs": PREFS_NAME, "entries": entries}
    output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    output.chmod(0o600)
    return StorageKeyExport(output=output, names=tuple(entries))
