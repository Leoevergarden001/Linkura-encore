"""Build and install the Android resource pack for the offline 5.1.0 client.

A pack is a directory whose ``files/`` subtree mirrors the game's external
files dir (``/sdcard/Android/data/com.oddno.lovelive/files``):

    files/D/<xx>/<name>                  asset cache (35,952 + 4,551 files)
    files/M/<name>                       catalog / master / per-player DBs
    files/linkura-local/storage-key.json local-storage key (see android_storage_key)
    MANIFEST.json                        sources, counts and sizes

Sources are the archived snapshot tar (``game-external-files.tar``), the
post-snapshot delta tar (``game-external-delta.tar``, wins on conflicts) and
the snapshot's ``game-private-files.tar`` for the PlayerPrefs storage key.
Only ``./D`` and ``./M`` are taken from the external tars; nothing from the
private tar except the storage-key entry. The pack is private: it derives
from a real install, so never commit or publish it.

Install needs no root. Android 11+ forbids ``adb push`` from creating
directories under ``Android/data``, so the Localify module creates the D/
layout on the game's first launch and ``install`` launches the game once
before pushing files into the existing directories.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import tarfile
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Sequence

from .android_storage_key import export_storage_key
from .cache_rekey import RekeyError, rekey_cache

PACKAGE = "com.oddno.lovelive"
ACTIVITY = f"{PACKAGE}/com.unity3d.player.UnityPlayerActivity"
DEVICE_FILES = f"/sdcard/Android/data/{PACKAGE}/files"
PREFS_MEMBER = "shared_prefs/com.oddno.lovelive.v2.playerprefs.xml"
CACHE_ROOTS = ("D", "M")


class ResourcePackError(RuntimeError):
    pass


@dataclass(frozen=True)
class PackSummary:
    root: Path
    d_files: int
    m_files: int
    total_bytes: int


def _cache_member(name: str) -> PurePosixPath | None:
    """Map a tar member name to D/... or M/..., or None when not wanted."""
    path = PurePosixPath(name)
    parts = [p for p in path.parts if p not in ("", ".")]
    if not parts or parts[0] not in CACHE_ROOTS:
        return None
    if any(p == ".." or p.startswith("._") for p in parts):
        return None
    return PurePosixPath(*parts)


def _extract_cache(tar_path: Path, files_root: Path) -> int:
    count = 0
    with tarfile.open(tar_path) as archive:
        for member in archive:
            target = _cache_member(member.name)
            if target is None or not member.isfile():
                continue
            out = files_root.joinpath(*target.parts)
            out.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(member)
            if source is None:
                continue
            with source, out.open("wb") as sink:
                shutil.copyfileobj(source, sink, 1024 * 1024)
            count += 1
    return count


def _extract_prefs(private_tar: Path, dest: Path) -> Path:
    with tarfile.open(private_tar) as archive:
        for member in archive:
            name = str(PurePosixPath(member.name)).lstrip("./")
            if name == PREFS_MEMBER and member.isfile():
                source = archive.extractfile(member)
                if source is None:
                    break
                out = dest / "playerprefs.xml"
                with source, out.open("wb") as sink:
                    shutil.copyfileobj(source, sink)
                out.chmod(0o600)
                return out
    raise ResourcePackError(f"{private_tar} has no {PREFS_MEMBER}")


def summarize(root: Path) -> PackSummary:
    files_root = root / "files"
    d_files = [p for p in (files_root / "D").rglob("*") if p.is_file()]
    m_files = [p for p in (files_root / "M").glob("*") if p.is_file()]
    total = sum(p.stat().st_size for p in d_files + m_files)
    return PackSummary(root=root, d_files=len(d_files), m_files=len(m_files), total_bytes=total)


def build_resource_pack(
    snapshot_tar: Path,
    private_tar: Path,
    output: Path,
    delta_tars: Sequence[Path] = (),
    force: bool = False,
    fresh_key: bool = True,
    sqlcipher: str = "sqlcipher",
) -> PackSummary:
    for source in (snapshot_tar, private_tar, *delta_tars):
        if not source.is_file():
            raise ResourcePackError(f"missing input {source}")
    if output.exists():
        if not force:
            raise ResourcePackError(f"{output} exists; pass force to replace it")
        shutil.rmtree(output)
    files_root = output / "files"
    files_root.mkdir(parents=True)
    extracted = {str(snapshot_tar): _extract_cache(snapshot_tar, files_root)}
    for delta in delta_tars:
        extracted[str(delta)] = _extract_cache(delta, files_root)
    with tempfile.TemporaryDirectory() as tmp:
        prefs = _extract_prefs(private_tar, Path(tmp))
        key_path = files_root / "linkura-local" / "storage-key.json"
        export_storage_key(prefs, key_path)
        if fresh_key:
            # Keep the catalog and master databases, under keys no install ever had.
            try:
                result = rekey_cache(files_root / "M", key_path, Path(tmp) / "rekeyed", sqlcipher=sqlcipher)
            except RekeyError as error:
                raise ResourcePackError(str(error)) from error
            shutil.rmtree(files_root / "M")
            (files_root / "M").mkdir()
            for database in result.databases.values():
                shutil.move(str(database), files_root / "M" / database.name)
            key_path.unlink()
            shutil.move(str(result.storage_key), key_path)
    summary = summarize(output)
    manifest = {
        "format": "linkura-android-resource-pack/1",
        "package": PACKAGE,
        "device_files_dir": DEVICE_FILES,
        "sources": extracted,
        "d_files": summary.d_files,
        "m_files": summary.m_files,
        "total_bytes": summary.total_bytes,
        "fresh_storage_key": fresh_key,
        "private": True,
    }
    (output / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return summary


Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def _run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(command), check=False, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def install_resource_pack(
    pack: Path,
    serial: str,
    adb: str = "adb",
    runner: Runner = _run,
    first_launch_wait: float = 25.0,
    log: Callable[[str], None] = print,
) -> PackSummary:
    summary = summarize(pack)
    if summary.d_files == 0 or summary.m_files == 0:
        raise ResourcePackError(f"{pack} does not look like a resource pack")

    def adb_cmd(*args: str) -> str:
        result = runner([adb, "-s", serial, *args])
        if result.returncode != 0:
            raise ResourcePackError(f"adb {' '.join(args)} failed:\n{result.stdout.strip()}")
        return result.stdout

    if f"package:{PACKAGE}" not in adb_cmd("shell", "pm", "list", "packages", PACKAGE):
        raise ResourcePackError(f"{PACKAGE} is not installed on {serial}; install the APK first")
    probe = adb_cmd("shell", f"ls -d {DEVICE_FILES}/D/77 2>/dev/null || echo missing").strip()
    if probe.endswith("missing"):
        log("launching the game once so it creates the cache directories")
        adb_cmd("shell", "am", "start", "-n", ACTIVITY)
        time.sleep(first_launch_wait)
        adb_cmd("shell", "am", "force-stop", PACKAGE)
        probe = adb_cmd("shell", f"ls -d {DEVICE_FILES}/D/77 2>/dev/null || echo missing").strip()
        if probe.endswith("missing"):
            raise ResourcePackError("cache directories were not created; is the Localify build installed?")
    adb_cmd("shell", "am", "force-stop", PACKAGE)
    files_root = pack / "files"
    for name in ("M", "linkura-local", "D"):
        log(f"pushing {name}/ ...")
        adb_cmd("push", str(files_root / name) + "/.", f"{DEVICE_FILES}/{name}/")
    counted = adb_cmd("shell", f"find {DEVICE_FILES}/D -type f | wc -l").strip()
    try:
        on_device = int(counted.split()[-1])
    except (ValueError, IndexError) as error:
        raise ResourcePackError(f"cannot count files on device: {counted!r}") from error
    if on_device < summary.d_files:
        raise ResourcePackError(f"device has {on_device} D files, pack has {summary.d_files}")
    log(f"device D files: {on_device} (pack {summary.d_files})")
    return summary
