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
from .cache_rekey import RekeyError, rekey_cache
from .ios_macho import IosError
from .ios_offline import DEFAULT_PORT, DEFAULT_RES_VERSION, OFFLINE_PLAYER_ID, stage_offline_ios_client
from .ios_symbols import write_symbols

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BUNDLE_ID = "org.linkura.preservation.probe"
DEFAULT_MODULE = REPO / "android/app/build/outputs/apk/debug/app-debug.apk"
DEFAULT_LSPATCH = REPO / "android/lspatch/lspatch_loaderfix.jar"
DEFAULT_DYLIB = REPO / "ios/build/LinkuraOffline.dylib"


def _ios_stage(args: argparse.Namespace) -> int:
    report = stage_offline_ios_client(
        args.source, args.dylib, args.output, bundle_id=args.bundle_id, display_name=args.display_name,
        port=args.port,
        redirect_assets=not args.keep_asset_urls,
        asset_base=args.asset_base_url,
        show_preview_bgm_fallback=args.show_preview_bgm_fallback,
        disable_lyric_videos=args.disable_lyric_videos,
        guard_destructive=not args.allow_destructive_actions,
        block_story=not args.allow_story_playback,
        block_archive=not args.allow_archive_playback,
        block_chapter=not args.allow_chapter_portal,
        skip_catalog=not args.allow_catalog_sync,
        res_version=None if args.per_player_res_version else args.res_version,
        player_id=None if args.keep_player_id else OFFLINE_PLAYER_ID,
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


def _master_open_gacha(args: argparse.Namespace) -> int:
    from .master_patch import open_gacha_series
    result = open_gacha_series(args.m_dir, args.storage_key, args.output, sqlcipher=args.sqlcipher)
    print(f"OK  {result.series} Gacha series opened ({result.exchange_offers} exchange offers, "
          f"{result.select_tickets} select-ticket series): {result.database}")
    print("    replace the device's M/ master database with it (same name, same storage key)")
    return 0


def _cache_rekey(args: argparse.Namespace) -> int:
    result = rekey_cache(args.m_dir, args.storage_key, args.output, sqlcipher=args.sqlcipher)
    for domain, database in result.databases.items():
        print(f"OK  {domain} database: {database}")
    for domain in result.skipped:
        print(f"WARN no {domain} database/entry: the client will not rebuild it from the cache")
    print(f"OK  fresh storage key: {result.storage_key} (replace the cache's M/ and storage-key.json with these)")
    return 0


def _pack_build(args: argparse.Namespace) -> int:
    summary = build_resource_pack(args.snapshot, args.private, args.output, args.delta, force=args.force,
                                  fresh_key=not args.keep_source_key)
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
    p.add_argument("--display-name", default="Linkura Offline", help="home screen name")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--keep-asset-urls", action="store_true", help="do not point asset URLs at the local server")
    p.add_argument("--asset-base-url", help="download assets from this base URL (serving <base>/ios/..., "
                   "<base>/raw/...) instead of the in-app server's /a/; at most 37 characters")
    p.add_argument("--show-preview-bgm-fallback", action="store_true",
                   help="use archived Daily BGM in the SHOW settings preview")
    p.add_argument("--disable-lyric-videos", action="store_true",
                   help="treat the SHOW music video option as off (lyric videos were not saved)")
    p.add_argument("--allow-destructive-actions", action="store_true",
                   help="keep cache delete / logout / user delete working (default: buttons do nothing)")
    p.add_argument("--allow-story-playback", action="store_true",
                   help="let 活動記録 再生 start its download (default: the dialog just closes)")
    p.add_argument("--allow-archive-playback", action="store_true",
                   help="let a スクールアイドルコネクト archive start its replay download (default: a tap does nothing)")
    p.add_argument("--allow-chapter-portal", action="store_true",
                   help="let Chapter RANK open the Chapter portal (its assets were not saved)")
    p.add_argument("--res-version", default=DEFAULT_RES_VERSION,
                   help="resource version of your cache; the client always reports it, so the storage key "
                        "alone (without the source player id) is enough")
    p.add_argument("--per-player-res-version", action="store_true",
                   help="keep reading resource versions from per-player storage (needs the source player id)")
    p.add_argument("--allow-catalog-sync", action="store_true",
                   help="let the client download the resource manifest when its saved version is missing "
                        "(needs the source player id in PlayerPrefs; default: keep the cached catalog)")
    p.add_argument("--keep-player-id", action="store_true",
                   help="read the player id from PlayerPrefs (needs the source player id; default: the "
                        "client is player 000000000, like the Android build)")
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

    p = sub.add_parser("cache-rekey", help="copy a cache's catalog and master databases under fresh storage keys")
    p.add_argument("--m-dir", type=Path, required=True, help="the cache's M/ directory")
    p.add_argument("--storage-key", type=Path, required=True, help="storage-key.json that pairs with it")
    p.add_argument("--output", type=Path, required=True, help="new directory: M/<catalog> + storage-key.json")
    p.add_argument("--sqlcipher", default="sqlcipher", help="SQLCipher 4 command line tool")
    p.set_defaults(func=_cache_rekey)

    p = sub.add_parser("master-open-gacha", help="copy the master database with ended Gacha series opened again")
    p.add_argument("--m-dir", type=Path, required=True, help="the cache's M/ directory")
    p.add_argument("--storage-key", type=Path, required=True, help="storage-key.json that pairs with it")
    p.add_argument("--output", type=Path, required=True, help="directory for M/<master database>")
    p.add_argument("--sqlcipher", default="sqlcipher", help="SQLCipher 4 command line tool")
    p.set_defaults(func=_master_open_gacha)

    p = sub.add_parser("android-resource-pack-build", help="assemble a private resource pack (D/, M/, storage key)")
    p.add_argument("--snapshot", type=Path, required=True, help="tar of the game's external files dir")
    p.add_argument("--delta", type=Path, action="append", default=[], help="later tar(s) of the same dir; later wins")
    p.add_argument("--private", type=Path, required=True, help="tar of the game's private files (storage key only)")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--force", action="store_true")
    p.add_argument("--keep-source-key", action="store_true",
                   help="keep all M/ databases and the source install's storage key (default: only the catalog "
                        "and master databases, re-keyed to fresh keys; needs the sqlcipher tool)")
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
    except (IosError, AndroidRepackError, ResourcePackError, StorageKeyError, RekeyError,
            OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"ERR {args.command}: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
