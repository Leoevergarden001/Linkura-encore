from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence


class AndroidRepackError(RuntimeError):
    """Raised when an Android client cannot be repacked or verified safely."""


@dataclass(frozen=True)
class AndroidRepackResult:
    source_apk: Path
    origin_apk: Path
    module_apk: Path
    output_apk: Path
    manifest: Path
    source_contained_origin: bool
    source_sha256: str
    origin_sha256: str
    module_sha256: str
    output_sha256: str
    lspatch_sha256: str
    embedded_module_path: str


Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def _default_runner(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _require_file(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise AndroidRepackError(f"{label} not found: {resolved}")
    return resolved


def _validate_apk(path: Path, label: str) -> None:
    try:
        with zipfile.ZipFile(path) as archive:
            if "AndroidManifest.xml" not in archive.namelist():
                raise AndroidRepackError(
                    f"{label} is missing AndroidManifest.xml: {path}"
                )
    except zipfile.BadZipFile as error:
        raise AndroidRepackError(f"{label} is not a valid APK ZIP: {path}") from error


def _prepare_origin(source: Path, destination: Path) -> bool:
    try:
        with zipfile.ZipFile(source) as archive:
            member = "assets/lspatch/origin.apk"
            if member not in archive.namelist():
                shutil.copy2(source, destination)
                return False
            _extract_member_by_local_header(source, archive.getinfo(member), destination)
            return True
    except (OSError, zipfile.BadZipFile) as error:
        raise AndroidRepackError(f"cannot read source APK: {source}: {error}") from error


def _extract_member_by_local_header(
    archive_path: Path, info: zipfile.ZipInfo, destination: Path
) -> None:
    """Extract one bounded entry without disabling ZipFile overlap protection.

    Some older LSPatch APKs have a central-directory layout that Python classifies
    as overlapping. We only need the exact, stored clean-origin member. Reading its
    declared compressed byte range and validating output length plus CRC retains the
    important safety checks without trusting or extracting any neighbouring entry.
    """

    if info.flag_bits & 0x1:
        raise AndroidRepackError("embedded origin APK is encrypted")
    if info.compress_type not in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}:
        raise AndroidRepackError(
            f"unsupported embedded origin compression: {info.compress_type}"
        )

    remaining = info.compress_size
    written = 0
    checksum = 0
    decompressor = (
        zlib.decompressobj(-zlib.MAX_WBITS)
        if info.compress_type == zipfile.ZIP_DEFLATED
        else None
    )
    with archive_path.open("rb") as source, destination.open("wb") as target:
        source.seek(info.header_offset)
        header = source.read(30)
        if len(header) != 30 or header[:4] != b"PK\x03\x04":
            raise AndroidRepackError("invalid local header for embedded origin APK")
        filename_length, extra_length = struct.unpack_from("<HH", header, 26)
        local_name = source.read(filename_length)
        source.seek(extra_length, os.SEEK_CUR)
        expected_name = info.filename.encode("utf-8")
        if local_name != expected_name:
            raise AndroidRepackError("embedded origin local-header name mismatch")

        while remaining:
            block = source.read(min(1024 * 1024, remaining))
            if not block:
                raise AndroidRepackError("embedded origin APK data is truncated")
            remaining -= len(block)
            output = decompressor.decompress(block) if decompressor else block
            if output:
                target.write(output)
                written += len(output)
                checksum = zlib.crc32(output, checksum)
        if decompressor:
            output = decompressor.flush()
            target.write(output)
            written += len(output)
            checksum = zlib.crc32(output, checksum)
            if not decompressor.eof:
                raise AndroidRepackError("embedded origin deflate stream is incomplete")

    if written != info.file_size:
        raise AndroidRepackError(
            f"embedded origin size mismatch: expected {info.file_size}, got {written}"
        )
    if checksum & 0xFFFFFFFF != info.CRC:
        raise AndroidRepackError("embedded origin CRC mismatch")


def _verify_patched_apk(path: Path, module_sha256: str) -> tuple[str, dict[str, object]]:
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            required = {"AndroidManifest.xml", "assets/lspatch/config.json", "assets/lspatch/origin.apk"}
            missing = sorted(required.difference(names))
            if missing:
                raise AndroidRepackError(
                    f"patched APK is missing required LSPatch entries: {missing}"
                )
            try:
                config = json.loads(archive.read("assets/lspatch/config.json"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise AndroidRepackError("patched APK has invalid LSPatch config") from error

            module_paths = sorted(
                name
                for name in names
                if name.startswith("assets/lspatch/modules/") and name.endswith(".apk")
            )
            matches: list[str] = []
            for name in module_paths:
                if hashlib.sha256(archive.read(name)).hexdigest() == module_sha256:
                    matches.append(name)
            if len(matches) != 1:
                raise AndroidRepackError(
                    "patched APK does not contain exactly one byte-identical input module "
                    f"(found {len(matches)})"
                )
            return matches[0], config
    except zipfile.BadZipFile as error:
        raise AndroidRepackError(f"LSPatch output is not a valid APK ZIP: {path}") from error


def repack_android_client(
    *,
    source_apk: Path,
    module_apk: Path,
    lspatch_jar: Path,
    output_apk: Path,
    java: str = "java",
    debuggable: bool = True,
    force: bool = False,
    runner: Runner = _default_runner,
) -> AndroidRepackResult:
    source = _require_file(source_apk, "source APK")
    module = _require_file(module_apk, "module APK")
    patcher = _require_file(lspatch_jar, "LSPatch JAR")
    output = output_apk.expanduser().resolve()
    manifest = output.with_suffix(output.suffix + ".json")

    if source == output:
        raise AndroidRepackError("output APK must not overwrite the source APK")
    if (output.exists() or manifest.exists()) and not force:
        raise AndroidRepackError(
            f"output or manifest already exists; pass --force to replace: {output}"
        )

    _validate_apk(source, "source APK")
    _validate_apk(module, "module APK")
    source_sha256 = _sha256(source)
    module_sha256 = _sha256(module)
    patcher_sha256 = _sha256(patcher)

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="linkura-repack-") as temporary:
        staging = Path(temporary)
        origin = staging / "origin.apk"
        generated_dir = staging / "output"
        generated_dir.mkdir()
        contained_origin = _prepare_origin(source, origin)
        _validate_apk(origin, "clean origin APK")
        origin_sha256 = _sha256(origin)

        command = [
            java,
            "-jar",
            str(patcher),
            "-f",
        ]
        if debuggable:
            command.append("-d")
        command.extend(["-m", str(module), "-o", str(generated_dir), str(origin)])
        process = runner(command)
        if process.returncode != 0:
            raise AndroidRepackError(
                f"LSPatch failed ({process.returncode}): {process.stdout.strip()}"
            )

        generated = sorted(generated_dir.glob("*.apk"))
        if len(generated) != 1:
            raise AndroidRepackError(
                f"LSPatch must produce exactly one APK, found {len(generated)}"
            )
        embedded_module_path, lspatch_config = _verify_patched_apk(
            generated[0], module_sha256
        )

        temporary_output = output.parent / f".{output.name}.tmp"
        try:
            shutil.copy2(generated[0], temporary_output)
            os.replace(temporary_output, output)
        finally:
            temporary_output.unlink(missing_ok=True)

        output_sha256 = _sha256(output)
        record = {
            "schemaVersion": 1,
            "source": {
                "path": str(source),
                "sha256": source_sha256,
                "containedOrigin": contained_origin,
            },
            "origin": {"sha256": origin_sha256},
            "module": {
                "path": str(module),
                "sha256": module_sha256,
                "embeddedPath": embedded_module_path,
            },
            "lspatch": {
                "path": str(patcher),
                "sha256": patcher_sha256,
                "config": lspatch_config,
            },
            "artifact": {"path": str(output), "sha256": output_sha256},
        }
        temporary_manifest = manifest.parent / f".{manifest.name}.tmp"
        try:
            temporary_manifest.write_text(
                json.dumps(record, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            os.replace(temporary_manifest, manifest)
        finally:
            temporary_manifest.unlink(missing_ok=True)

    return AndroidRepackResult(
        source_apk=source,
        origin_apk=Path("assets/lspatch/origin.apk") if contained_origin else source,
        module_apk=module,
        output_apk=output,
        manifest=manifest,
        source_contained_origin=contained_origin,
        source_sha256=source_sha256,
        origin_sha256=origin_sha256,
        module_sha256=module_sha256,
        output_sha256=output_sha256,
        lspatch_sha256=patcher_sha256,
        embedded_module_path=embedded_module_path,
    )
