"""Command line for packaging and installing linkura-offline.

    python3 -m tools.linkura_offline <command> --help

Every command works on files the user provides (their own client and asset
cache); nothing here downloads game data.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from .android_repack import AndroidRepackError, repack_android_client
from .android_resource_pack import ResourcePackError, build_resource_pack, install_resource_pack
from .android_storage_key import StorageKeyError, export_storage_key
from .ios_macho import IosError
from .ios_offline import DEFAULT_PORT, stage_offline_ios_client
from .ios_symbols import write_symbols

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BUNDLE_ID = "org.linkura.preservation.probe"
DEFAULT_MODULE = REPO / "android/app/build/outputs/apk/debug/app-debug.apk"
DEFAULT_LSPATCH = REPO / "android/lspatch/lspatch_loaderfix.jar"
DEFAULT_DYLIB = REPO / "ios/build/LinkuraOffline.dylib"


def _ios_stage(args: argparse.Namespace) -> int:
    report = stage_offline_ios_client(
        args.source, args.dylib, args.output, bundle_id=args.bundle_id, port=args.port,
        redirect_assets=not args.keep_asset_urls,
        show_preview_bgm_fallback=args.show_preview_bgm_fallback,
        disable_lyric_videos=args.disable_lyric_videos,
        guard_destructive=not args.allow_destructive_actions,
        block_story=not args.allow_story_playback,
        block_chapter=not args.allow_chapter_portal,
    )
    for patch in report["literalPatches"]:
        print(f"OK  metadata {patch['old']} -> {patch['new']}")
    for patch in report["codePatches"]:
        print(f"OK  code {patch['address']} {patch['name']}")
    print(f"OK  UNSIGNED offline iOS IPA: {report['artifact']['path']} "
          f"({report['artifact']['bundleIdentifier']}); sign before installing")
    return 0


def _ios_symbols(args: argparse.Namespace) -> int:
    """Address -> method name map from the user's own IPA, for disassembly helpers."""
    with zipfile.ZipFile(args.ipa) as archive, tempfile.TemporaryDirectory() as tmp:
        names = archive.namelist()
        try:
            metadata = next(n for n in names if n.endswith("Data/Managed/Metadata/global-metadata.dat"))
            framework = next(n for n in names if n.endswith("Frameworks/UnityFramework.framework/UnityFramework"))
        except StopIteration:
            raise IosError("IPA has no global-metadata.dat / UnityFramework") from None
        root = Path(tmp)
        archive.extract(metadata, root)
        archive.extract(framework, root)
        count = write_symbols(root / metadata, root / framework, args.output)
    print(f"OK  {count} symbols -> {args.output}")
    return 0


def _android_repack(args: argparse.Namespace) -> int:
    result = repack_android_client(
        source_apk=args.source, module_apk=args.module, lspatch_jar=args.lspatch_jar,
        output_apk=args.output, java=args.java, debuggable=not args.no_debuggable, force=args.force,
    )
    source_kind = "embedded clean origin" if result.source_contained_origin else "source APK"
    print(f"OK  Android repack: {result.output_apk}")
    print(f"OK  origin: {source_kind} sha256={result.origin_sha256}")
    print(f"OK  module: {result.module_sha256} ({result.embedded_module_path})")
    print(f"OK  artifact: sha256={result.output_sha256}")
    print(f"OK  build manifest: {result.manifest}")
    return 0


def _android_storage_key(args: argparse.Namespace) -> int:
    result = export_storage_key(args.prefs, args.output, force=args.force)
    print(f"OK  storage key: {result.output} ({len(result.names)} entry, private: keep out of git)")
    return 0


def _pack_build(args: argparse.Namespace) -> int:
    summary = build_resource_pack(args.snapshot, args.private, args.output, args.delta, force=args.force)
    print(f"OK  resource pack: {summary.root} D={summary.d_files} M={summary.m_files} "
          f"bytes={summary.total_bytes} (private: keep out of git)")
    return 0


def _pack_install(args: argparse.Namespace) -> int:
    summary = install_resource_pack(args.pack, args.serial, adb=args.adb)
    print(f"OK  resource pack installed on {args.serial}: D={summary.d_files} M={summary.m_files}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python3 -m tools.linkura_offline", description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ios-offline-stage", help="decrypted 5.0.1 IPA + LinkuraOffline.dylib -> UNSIGNED offline IPA")
    p.add_argument("--source", type=Path, required=True, help="decrypted 5.0.1 IPA (your own)")
    p.add_argument("--dylib", type=Path, default=DEFAULT_DYLIB, help="ios/build.sh output")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--keep-asset-urls", action="store_true", help="do not point asset URLs at the local server")
    p.add_argument("--show-preview-bgm-fallback", action="store_true",
                   help="use archived Daily BGM in the SHOW settings preview")
    p.add_argument("--disable-lyric-videos", action="store_true",
                   help="treat the SHOW music video option as off (lyric videos were not saved)")
    p.add_argument("--allow-destructive-actions", action="store_true",
                   help="keep cache delete / logout / user delete working (default: buttons do nothing)")
    p.add_argument("--allow-story-playback", action="store_true",
                   help="let 活動記録 再生 start its download (default: the dialog just closes)")
    p.add_argument("--allow-chapter-portal", action="store_true",
                   help="let Chapter RANK open the Chapter portal (its assets were not saved)")
    p.set_defaults(func=_ios_stage)

    p = sub.add_parser("ios-symbols", help="address -> method name map from an IPA (for tools/re)")
    p.add_argument("--ipa", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True, help="JSON list of [address, name]")
    p.set_defaults(func=_ios_symbols)

    p = sub.add_parser("android-repack", help="rebuild a client APK with the offline module using LSPatch")
    p.add_argument("--source", type=Path, required=True, help="your 5.1.0 client APK (or an LSPatch-patched one)")
    p.add_argument("--module", type=Path, default=DEFAULT_MODULE)
    p.add_argument("--lspatch-jar", type=Path, default=DEFAULT_LSPATCH)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--java", default="java", help="Java 21+ runtime for LSPatch")
    p.add_argument("--no-debuggable", action="store_true")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=_android_repack)

    p = sub.add_parser("android-storage-key", help="export the PlayerPrefs storage key that pairs with an asset cache")
    p.add_argument("--prefs", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=_android_storage_key)

    p = sub.add_parser("android-resource-pack-build", help="assemble a private resource pack (D/, M/, storage key)")
    p.add_argument("--snapshot", type=Path, required=True, help="tar of the game's external files dir")
    p.add_argument("--delta", type=Path, action="append", default=[], help="later tar(s) of the same dir; later wins")
    p.add_argument("--private", type=Path, required=True, help="tar of the game's private files (storage key only)")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=_pack_build)

    p = sub.add_parser("android-resource-pack-install", help="push a resource pack to a device with plain adb")
    p.add_argument("--pack", type=Path, required=True)
    p.add_argument("--serial", required=True)
    p.add_argument("--adb", default="adb")
    p.set_defaults(func=_pack_install)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (IosError, AndroidRepackError, ResourcePackError, StorageKeyError,
            OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"ERR {args.command}: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
