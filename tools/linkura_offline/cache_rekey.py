"""Give an asset cache a fresh, unused local-storage key.

The 5.x client keeps one random 256-byte seed per storage domain in
PlayerPrefs (LocalDataStorageProvider, Core.dll):

- PlayerPrefs name: base64(HMAC-SHA1(<static key>, domain)), URL-encoded on disk.
  The catalog domain ("//catalog") is ``CATALOG_PREFS_KEY`` on every install.
- value: base64(seed), seed = 256 bytes from RNGCryptoServiceProvider.
- database file: ``M/`` + lowercase unpadded Base32(MD5(seed)).
- SQLCipher 4 raw key: SHA-256(seed) (Hailstorm.CatalogDBInitializer).

Only the catalog database is needed to reuse a cache: D/ files are encrypted
with per-asset catalog seeds, the master databases are rebuilt by the client,
and the "app" storage holds the source install's identity (player id) and
settings. ``rekey_catalog`` copies the catalog database under a newly
generated seed, so the output pairs with a storage key that never belonged to
any install. Needs the ``sqlcipher`` command line tool (SQLCipher 4).
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import urllib.parse
from dataclasses import dataclass
from pathlib import Path

CATALOG_PREFS_KEY = "JWbAatNSI1NpHhCdU%2BgJllY0BTU%3D"
SEED_BYTES = 256


class RekeyError(RuntimeError):
    pass


def storage_name(seed: bytes) -> str:
    return base64.b32encode(hashlib.md5(seed).digest()).decode("ascii").rstrip("=").lower()


def sqlcipher_key(seed: bytes) -> str:
    return hashlib.sha256(seed).hexdigest()


def encode_seed(seed: bytes) -> str:
    """PlayerPrefs value as stored in storage-key.json (URL-encoded base64)."""
    return urllib.parse.quote(base64.b64encode(seed).decode("ascii"), safe="")


def decode_seed(value: str) -> bytes:
    seed = base64.b64decode(urllib.parse.unquote(value), validate=True)
    if len(seed) != SEED_BYTES:
        raise RekeyError(f"storage seed is {len(seed)} bytes, expected {SEED_BYTES}")
    return seed


def read_catalog_seed(storage_key: Path) -> tuple[bytes, dict]:
    try:
        payload = json.loads(storage_key.read_text(encoding="utf-8"))
        value = payload["entries"][CATALOG_PREFS_KEY]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RekeyError(f"{storage_key} has no catalog storage key entry") from error
    return decode_seed(value), payload


@dataclass(frozen=True)
class RekeyResult:
    database: Path
    storage_key: Path
    tables: int


def _sqlcipher(binary: str, database: Path, script: str) -> str:
    # The key goes through stdin, never the command line.
    result = subprocess.run([binary, "-batch", "-bail", str(database)], input=script, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    if result.returncode != 0:
        raise RekeyError(f"sqlcipher failed: {result.stdout.strip()[-300:]}")
    return result.stdout


def _count_tables(binary: str, database: Path, seed: bytes) -> int:
    out = _sqlcipher(binary, database,
                     f"PRAGMA key = \"x'{sqlcipher_key(seed)}'\";\n"
                     "SELECT 'tables=' || count(*) FROM sqlite_master WHERE type = 'table';\n")
    for line in out.splitlines():
        if line.startswith("tables="):
            return int(line.split("=", 1)[1])
    raise RekeyError("the catalog database does not open with its storage key")


def rekey_catalog(m_dir: Path, storage_key: Path, output: Path, *, sqlcipher: str = "sqlcipher",
                  new_seed: bytes | None = None) -> RekeyResult:
    """Write ``output/M/<name>`` and ``output/storage-key.json`` for a fresh seed."""
    if shutil.which(sqlcipher) is None:
        raise RekeyError(f"{sqlcipher!r} not found; install SQLCipher 4 (e.g. brew install sqlcipher)")
    old_seed, payload = read_catalog_seed(storage_key)
    source = m_dir / storage_name(old_seed)
    if not source.is_file():
        raise RekeyError(f"catalog database {source.name} for this storage key is not in {m_dir}")
    seed = new_seed if new_seed is not None else os.urandom(SEED_BYTES)
    if len(seed) != SEED_BYTES or seed == old_seed:
        raise RekeyError("the new seed must be 256 fresh bytes")
    target_dir = output / "M"
    target = target_dir / storage_name(seed)
    key_out = output / "storage-key.json"
    if key_out.exists() or target.exists():
        raise RekeyError(f"{output} already holds a re-keyed cache")

    with tempfile.TemporaryDirectory(prefix="rekey-", dir=output.parent if output.parent.exists() else None) as tmp:
        work = Path(tmp) / "catalog.db"
        shutil.copy2(source, work)
        journal = source.with_name(source.name + "-journal")
        if journal.is_file():  # a hot journal is rolled back on open; keep it with the copy
            shutil.copy2(journal, work.with_name(work.name + "-journal"))
        tables = _count_tables(sqlcipher, work, old_seed)
        _sqlcipher(sqlcipher, work,
                   f"PRAGMA key = \"x'{sqlcipher_key(old_seed)}'\";\n"
                   f"PRAGMA rekey = \"x'{sqlcipher_key(seed)}'\";\n")
        if work.with_name(work.name + "-journal").exists():
            raise RekeyError("the re-keyed database still has a journal")
        if _count_tables(sqlcipher, work, seed) != tables:
            raise RekeyError("the re-keyed database lost tables")
        target_dir.mkdir(parents=True, exist_ok=True)
        shutil.move(str(work), target)
        target.chmod(0o644)

    out_payload = {"prefs": payload.get("prefs"), "entries": {CATALOG_PREFS_KEY: encode_seed(seed)}}
    key_out.write_text(json.dumps(out_payload, indent=2) + "\n", encoding="utf-8")
    key_out.chmod(0o600)
    return RekeyResult(database=target, storage_key=key_out, tables=tables)
