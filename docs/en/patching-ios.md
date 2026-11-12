<!-- Translation source: 60f383ea76a885c71ddad0e2d24e2955fc4c6c78 (docs/zh/patching-ios.md) -->

# iOS patching: find and add patches

There are no runtime hooks on iOS. Client behavior is changed by statically patching UnityFramework during packaging. Each entry in `tools/linkura_offline/ios_offline.py` contains:

```python
(address, "description", "original bytes (little-endian hex)", "replacement bytes")
```

`patch_code` checks the original bytes and stops on a mismatch. This limits patches to the verified 5.0.1 build and avoids corrupting another version.

## Prepare

```sh
# Symbol table: address -> method name (from your IPA metadata and UnityFramework)
python3 -m tools.linkura_offline ios-symbols --ipa <IPA> --output sym.json
unzip -j <IPA> 'Payload/*.app/Frameworks/UnityFramework.framework/UnityFramework' -d .
alias re='python3 tools/re/il2cpp_re.py -f UnityFramework -s sym.json'
```

## Find a patch site: Chapter RANK example

1. **Find the entry by name:** `re find 'ChapterPoint::OnPointerClick'`. Async logic lives in a compiler-generated state machine: `re find '<OnPointerClick>d__6::MoveNext'`.
2. **Disassemble:** `re dis <address> 0x600` labels call targets, such as `-> Tecotec.CommonApiCache::get_CurrentChapterRank`, followed by `cbz w0, <notice branch>`.
3. **Check other callers:** `re callers 'CommonApiCache::get_CurrentChapterRank$'`. The top bar also uses this getter, so change the branch at the call site, not the getter.
4. **Read original bytes:** `re bytes 0x2eb0fe8` returns `00070034` (`cbz w0, +0xe0`).
5. **Write the new instruction:** unconditional `b +0xe0` is `0x14000000 | (0xe0 >> 2)` = `0x14000038`, little-endian `38000014`.
6. **Add a patch group:** give it a function, a `stage_offline_ios_client` switch, and a CLI `--allow-*` option. Add a test to `tools/tests/test_ios_offline.py` that verifies original and new instruction types and branch targets.
7. Package, install, and verify on a device. Check `server.log` to confirm the original request no longer appears.

## Common AArch64 encodings

| Purpose | Encoding |
|---|---|
| Return from a void function | `ret` = `c0035fd6` |
| No-op | `nop` = `1f2003d5` |
| Unconditional branch | `b off` = `0x14000000 \| ((off >> 2) & 0x3FFFFFF)` |
| Conditional branch target | `cbz/cbnz`: `imm19 = (word >> 5) & 0x7FFFF`; target = address + `imm19 << 2` |

## Existing patch groups

| Group | Effect | Disable |
|---|---|---|
| `COMPAT_PATCHES` | Compatibility between 5.1 assets and the 5.0.1 client (Dream Live member-panel lookup) | Cannot be disabled |
| `LYRIC_VIDEO_PATCHES` | Treat SHOW lyric MV as off because MV assets were not saved | Off by default; enable with `--disable-lyric-videos` |
| `DESTRUCTIVE_ACTION_PATCHES` | Return immediately from eight cache deletion, logout, and user deletion entry points | `--allow-destructive-actions` |
| `STORY_PLAYBACK_PATCHES` | Make OK in the story playback confirmation close the dialog | `--allow-story-playback` |
| `TITLE_ID_PATCHES` | Hide player ID on the title screen | — |
| `CHAPTER_PORTAL_PATCHES` | Tapping Chapter RANK shows the out-of-period notice | `--allow-chapter-portal` |
| `RES_VERSION_PATCHES` | Always report the cached asset version (`--res-version`), ignoring per-player local storage | `--per-player-res-version` |
| `CATALOG_SYNC_PATCHES` | Keep the cached catalog instead of downloading a new one | `--allow-catalog-sync` |
| `PLAYER_ID_PATCHES` | Fix player ID to 000000000, as on Android; no original account PlayerPrefs needed | `--keep-player-id` |

The last three groups let iOS run with only the catalog database's storage key. When a method must return a string, the patch repurposes an unused runtime string constant (a Mono BTLS error), writes the desired value, and points the method's metadata initialization at that slot.

Metadata string rewrites (API URLs, BGM names, etc.) live in the same file (`rewrite_literals`, `rewrite_default_values`). They shorten strings in place without shifting offsets.
