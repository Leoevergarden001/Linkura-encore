"""Give an asset cache a fresh, unused local-storage key.

The 5.x client keeps one random 256-byte seed per storage domain in
PlayerPrefs (LocalDataStorageProvider, Core.dll):

- PlayerPrefs name: base64(HMAC-SHA1(<static key>, domain)), URL-encoded on disk,
  the same on every install: ``CATALOG_PREFS_KEY`` ("//catalog"),
  ``MASTER_PREFS_KEY`` ("//master"); the "app" domain is left alone.
- value: base64(seed), seed = 256 bytes from RNGCryptoServiceProvider.
- database file: ``M/`` + lowercase unpadded Base32(MD5(seed)).
- SQLCipher 4 raw key: SHA-256(seed) (catalog and master databases alike).

A cache needs the catalog database and the master database: D/ files are
encrypted with per-asset catalog seeds, and the client does not rebuild an
empty master database from the cache. The "app" storage holds the source
install's identity (player id) and settings and is not copied. ``rekey_cache``
copies both databases under newly generated seeds, so the output pairs with
storage keys that never belonged to any install. Needs the ``sqlcipher``
command line tool (SQLCipher 4).
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
MASTER_PREFS_KEY = "Dr%2BcmYmVx%2B1kQ8uTmoYADAduWVk%3D"
# (domain, PlayerPrefs name, required)
DOMAINS = (("catalog", CATALOG_PREFS_KEY, True), ("master", MASTER_PREFS_KEY, False))
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


def read_storage_key(storage_key: Path) -> dict:
    try:
        payload = json.loads(storage_key.read_text(encoding="utf-8"))
        if not isinstance(payload["entries"], dict):
            raise TypeError
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RekeyError(f"{storage_key} is not a storage-key.json") from error
    return payload


@dataclass(frozen=True)
class RekeyResult:
    databases: dict[str, Path]     # domain -> re-keyed database
    storage_key: Path
    skipped: tuple[str, ...]       # optional domains without entry or database


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
    raise RekeyError(f"{database.name} does not open with its storage key")


def _rekey_one(binary: str, source: Path, old_seed: bytes, new_seed: bytes, target: Path, tmp: Path) -> None:
    work = tmp / source.name
    shutil.copy2(source, work)
    journal = source.with_name(source.name + "-journal")
    if journal.is_file():  # a hot journal is rolled back on open; keep it with the copy
        shutil.copy2(journal, work.with_name(work.name + "-journal"))
    tables = _count_tables(binary, work, old_seed)
    _sqlcipher(binary, work,
               f"PRAGMA key = \"x'{sqlcipher_key(old_seed)}'\";\n"
               f"PRAGMA rekey = \"x'{sqlcipher_key(new_seed)}'\";\n")
    leftover = work.with_name(work.name + "-journal")
    if leftover.exists():
        leftover.unlink()  # journal_mode=DELETE: a cold journal is only an empty header
    if _count_tables(binary, work, new_seed) != tables:
        raise RekeyError(f"the re-keyed {source.name} lost tables")
    shutil.move(str(work), target)
    target.chmod(0o644)


def rekey_cache(m_dir: Path, storage_key: Path, output: Path, *, sqlcipher: str = "sqlcipher",
                new_seeds: dict[str, bytes] | None = None) -> RekeyResult:
    """Write ``output/M/<names>`` and ``output/storage-key.json`` under fresh seeds."""
    if shutil.which(sqlcipher) is None:
        raise RekeyError(f"{sqlcipher!r} not found; install SQLCipher 4 (e.g. brew install sqlcipher)")
    payload = read_storage_key(storage_key)
    key_out = output / "storage-key.json"
    if key_out.exists() or (output / "M").exists():
        raise RekeyError(f"{output} already holds a re-keyed cache")
    plan, skipped = [], []
    for domain, prefs_key, required in DOMAINS:
        value = payload["entries"].get(prefs_key)
        source = m_dir / storage_name(decode_seed(value)) if value else None
        if source is None or not source.is_file():
            if required:
                raise RekeyError(f"no {domain} database for this storage key in {m_dir}")
            skipped.append(domain)
            continue
        old_seed = decode_seed(value)
        seed = (new_seeds or {}).get(domain) or os.urandom(SEED_BYTES)
        if len(seed) != SEED_BYTES or seed == old_seed:
            raise RekeyError("a new seed must be 256 fresh bytes")
        plan.append((domain, prefs_key, source, old_seed, seed))

    (output / "M").mkdir(parents=True)
    databases, entries = {}, {}
    with tempfile.TemporaryDirectory(prefix="rekey-", dir=output) as tmp:
        for domain, prefs_key, source, old_seed, seed in plan:
            target = output / "M" / storage_name(seed)
            _rekey_one(sqlcipher, source, old_seed, seed, target, Path(tmp))
            databases[domain] = target
            entries[prefs_key] = encode_seed(seed)
    key_out.write_text(json.dumps({"prefs": payload.get("prefs"), "entries": entries}, indent=2) + "\n",
                       encoding="utf-8")
    key_out.chmod(0o600)
    return RekeyResult(databases=databases, storage_key=key_out, skipped=tuple(skipped))
