"""Stage an offline iOS client: decrypted IPA + LinkuraOffline.dylib -> UNSIGNED IPA.

The iOS client cannot be hooked at runtime on a stock device, so the
offline runtime works at the data level:

* IL2CPP metadata string literals: the API base URL (and, optionally, the
  asset base URLs) are rewritten to ``http://127.0.0.1:<port>``. The
  literal is shortened in place: its table entry gets the new length and the
  first bytes of its data are overwritten; no offsets move.
* Info.plist: separate bundle id, App Transport Security exception for
  plain HTTP, file sharing so Documents/LinkuraOffline (log, config,
  overrides) is visible in the Files app and Finder.
* LinkuraOffline.dylib (``ios/build.sh``) is embedded in
  Frameworks/ and loaded through an LC_LOAD_DYLIB command in the main
  executable; it runs the loopback API server.

Nothing is signed or installed here; signing needs the user's own Personal
Team identity and a provisioning profile for the bundle id.
"""

from __future__ import annotations

import json
import plistlib
import re
import shutil
import stat
import struct
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path

from .ios_client import app_info, sha256_file
from .ios_macho import IosError, MachO, insert_dylib

DYLIB_NAME = "LinkuraOffline.dylib"
DYLIB_INSTALL_NAME = f"@executable_path/Frameworks/{DYLIB_NAME}"
METADATA_MEMBER = "Data/Managed/Metadata/global-metadata.dat"
API_BASE = "https://api.link-like-lovelive.app"
ASSET_BASE = "https://assets.link-like-lovelive.app"
DEFAULT_BUNDLE_ID = "org.linkura.preservation.client501"
DEFAULT_PORT = 52680
SHOW_PREVIEW_BGM = "bgm_rhythm_system_preview_0001"
SHOW_PREVIEW_FALLBACK_BGM = "bgm_quest_daily_0001"
FRAMEWORK_MEMBER = "Frameworks/UnityFramework.framework/UnityFramework"

# SHOW lyric videos (music_lyric_video_*.usm) were never downloaded and the
# official CDN is gone, so a song with one asks to download it and the failed
# download drops the player back to the title screen.  These five verified
# 5.0.1 (arm64) sites make the client treat RhythmGameOptionValue.
# EnableMusicVideo as off, exactly as if the player had disabled it:
# (address, what it is, original bytes, replacement bytes).
_MOV_W0_0_RET = "00008052c0035fd6"  # mov w0, #0 ; ret
LYRIC_VIDEO_PATCHES = (
    (0x2989008, "RhythmGameOptionValue::get_EnableMusicVideo", "00a84039c0035fd6", _MOV_W0_0_RET),
    (0x2E665D4, "ConfigResolver::get_EnableMusicVideo", "00a84039c0035fd6", _MOV_W0_0_RET),
    (0x2FBCE80, "RhythmGameMainSceneParam::get_ShouldMusicVideo", "f44fbea9fd7b01a9", _MOV_W0_0_RET),
    # Inline "if (!option.EnableMusicVideo) skip download": cbz w8 -> b (same target).
    (0x223D430, "RhythmGamePrepareSceneView TryDownloadLyricVideoIfNeeded", "a81b0034", "dd000014"),
    (0x29749CC, "RhythmGameGrandPrixPrepareSceneView TryDownloadLyricVideoIfNeeded", "a81b0034", "dd000014"),
)

# 5.0.1 client vs. the later (R2606002) resources.
# DreamLiveStageSelectPanel.Setup looks each member up with
# Transform.Find(charactersId) on the dream_member_<series> prefab root, i.e.
# direct children only.  The 1003 (Oracle Étude) prefab nests its 11 members
# in Scroll View/Viewport/Content so they scroll; Find returns null and the
# tiles stay white and dead.  Redirect that one call to the client's own
# Inspix.TransformExtension.findChildByName(Transform, string), a recursive
# search with the same register layout, so the prefab keeps its scroll view.
COMPAT_PATCHES = (
    (0x2F08398, "DreamLiveStageSelectPanel::Setup Transform.Find -> TransformExtension.findChildByName",
     "067f2895", "3c161094"),
)

# 活動記録 story parts need assets that were never archived; confirming
# 再生 starts a download that fails with a "ログイン画面へ / リトライ" dialog
# and sends the player back to the title.  Make the 再生確認 OK button behave
# like キャンセル (TPopupPlayBackDialog.OnClickOk -> b OnClickCancel; both
# (this) instance methods), so the dialog just closes.
STORY_PLAYBACK_PATCHES = (
    (0x2D3DE94, "TPopupPlayBackDialog::OnClickOk -> OnClickCancel", "ff0301d1", "4d000014"),
)

# Chapter RANK in the Home header opens the Chapter portal when the header
# rank (CommonApiCache.CurrentChapterRank) is non-zero.  The portal loads
# chapter_prefab_100031 and its three images, which were never saved (empty
# placeholders in every archive) and the CDN is gone, so it ends in a
# download error.  <OnPointerClick>d__6.MoveNext already shows the
# 「Chapter対象期間外です」 alert when the rank is 0; make that `cbz` an
# unconditional `b` so the header keeps showing the rank and the tap shows
# the alert.
CHAPTER_PORTAL_PATCHES = (
    (0x2EB0FE8, "PointerSceneChangeMenuChapterPoint.OnPointerClick -> period alert", "00070034", "38000014"),
)

def _adrp(pc: int, target: int, rd: int) -> str:
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    word = 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd
    return word.to_bytes(4, "little").hex()


def _add_imm(rd: int, rn: int, imm12: int) -> str:
    word = 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd
    return word.to_bytes(4, "little").hex()


# Core.GetStoredResourceVersion (also behind GetCachedResourceVersion and
# GetRequestedResourceVersion) reads resource versions from the per-player
# local storage. Without the source account's player id in PlayerPrefs the
# player is 000000000 and that storage is empty, so the client cannot open the
# cached catalog and stays on the title screen. Android hooks the method to
# report the cache's version. Offline there is exactly one version, so here the
# method always returns it: its "nothing stored" path is taken unconditionally
# and loads a string literal rewritten to that version. The borrowed literal is
# a Mono BTLS error message (iOS uses Apple TLS and the offline client speaks
# plain HTTP, so it is never shown). The method's lazy metadata init is
# retargeted from the "" slot (used only on that path) to the borrowed slot.
RES_VERSION_LITERAL = "BTLS is not supported in this runtime."
DEFAULT_RES_VERSION = "R2606002@RQMqABAxdj0b5c8TKh4+PNDUYQ=="
_RES_VERSION_SLOT = 0x985C468
RES_VERSION_PATCHES = (
    (0x31CD388, "Core::GetStoredResourceVersion init: \"\" slot -> version literal slot", "203403b0",
     _adrp(0x31CD388, _RES_VERSION_SLOT, 0)),
    (0x31CD38C, "Core::GetStoredResourceVersion init: slot offset", "00e03f91",
     _add_imm(0, 0, _RES_VERSION_SLOT & 0xFFF)),
    (0x31CD39C, "Core::GetStoredResourceVersion: ignore per-player storage", "f40400b4", "27000014"),
    (0x31CD438, "Core::GetStoredResourceVersion: return version literal", "283403b0",
     _adrp(0x31CD438, _RES_VERSION_SLOT, 8)),
    (0x31CD43C, "Core::GetStoredResourceVersion: slot offset", "08e13f91",
     _add_imm(8, 8, _RES_VERSION_SLOT & 0xFFF)),
)

def _ldr_x(rt: int, rn: int, offset: int) -> str:
    word = 0xF9400000 | (((offset >> 3) & 0xFFF) << 10) | (rn << 5) | rt
    return word.to_bytes(4, "little").hex()


# IdentityStorage.get_PlayerId reads the player id from (encrypted) PlayerPrefs.
# Without the source account's entry it returns null, and Home preparation
# (ProfileDecoration.PrepareByCustomData and some 30 other callers) throws
# NullReferenceException, so LOGIN never leaves the title screen. Android seeds
# the missing identity with player 000000000 through the game's SaveIdentity;
# here the getter returns that id as a string literal. The borrowed literal is
# a Mono BTLS error message (iOS uses Apple TLS, so it is never shown), and the
# getter's lazy metadata init is retargeted from the PlayerPrefs key slot
# (used only by the replaced load) to the borrowed slot.
PLAYER_ID_LITERAL = "Attempting to modify read-only MonoBtlsX509VerifyParam instance."
OFFLINE_PLAYER_ID = "000000000"
_PLAYER_ID_SLOT = 0x985C160
PLAYER_ID_PATCHES = (
    (0x31CEE6C, "IdentityStorage::get_PlayerId init: key slot -> player id literal slot", "c03103f0",
     _adrp(0x31CEE6C, _PLAYER_ID_SLOT, 0)),
    (0x31CEE70, "IdentityStorage::get_PlayerId init: slot offset", "00000991",
     _add_imm(0, 0, _PLAYER_ID_SLOT & 0xFFF)),
    (0x31CEE80, "IdentityStorage::get_PlayerId: player id literal page", "601e40f9",
     _adrp(0x31CEE80, _PLAYER_ID_SLOT, 8)),
    (0x31CEE84, "IdentityStorage::get_PlayerId: load player id literal", "e00000b4",
     _ldr_x(0, 8, _PLAYER_ID_SLOT & 0xFFF)),
    (0x31CEE88, "IdentityStorage::get_PlayerId: restore frame", "c83103f0", "fd7b41a9"),
    (0x31CEE8C, "IdentityStorage::get_PlayerId: restore registers", "1f2003d5", "f44fc2a8"),
    (0x31CEE90, "IdentityStorage::get_PlayerId: return", "012141f9", "c0035fd6"),
)

# IdentityStorage.get_DeviceSpecificId (same shape, field 0x40) is null for the
# same reason. After login the client puts it into the API default headers
# (APICommon.AddRequestHeaderValue "x-device-specific-id"); API calls tolerate
# the null value, but every web view (メニュー > ヘルプ and the other support
# pages) copies the default headers through WebViewObject.AddCustomHeader and
# the null string aborts the app in -[NSPlaceholderString initWithBytes:...].
# It returns the same offline id literal.
DEVICE_ID_PATCHES = (
    (0x31CEEC0, "IdentityStorage::get_DeviceSpecificId init: key slot -> player id literal slot", "c03103f0",
     _adrp(0x31CEEC0, _PLAYER_ID_SLOT, 0)),
    (0x31CEEC4, "IdentityStorage::get_DeviceSpecificId init: slot offset", "00000991",
     _add_imm(0, 0, _PLAYER_ID_SLOT & 0xFFF)),
    (0x31CEED4, "IdentityStorage::get_DeviceSpecificId: player id literal page", "602240f9",
     _adrp(0x31CEED4, _PLAYER_ID_SLOT, 8)),
    (0x31CEED8, "IdentityStorage::get_DeviceSpecificId: load player id literal", "e00000b4",
     _ldr_x(0, 8, _PLAYER_ID_SLOT & 0xFFF)),
    (0x31CEEDC, "IdentityStorage::get_DeviceSpecificId: restore frame", "c83103f0", "fd7b41a9"),
    (0x31CEEE0, "IdentityStorage::get_DeviceSpecificId: restore registers", "1f2003d5", "f44fc2a8"),
    (0x31CEEE4, "IdentityStorage::get_DeviceSpecificId: return", "012141f9", "c0035fd6"),
)

# Hailstorm's AssetCatalogSynchronizer.SynchronizeRemoteCatalog downloads the
# resource manifest unless the saved resource version equals the requested
# one. The saved version is per-player local storage: without the source
# account's player id in PlayerPrefs (player 000000000) it is empty and the
# client starts a manifest download, which offline ends in a download error.
# There is no CDN to synchronize with offline, so always take the "versions
# equal" branch: the cache's own catalog database stays in use. This replaces
# the source player id entry in the storage-key.json of an iOS install
# (Android does the same through the Core.GetStoredResourceVersion hook).
CATALOG_SYNC_PATCHES = (
    (0x323B564, "AssetCatalogSynchronizer.SynchronizeRemoteCatalog: treat versions as equal", "60200037", "03010014"),
)

# The title screen prints "ID <player id>" from PlayerPrefs.  The iOS build
# reuses the Android cache, so that ID belongs to the source account, not to
# this client.  TitleSceneController.SetPlayerId already shows an empty label
# for an empty ID; drop its `cbz` so it always takes that path.
TITLE_ID_PATCHES = (
    (0x24DE3C8, "TitleSceneController::SetPlayerId always empty label", "a0000034", "1f2003d5"),
)

# Destructive account/cache actions.  Offline, キャッシュ削除 would wipe the
# imported ~20 GB asset cache (no longer downloadable) and logout / user
# deletion would reset the local identity.  Android blocks them with a
# Localify hook; iOS has no runtime hooks, so the handlers (all void) start
# with ``ret`` instead: the buttons do nothing.  Same method list as the
# Android OfflineGuard (HookShare.cpp).
_RET = "c0035fd6"
DESTRUCTIVE_ACTION_PATCHES = (
    (0x24D8C2C, "TitleMenuPopup::OnClickCacheClear", "ff0302d1", _RET),
    (0x24D8EA4, "TitleMenuPopup::OnClickLogout", "ff0302d1", _RET),
    (0x24D8F3C, "TitleMenuPopup::OnClickDeleteUser", "ffc302d1", _RET),
    (0x2EC57D8, "TPopupSetting::OnClickCacheClear", "ff8302d1", _RET),
    (0x2EC8A20, "TPopupSettingMenu::OnClickDeleteUserPositiveButton", "ff4302d1", _RET),
    (0x2EC8AD4, "TPopupSettingMenu::Logout", "ff8302d1", _RET),
    (0x2EC8B8C, "TPopupSettingMenu::OnClickLogoutPositiveButton", "f44fbea9", _RET),
    (0x31CDB64, "Core::RecreateCurrentSavedata", "f657bda9", _RET),
)


@dataclass(frozen=True)
class LiteralPatch:
    index: int
    old: str
    new: str


def _literal_tables(data: bytes) -> tuple[int, int, int, int]:
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != 0xFAB11BAF or version != 31:
        raise IosError("expected IL2CPP metadata v31")
    table_offset, table_size = struct.unpack_from("<II", data, 8)
    data_offset, data_size = struct.unpack_from("<II", data, 16)
    if table_size % 8:
        raise IosError("misaligned string literal table")
    return table_offset, table_size // 8, data_offset, data_size


def rewrite_literals(data: bytes, rewrite) -> tuple[bytes, list[LiteralPatch]]:
    """Apply ``rewrite(old) -> new | None`` to every string literal.

    A replacement must be ASCII and no longer than the original, and the
    literal's data range must not be shared with another literal.
    """
    table_offset, count, data_offset, data_size = _literal_tables(data)
    entries = [struct.unpack_from("<Ii", data, table_offset + i * 8) for i in range(count)]
    out = bytearray(data)
    patches: list[LiteralPatch] = []
    for index, (length, data_index) in enumerate(entries):
        if data_index < 0 or data_index + length > data_size:
            raise IosError(f"string literal {index} is out of range")
        raw = data[data_offset + data_index:data_offset + data_index + length]
        try:
            old = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        new = rewrite(old)
        if new is None or new == old:
            continue
        encoded = new.encode("ascii")
        if len(encoded) > length:
            raise IosError(f"replacement for {old!r} is longer than the original")
        start, end = data_index, data_index + length
        for other, (other_length, other_index) in enumerate(entries):
            if other != index and other_index < end and start < other_index + other_length:
                raise IosError(f"string literal {index} shares data with literal {other}")
        struct.pack_into("<I", out, table_offset + index * 8, len(encoded))
        out[data_offset + start:data_offset + start + len(encoded)] = encoded
        patches.append(LiteralPatch(index, old, new))
    return bytes(out), patches


def rewrite_default_values(data: bytes, rewrite) -> tuple[bytes, list[LiteralPatch]]:
    """Rewrite string field/parameter default values (metadata section 8).

    Strings there are stored as a zigzag varint length (2*n for short
    strings, one byte while n < 64) followed by UTF-8 bytes. C# inlines
    constants and optional-parameter defaults as literals, so this only keeps
    reflection-visible defaults consistent with the literal patch.
    """
    offset, size = struct.unpack_from("<II", data, 8 + 8 * 8)
    out = bytearray(data)
    patches: list[LiteralPatch] = []
    section = data[offset:offset + size]
    for target in {API_BASE, ASSET_BASE}:
        encoded_target = target.encode()
        start = 0
        while (found := section.find(encoded_target, start)) != -1:
            start = found + 1
            if found == 0 or section[found - 1] != 2 * len(encoded_target):
                continue
            # Only exact strings: the next byte begins another entry.
            new = rewrite(target)
            if new is None:
                continue
            encoded = new.encode("ascii")
            if len(encoded) > len(encoded_target) or len(encoded) >= 64:
                raise IosError(f"default-value replacement for {target!r} too long")
            base = offset + found
            out[base - 1] = 2 * len(encoded)
            out[base:base + len(encoded)] = encoded
            patches.append(LiteralPatch(-1, target, new))
    return bytes(out), patches


def offline_rewriter(port: int, redirect_assets: bool):
    local = f"http://127.0.0.1:{port}"

    def rewrite(value: str) -> str | None:
        if value == API_BASE:
            return local
        if redirect_assets and value.startswith(ASSET_BASE):
            return local + "/a" + value[len(ASSET_BASE):]
        return None

    return rewrite


def rewrite_show_preview_bgm(data: bytes) -> tuple[bytes, list[LiteralPatch]]:
    """Use the archived Daily BGM for the unavailable SHOW settings preview.

    Both the cue and ACB name must agree.  This only changes the preview
    resource identity; the rhythm option controls and gameplay assets are not
    rewritten.  Fail closed on an unknown client metadata layout.
    """
    replacements = {
        SHOW_PREVIEW_BGM: SHOW_PREVIEW_FALLBACK_BGM,
        SHOW_PREVIEW_BGM + ".acb": SHOW_PREVIEW_FALLBACK_BGM + ".acb",
    }
    changed, patches = rewrite_literals(data, replacements.get)
    if sorted(p.old for p in patches) != sorted(replacements):
        raise IosError("SHOW preview BGM literals differ from the verified 5.0.1 client")
    return changed, patches


def patch_code(framework: bytes, table) -> tuple[bytes, list[dict]]:
    """Apply verified (address, name, old, new) byte patches.  Fail closed."""
    image = MachO(framework)
    out = bytearray(framework)
    patches = []
    for address, name, old_hex, new_hex in table:
        old, new = bytes.fromhex(old_hex), bytes.fromhex(new_hex)
        if not image.is_code(address):
            raise IosError(f"{name}: {address:#x} is not code")
        offset = image.vm_to_file(address, len(old))
        if framework[offset:offset + len(old)] != old:
            raise IosError(f"{name}: bytes differ from the verified 5.0.1 client")
        out[offset:offset + len(new)] = new
        patches.append({"address": hex(address), "name": name, "old": old_hex, "new": new_hex})
    return bytes(out), patches


def disable_lyric_video(framework: bytes) -> tuple[bytes, list[dict]]:
    """Patch UnityFramework so SHOW never requests a lyric video."""
    return patch_code(framework, LYRIC_VIDEO_PATCHES)


def apply_compat_patches(framework: bytes) -> tuple[bytes, list[dict]]:
    """5.0.1 client fixes needed by the later resources (always safe)."""
    return patch_code(framework, COMPAT_PATCHES)


def block_story_playback(framework: bytes) -> tuple[bytes, list[dict]]:
    """Close the story 再生確認 dialog instead of starting a failing download."""
    return patch_code(framework, STORY_PLAYBACK_PATCHES)


def block_chapter_portal(framework: bytes) -> tuple[bytes, list[dict]]:
    """Show the Chapter period alert instead of opening the unsaved portal."""
    return patch_code(framework, CHAPTER_PORTAL_PATCHES)


def fixed_res_version(framework: bytes) -> tuple[bytes, list[dict]]:
    """Report the cache's resource version regardless of the player."""
    return patch_code(framework, RES_VERSION_PATCHES)


def rewrite_res_version_literal(data: bytes, res_version: str) -> tuple[bytes, list[LiteralPatch]]:
    if not re.fullmatch(r"R\d+(@[A-Za-z0-9+/=]+)?", res_version) or len(res_version) > len(RES_VERSION_LITERAL):
        raise IosError(f"resource version {res_version!r} is not a resource version of at most "
                       f"{len(RES_VERSION_LITERAL)} characters")
    changed, patches = rewrite_literals(data, {RES_VERSION_LITERAL: res_version}.get)
    if len(patches) != 1:
        raise IosError("resource version carrier literal differs from the verified 5.0.1 client")
    return changed, patches


def fixed_player_id(framework: bytes) -> tuple[bytes, list[dict]]:
    """Report the offline id as player id and device id instead of reading PlayerPrefs."""
    return patch_code(framework, PLAYER_ID_PATCHES + DEVICE_ID_PATCHES)


def rewrite_player_id_literal(data: bytes, player_id: str = OFFLINE_PLAYER_ID) -> tuple[bytes, list[LiteralPatch]]:
    if not re.fullmatch(r"\d{1,20}", player_id):
        raise IosError(f"player id {player_id!r} must be 1-20 digits")
    changed, patches = rewrite_literals(data, {PLAYER_ID_LITERAL: player_id}.get)
    if len(patches) != 1:
        raise IosError("player id carrier literal differs from the verified 5.0.1 client")
    return changed, patches


def skip_catalog_sync(framework: bytes) -> tuple[bytes, list[dict]]:
    """Keep the cached catalog instead of downloading the manifest."""
    return patch_code(framework, CATALOG_SYNC_PATCHES)


def hide_title_player_id(framework: bytes) -> tuple[bytes, list[dict]]:
    """Do not print the (source account's) player ID on the title screen."""
    return patch_code(framework, TITLE_ID_PATCHES)


def guard_destructive_actions(framework: bytes) -> tuple[bytes, list[dict]]:
    """Make cache delete / logout / user delete handlers no-ops."""
    return patch_code(framework, DESTRUCTIVE_ACTION_PATCHES)


def patch_info_plist(info: dict, bundle_id: str) -> dict:
    info = dict(info)
    info["CFBundleIdentifier"] = bundle_id
    info["CFBundleDisplayName"] = "Linkura Offline"
    ats = dict(info.get("NSAppTransportSecurity") or {})
    ats["NSAllowsArbitraryLoads"] = True
    ats["NSAllowsLocalNetworking"] = True
    info["NSAppTransportSecurity"] = ats
    info["UIFileSharingEnabled"] = True
    info["LSSupportsOpeningDocumentsInPlace"] = True
    return info


def stage_offline_ios_client(
    source: Path,
    dylib: Path,
    output: Path,
    *,
    bundle_id: str = DEFAULT_BUNDLE_ID,
    port: int = DEFAULT_PORT,
    redirect_assets: bool = True,
    show_preview_bgm_fallback: bool = False,
    disable_lyric_videos: bool = False,
    guard_destructive: bool = True,
    compat_patches: bool = True,
    block_story: bool = True,
    hide_title_id: bool = True,
    block_chapter: bool = True,
    skip_catalog: bool = True,
    res_version: str | None = DEFAULT_RES_VERSION,
    player_id: str | None = OFFLINE_PLAYER_ID,
) -> dict:
    source, dylib, output = (p.expanduser().resolve() for p in (source, dylib, output))
    manifest = output.with_suffix(output.suffix + ".json")
    if output.exists() or manifest.exists() or output in (source, dylib):
        raise IosError("refusing to overwrite the source, dylib, output or manifest")
    if not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", bundle_id):
        raise IosError("invalid bundle identifier")
    if not 1024 <= port <= 65535:
        raise IosError("port must be 1024-65535")
    dylib_data = dylib.read_bytes()
    image = MachO(dylib_data)
    if image.filetype != 6 or 2 not in image.platforms or any(e["cryptid"] for e in image.encryption):
        raise IosError("dylib must be an unencrypted arm64 iPhoneOS dylib")

    with zipfile.ZipFile(source) as archive:
        app, info, members = app_info(archive)
        if bundle_id == info.get("CFBundleIdentifier"):
            raise IosError("bundle id must differ from the official app")
        main = app + info["CFBundleExecutable"]
        main_data = archive.read(main)
        if any(e["cryptid"] for e in MachO(main_data).encryption):
            raise IosError("the IPA is encrypted; use the decrypted IPA")
        framework = app + FRAMEWORK_MEMBER
        if framework in members and any(e["cryptid"] for e in MachO(archive.read(framework)).encryption):
            raise IosError("UnityFramework is encrypted; use the decrypted IPA")
        if app + "Frameworks/" + DYLIB_NAME in members:
            raise IosError("LinkuraOffline.dylib is already embedded; start from the original IPA")
        metadata_member = app + METADATA_MEMBER
        if metadata_member not in members:
            raise IosError("global-metadata.dat not found")
        rewriter = offline_rewriter(port, redirect_assets)
        metadata, patches = rewrite_literals(archive.read(metadata_member), rewriter)
        metadata, default_patches = rewrite_default_values(metadata, rewriter)
        patches += default_patches
        if not any(p.old == API_BASE for p in patches):
            raise IosError(f"API base URL literal {API_BASE!r} not found in metadata")
        if show_preview_bgm_fallback:
            metadata, preview_patches = rewrite_show_preview_bgm(metadata)
            patches += preview_patches
        if res_version:
            metadata, version_patches = rewrite_res_version_literal(metadata, res_version)
            patches += version_patches
        if player_id:
            metadata, player_patches = rewrite_player_id_literal(metadata, player_id)
            patches += player_patches

        replacements = {
            app + "Info.plist": plistlib.dumps(patch_info_plist(info, bundle_id)),
            metadata_member: metadata,
            main: insert_dylib(main_data, DYLIB_INSTALL_NAME),
        }
        code_patches: list[dict] = []
        if (disable_lyric_videos or guard_destructive or compat_patches or block_story or hide_title_id
                or block_chapter or skip_catalog or res_version or player_id):
            if framework not in members:
                raise IosError("UnityFramework not found")
            framework_data = archive.read(framework)
            for enabled, patcher in ((compat_patches, apply_compat_patches),
                                     (disable_lyric_videos, disable_lyric_video),
                                     (guard_destructive, guard_destructive_actions),
                                     (block_story, block_story_playback),
                                     (hide_title_id, hide_title_player_id),
                                     (block_chapter, block_chapter_portal),
                                     (skip_catalog, skip_catalog_sync),
                                     (bool(res_version), fixed_res_version),
                                     (bool(player_id), fixed_player_id)):
                if enabled:
                    framework_data, applied = patcher(framework_data)
                    code_patches += applied
            replacements[framework] = framework_data
        additions = {app + "Frameworks/" + DYLIB_NAME: dylib_data}
        source_hash = sha256_file(source)
        output.parent.mkdir(parents=True, exist_ok=True)
        removed = []
        with tempfile.TemporaryDirectory(prefix="ios-offline-", dir=output.parent) as temporary:
            staged = Path(temporary) / "unsigned.ipa"
            with zipfile.ZipFile(staged, "w", zipfile.ZIP_DEFLATED, compresslevel=3) as target:
                for name, member in members.items():
                    if "/_CodeSignature/" in name or name.endswith("/embedded.mobileprovision"):
                        removed.append(name)
                        continue
                    target.writestr(member, replacements.get(name, b"") if name in replacements
                                    else archive.read(member))
                for name, data in additions.items():
                    member = zipfile.ZipInfo(name)
                    member.compress_type = zipfile.ZIP_DEFLATED
                    member.external_attr = (stat.S_IFREG | 0o755) << 16
                    target.writestr(member, data)
            with zipfile.ZipFile(staged) as check:
                if check.testzip() is not None:
                    raise IosError("staged IPA ZIP verification failed")
                if DYLIB_INSTALL_NAME not in MachO(check.read(main)).dependencies:
                    raise IosError("LC_LOAD_DYLIB verification failed")
                rewrite_literals(check.read(metadata_member), lambda value: None)
            if sha256_file(source) != source_hash:
                raise IosError("source IPA changed during staging")
            with staged.open("rb") as inp, output.open("xb") as out:
                shutil.copyfileobj(inp, out)

    report = {
        "schemaVersion": 1,
        "artifactType": "unsigned-ios-offline",
        "source": {"path": str(source), "sha256": source_hash,
                   "clientVersion": info.get("CFBundleShortVersionString")},
        "dylib": {"path": str(dylib), "sha256": sha256_file(dylib)},
        "artifact": {"path": str(output), "sha256": sha256_file(output), "bundleIdentifier": bundle_id},
        "port": port,
        "showPreviewBgmFallback": show_preview_bgm_fallback,
        "disableLyricVideos": disable_lyric_videos,
        "guardDestructiveActions": guard_destructive,
        "compatPatches": compat_patches,
        "blockStoryPlayback": block_story,
        "hideTitlePlayerId": hide_title_id,
        "blockChapterPortal": block_chapter,
        "skipCatalogSync": skip_catalog,
        "fixedResVersion": res_version,
        "fixedPlayerId": player_id,
        "codePatches": code_patches,
        "literalPatches": [{"index": p.index, "old": p.old, "new": p.new} for p in patches],
        "removedSignatureFiles": removed,
        "signingRequired": True,
        "runtimeValidated": False,
    }
    with manifest.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    return report
