import os
import struct
import unittest
import zipfile
from pathlib import Path

from tools.linkura_offline.ios_macho import IosError
from tools.linkura_offline.ios_offline import (
    API_BASE,
    ASSET_BASE,
    FRAMEWORK_MEMBER,
    COMPAT_PATCHES,
    DESTRUCTIVE_ACTION_PATCHES,
    LYRIC_VIDEO_PATCHES,
    apply_compat_patches,
    block_chapter_portal,
    block_story_playback,
    hide_title_player_id,
    disable_lyric_video,
    guard_destructive_actions,
    offline_rewriter,
    patch_info_plist,
    rewrite_default_values,
    rewrite_literals,
    rewrite_show_preview_bgm,
)

HEADER = 256  # v31 header: magic, version, 31 (offset, size) pairs


def make_metadata(literals: list[str], defaults: bytes = b"") -> bytes:
    blob = b"".join(s.encode() for s in literals)
    table = b""
    index = 0
    for s in literals:
        table += struct.pack("<Ii", len(s.encode()), index)
        index += len(s.encode())
    sections = {0: table, 1: blob, 8: defaults}
    header = bytearray(HEADER)
    struct.pack_into("<II", header, 0, 0xFAB11BAF, 31)
    body = b""
    for i in range(31):
        data = sections.get(i, b"")
        struct.pack_into("<II", header, 8 + i * 8, HEADER + len(body), len(data))
        body += data
    return bytes(header) + body


def literal_values(data: bytes) -> list[str]:
    table_offset, table_size = struct.unpack_from("<II", data, 8)
    data_offset, _ = struct.unpack_from("<II", data, 16)
    out = []
    for i in range(table_size // 8):
        length, index = struct.unpack_from("<Ii", data, table_offset + i * 8)
        out.append(data[data_offset + index:data_offset + index + length].decode())
    return out


class LiteralRewriteTest(unittest.TestCase):
    def test_api_and_asset_literals_point_at_local_server(self):
        metadata = make_metadata([API_BASE, ASSET_BASE + "/ios", "R2604001", ASSET_BASE + "/raw/"])
        out, patches = rewrite_literals(metadata, offline_rewriter(52680, True))
        self.assertEqual(literal_values(out), [
            "http://127.0.0.1:52680", "http://127.0.0.1:52680/a/ios", "R2604001", "http://127.0.0.1:52680/a/raw/"])
        self.assertEqual(len(patches), 3)
        self.assertEqual(len(out), len(metadata))

    def test_keep_asset_urls(self):
        metadata = make_metadata([API_BASE, ASSET_BASE + "/ios"])
        out, _ = rewrite_literals(metadata, offline_rewriter(52680, False))
        self.assertEqual(literal_values(out), ["http://127.0.0.1:52680", ASSET_BASE + "/ios"])

    def test_rejects_longer_replacement_and_bad_version(self):
        metadata = make_metadata(["https://x.y"])
        with self.assertRaises(IosError):
            rewrite_literals(metadata, lambda v: v + "/longer" if v.startswith("https") else None)
        bad = bytearray(metadata)
        struct.pack_into("<I", bad, 4, 29)
        with self.assertRaises(IosError):
            rewrite_literals(bytes(bad), lambda v: None)


class DefaultValueRewriteTest(unittest.TestCase):
    def test_length_prefixed_strings(self):
        def entry(s: str) -> bytes:
            return bytes([2 * len(s)]) + s.encode()
        defaults = entry(API_BASE) + entry(ASSET_BASE) + entry("R2604001")
        metadata = make_metadata(["x"], defaults)
        out, patches = rewrite_default_values(metadata, offline_rewriter(52680, True))
        offset, size = struct.unpack_from("<II", out, 8 + 8 * 8)
        section = out[offset:offset + size]
        local = b"http://127.0.0.1:52680"
        self.assertEqual(section[0], 2 * len(local))
        self.assertEqual(section[1:1 + len(local)], local)
        self.assertEqual(len(patches), 2)
        self.assertIn(b"R2604001", section)


class ShowPreviewBgmRewriteTest(unittest.TestCase):
    def test_rewrites_only_preview_cue_and_acb(self):
        metadata = make_metadata([
            "bgm_rhythm_system_preview_0001",
            "bgm_rhythm_system_preview_0001.acb",
            "rhythmgame_chart_system_preview.bytes",
        ])
        out, patches = rewrite_show_preview_bgm(metadata)
        self.assertEqual(literal_values(out), [
            "bgm_quest_daily_0001",
            "bgm_quest_daily_0001.acb",
            "rhythmgame_chart_system_preview.bytes",
        ])
        self.assertEqual(len(patches), 2)
        self.assertEqual(len(out), len(metadata))

    def test_rejects_unknown_client_layout(self):
        metadata = make_metadata(["bgm_rhythm_system_preview_0001"])
        with self.assertRaises(IosError):
            rewrite_show_preview_bgm(metadata)


# The decrypted 5.0.1 IPA is not in the repository; point LINKURA_IPA at it.
SOURCE_IPA = Path(os.environ.get("LINKURA_IPA", "com.oddno.lovelive_5.0.1.ipa"))


@unittest.skipUnless(SOURCE_IPA.exists(), "set LINKURA_IPA to the decrypted 5.0.1 IPA")
class LyricVideoPatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(SOURCE_IPA) as archive:
            member = next(n for n in archive.namelist() if n.endswith(".app/" + FRAMEWORK_MEMBER))
            cls.framework = archive.read(member)

    def test_patches_exactly_the_verified_sites(self):
        out, patches = disable_lyric_video(self.framework)
        self.assertEqual(len(patches), len(LYRIC_VIDEO_PATCHES))
        self.assertEqual(len(out), len(self.framework))
        changed = [i for i in range(0, len(out), 4) if out[i:i + 4] != self.framework[i:i + 4]]
        expected = sorted(a + k for a, _, old, new in LYRIC_VIDEO_PATCHES
                          for k in range(0, len(new) // 2, 4) if old[2 * k:2 * k + 8] != new[2 * k:2 * k + 8])
        self.assertEqual(changed, expected)

    def test_destructive_guard_and_lyric_patches_combine(self):
        out, lyric = disable_lyric_video(self.framework)
        out, guard = guard_destructive_actions(out)
        self.assertEqual(len(guard), len(DESTRUCTIVE_ACTION_PATCHES))
        changed = sum(1 for i in range(0, len(out), 4) if out[i:i + 4] != self.framework[i:i + 4])
        self.assertEqual(changed, 6 + len(DESTRUCTIVE_ACTION_PATCHES))  # lyric: 3 getters + 1 extra word + 2 branches

    def test_dream_member_find_redirect(self):
        out, applied = apply_compat_patches(self.framework)
        self.assertEqual(len(applied), len(COMPAT_PATCHES))
        word = int.from_bytes(out[0x2F08398:0x2F0839C], "little")
        self.assertEqual(word >> 26, 0b100101)  # still a BL
        self.assertEqual(0x2F08398 + ((word & 0x3FFFFFF) << 2), 0x330DC88)

    def test_story_ok_branches_to_cancel(self):
        out, _ = block_story_playback(self.framework)
        word = int.from_bytes(out[0x2D3DE94:0x2D3DE98], "little")
        self.assertEqual(word >> 26, 0b000101)  # B, not BL
        self.assertEqual(0x2D3DE94 + ((word & 0x3FFFFFF) << 2), 0x2D3DFC8)

    def test_chapter_rank_tap_takes_period_alert_branch(self):
        before = int.from_bytes(self.framework[0x2EB0FE8:0x2EB0FEC], "little")
        self.assertEqual(before >> 24, 0x34)  # cbz w0, <alert>
        alert = 0x2EB0FE8 + (((before >> 5) & 0x7FFFF) << 2)
        out, applied = block_chapter_portal(self.framework)
        word = int.from_bytes(out[0x2EB0FE8:0x2EB0FEC], "little")
        self.assertEqual(word >> 26, 0b000101)  # unconditional B
        self.assertEqual(0x2EB0FE8 + ((word & 0x3FFFFFF) << 2), alert)
        self.assertEqual(alert, 0x2EB10C8)
        self.assertEqual(len(applied), 1)

    def test_title_id_hidden(self):
        out, applied = hide_title_player_id(self.framework)
        self.assertEqual(out[0x24DE3C8:0x24DE3CC].hex(), "1f2003d5")
        self.assertEqual(len(applied), 1)

    def test_refuses_already_patched_binary(self):
        out, _ = disable_lyric_video(self.framework)
        with self.assertRaises(IosError):
            disable_lyric_video(out)


class PlistTest(unittest.TestCase):
    def test_plist_changes(self):
        info = patch_info_plist({"CFBundleIdentifier": "com.oddno.lovelive",
                                 "NSAppTransportSecurity": {"NSExceptionDomains": {}}}, "org.example.lab")
        self.assertEqual(info["CFBundleIdentifier"], "org.example.lab")
        self.assertTrue(info["NSAppTransportSecurity"]["NSAllowsLocalNetworking"])
        self.assertIn("NSExceptionDomains", info["NSAppTransportSecurity"])
        self.assertTrue(info["UIFileSharingEnabled"])


if __name__ == "__main__":
    unittest.main()
