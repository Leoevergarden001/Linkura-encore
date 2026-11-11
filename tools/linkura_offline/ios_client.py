from __future__ import annotations

import hashlib
import json
import plistlib
import re
import stat
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

from .ios_macho import IosError, MachO, insert_dylib
from .ios_metadata import TARGETS, Metadata31, map_methods


PROBE_NAME = "LinkuraPreservationProbe.dylib"
PROBE_INSTALL_NAME = f"@executable_path/Frameworks/{PROBE_NAME}"


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def checked_members(archive: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
    members = {}
    total = 0
    for info in archive.infolist():
        name = info.filename
        parts = PurePosixPath(name).parts
        if not parts or name.startswith("/") or "\\" in name or any(p in (".", "..") for p in name.rstrip("/").split("/")):
            raise IosError(f"unsafe IPA member: {name!r}")
        if name in members:
            raise IosError(f"duplicate IPA member: {name}")
        if stat.S_ISLNK(info.external_attr >> 16):
            raise IosError(f"symlink IPA member is unsupported: {name}")
        total += info.file_size
        if info.file_size > 512 * 1024**2 or total > 4 * 1024**3:
            raise IosError("IPA exceeds supported member/total uncompressed size")
        members[name] = info
    return members


def app_info(archive: zipfile.ZipFile) -> tuple[str, dict, dict[str, zipfile.ZipInfo]]:
    members = checked_members(archive)
    plists = [n for n in members if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
    if len(plists) != 1:
        raise IosError("IPA must contain exactly one top-level Payload/*.app")
    info = plistlib.loads(archive.read(plists[0]))
    executable = info.get("CFBundleExecutable")
    if not isinstance(executable, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", executable) or executable in (".", ".."):
        raise IosError("unsupported CFBundleExecutable")
    app = plists[0].rsplit("/", 1)[0] + "/"
    if app + executable not in members:
        raise IosError("IPA main executable is missing")
    return app, info, members


def inspect_ios_client(source: Path) -> dict:
    source = source.expanduser().resolve()
    with zipfile.ZipFile(source) as archive:
        app, info, members = app_info(archive)
        main_name = app + info["CFBundleExecutable"]
        unity_name = app + "Frameworks/UnityFramework.framework/UnityFramework"
        metadata_name = app + "Data/Managed/Metadata/global-metadata.dat"
        if unity_name not in members or metadata_name not in members:
            raise IosError("IPA does not contain the expected UnityFramework/IL2CPP metadata")
        main_data, unity_data, metadata_data = (archive.read(n) for n in (main_name, unity_name, metadata_name))
        main, unity = MachO(main_data), MachO(unity_data)
        binaries = []
        for name, data, image in ((main_name, main_data, main), (unity_name, unity_data, unity)):
            binaries.append(dict(member=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), **image.summary()))
        unencrypted = not any(e["cryptid"] for image in (main, unity) for e in image.encryption)
        methods = []
        mapping_status = "encrypted-code-not-mapped"
        if unencrypted:
            metadata = Metadata31(metadata_data)
            methods = map_methods(metadata, unity)
            mapping_status = "static-only-not-runtime-validated"
        found = {(m["image"], m["namespace"], m["type"], m["method"]) for m in methods}
        return dict(
            schemaVersion=1, generatedAt=datetime.now(timezone.utc).isoformat(),
            source=dict(path=str(source), bytes=source.stat().st_size, sha256=sha256_file(source)),
            app={k: info.get(k) for k in ("CFBundleIdentifier", "CFBundleShortVersionString", "CFBundleVersion", "MinimumOSVersion", "UIDeviceFamily")},
            binaries=binaries, metadata=dict(member=metadata_name, bytes=len(metadata_data), sha256=hashlib.sha256(metadata_data).hexdigest()),
            inventory=dict(entries=len(members), uncompressedBytes=sum(i.file_size for i in members.values()),
                           builtinDataBytes=sum(i.file_size for n, i in members.items() if n.startswith(app + "Data/"))),
            mappingStatus=mapping_status, methods=methods,
            missingTargets=[dict(zip(("image", "namespace", "type", "method"), t)) for t in TARGETS if t not in found],
            gates=dict(encryptionMarkerClear=unencrypted, installedOnDevice=False, probeLoadedOnDevice=False,
                       compatibilityPatched=False, loginVerified=False, iosResourcesPreserved=False, oneSongVerified=False),
        )


def write_ios_inspection(source: Path, output: Path) -> dict:
    output = output.expanduser().resolve()
    if output.exists() or output == source.expanduser().resolve():
        raise IosError(f"refusing to overwrite inspection output: {output}")
    report = inspect_ios_client(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return report


def stage_ios_client(source: Path, output: Path, *, bundle_id: str, probe: Path | None = None) -> dict:
    """Produce an explicitly UNSIGNED baseline/probe IPA; never install or sign.

    No gameplay code is patched here. An optional probe is loaded via a signed-at-
    installation LC_LOAD_DYLIB command, without changing existing code offsets.
    """
    source, output = source.expanduser().resolve(), output.expanduser().resolve()
    manifest = output.with_suffix(output.suffix + ".json")
    if output.exists() or manifest.exists() or output == source or (probe and output == probe.resolve()):
        raise IosError("refusing to overwrite source, probe, output, or manifest")
    if not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", bundle_id):
        raise IosError("invalid experimental bundle identifier")
    with zipfile.ZipFile(source) as archive:
        app, info, members = app_info(archive)
        if bundle_id == info.get("CFBundleIdentifier"):
            raise IosError("experimental bundle ID must differ from the official app")
        if any(n.endswith(".appex/Info.plist") or ".app/Watch/" in n for n in members):
            raise IosError("nested app extensions/watch apps require separate provisioning; unsupported")
        binary_names = [app + info["CFBundleExecutable"]]
        binary_names += [n for n in members if (".framework/" in n and n.rsplit("/", 1)[-1] == n.split(".framework/")[0].rsplit("/", 1)[-1]) or n.endswith(".dylib")]
        for name in binary_names:
            binary = MachO(archive.read(name))
            if any(e["cryptid"] for e in binary.encryption):
                raise IosError(f"cannot stage encrypted executable: {name}")
            if 2 not in binary.platforms:
                raise IosError(f"expected iPhoneOS device binary: {name}")
        replacements = {}
        info["CFBundleIdentifier"] = bundle_id
        info["CFBundleDisplayName"] = "Linkura iOS Lab"
        replacements[app + "Info.plist"] = plistlib.dumps(info)
        if probe:
            probe = probe.expanduser().resolve()
            probe_data = probe.read_bytes()
            image = MachO(probe_data)
            if image.filetype != 6 or 2 not in image.platforms or any(e["cryptid"] for e in image.encryption):
                raise IosError("probe must be an unencrypted arm64 iPhoneOS dylib")
            probe_member = app + "Frameworks/" + PROBE_NAME
            if probe_member in members:
                raise IosError("probe is already embedded; start with the preserved original IPA")
            replacements[probe_member] = probe_data
            main = app + info["CFBundleExecutable"]
            replacements[main] = insert_dylib(archive.read(main), PROBE_INSTALL_NAME)
        source_hash = sha256_file(source)
        output.parent.mkdir(parents=True, exist_ok=True)
        removed = []
        with tempfile.TemporaryDirectory(prefix="ios-stage-", dir=output.parent) as temporary:
            staged = Path(temporary) / "unsigned.ipa"
            with zipfile.ZipFile(staged, "w", zipfile.ZIP_DEFLATED, compresslevel=3) as target:
                for name, member in members.items():
                    if "/_CodeSignature/" in name or name.endswith("/embedded.mobileprovision"):
                        removed.append(name)
                        continue
                    data = replacements.pop(name) if name in replacements else archive.read(member)
                    target.writestr(member, data)
                for name, data in replacements.items():
                    member = zipfile.ZipInfo(name)
                    member.compress_type = zipfile.ZIP_DEFLATED
                    member.external_attr = (stat.S_IFREG | 0o755) << 16
                    target.writestr(member, data)
            with zipfile.ZipFile(staged) as target:
                if target.testzip() is not None:
                    raise IosError("staged IPA ZIP verification failed")
                if probe and PROBE_INSTALL_NAME not in MachO(target.read(app + info["CFBundleExecutable"])).dependencies:
                    raise IosError("staged IPA probe dependency verification failed")
            if sha256_file(source) != source_hash:
                raise IosError("source IPA changed during staging")
            # Exclusive creation prevents accidental concurrent replacement.
            with staged.open("rb") as inp, output.open("xb") as out:
                import shutil
                shutil.copyfileobj(inp, out)
    report = dict(schemaVersion=1, artifactType="unsigned-ios-probe" if probe else "unsigned-ios-baseline",
                  source=dict(path=str(source), sha256=source_hash),
                  artifact=dict(path=str(output), sha256=sha256_file(output), bundleIdentifier=bundle_id),
                  probe=dict(path=str(probe), sha256=sha256_file(probe)) if probe else None,
                  removedSignatureFiles=removed, signingRequired=True, compatibilityPatched=False,
                  runtimeValidated=False,
                  notes=["Sign all nested code and the app with matching provisioning/entitlements before installing.",
                         "Experimental bundle ID changes may affect keychain/account APIs; these are not validated."])
    with manifest.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return report
