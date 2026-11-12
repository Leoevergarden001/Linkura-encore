<!-- Translation source: 501289cf3b617f15507c0039ea96fbc2e157595c (docs/zh/resources.md) -->

# Prepare assets

The official CDN is closed. Assets can only come from a cache downloaded by the game on a device you own.

## Cache layout

The game's external files directory is `/sdcard/Android/data/com.oddno.lovelive/files` on Android and `Documents/` in the iOS app container:

| Path | Contents |
|---|---|
| `D/<two-character prefix>/<RealName>` | Assets (Hailstorm cache): UnityFS bundles, audio (`acb`/`awb`), video (`usm`), encrypted text, etc. |
| `M/` | Databases (SQLCipher) |

Databases in `M/` are named and encrypted using **storage keys**. For each storage domain, the client stores a random 256-byte seed in PlayerPrefs (on Android, `shared_prefs/com.oddno.lovelive.v2.playerprefs.xml`):

| Storage domain | PlayerPrefs name | Database in `M/` | Needed offline? |
|---|---|---|---|
| `//catalog` | `JWbAatNSI1NpHhCdU%2BgJllY0BTU%3D` | Catalog database: maps assets to files in `D/` | Yes |
| `//master` | `Dr%2BcmYmVx%2B1kQ8uTmoYADAduWVk%3D` | Master database (about 57 MB) | Yes: the client cannot recreate an empty master database from cache |
| `app` | — | Player ID, local settings, etc. | No; it contains the original account identity and must be omitted |

- Database filename = lowercase, unpadded Base32(MD5(seed)).
- SQLCipher 4 raw key = SHA-256(seed) for both databases.
- `D/` assets use per-file seeds from the asset catalog, unrelated to the storage keys.

To move a cache to another device, bring both databases and their storage keys. The module writes the keys before Unity reads PlayerPrefs. By default, the packager **replaces the storage keys**: it re-encrypts and renames each database under a new seed and omits the `app` database. The packaged keys therefore belong to no original installation.

## Android: build a resource pack

Use an environment that can read the game's private directory (a rooted device or emulator). First create two tar files:

```sh
# External files (asset cache)
adb shell su -c 'tar -C /sdcard/Android/data/com.oddno.lovelive/files -cf /sdcard/game-external.tar D M'
# Private files (only the PlayerPrefs storage key is used)
adb shell su -c 'tar -C /data/data/com.oddno.lovelive -cf /sdcard/game-private.tar shared_prefs'
adb pull /sdcard/game-external.tar /sdcard/game-private.tar .
```

```sh
python3 -m tools.linkura_offline android-resource-pack-build \
  --snapshot game-external.tar [--delta <later-delta.tar> ...] \
  --private game-private.tar --output <resource pack directory>
```

The resource pack contains `D/`, freshly keyed catalog and master databases (the only two files in `M/`), their keys at `files/linkura-local/storage-key.json`, and `MANIFEST.json`. Rekeying requires the SQLCipher 4 CLI (`brew install sqlcipher`). `--keep-source-key` retains the original `M/` and storage keys instead. Do not use an exFAT output directory (macOS creates `._*` files). To transfer the pack, see [Android](android.md#android-import-resources).

To export only the storage key: `python3 -m tools.linkura_offline android-storage-key --prefs <playerprefs.xml> --output storage-key.json`.

An existing cache (for example, an older resource pack or iOS container) can also be rekeyed separately:

```sh
python3 -m tools.linkura_offline cache-rekey \
  --m-dir <cache M/> --storage-key <matching storage-key.json> --output <new directory>
```

Replace the old `M/` and `storage-key.json` with the two databases under `<new directory>/M/` and `<new directory>/storage-key.json`. If the source `storage-key.json` lacks a master database entry (as may happen with an iOS device export), only the catalog database is processed and the tool warns you. Such a cache cannot supply master data on a new device.

<a id="ios-from-android"></a>

## iOS: convert an Android cache

The iOS client reads the same cache layout and storage keys, but Unity rejects asset bundles built for a different platform, and Android bundles do not contain Metal shaders:

```sh
python3 tools/resources/android_to_ios.py \
  --cache <directory containing D/ and M/, e.g. resource pack files/> \
  --storage-key <resource pack files/linkura-local/storage-key.json> \
  --ipa <decrypted 5.0.1 IPA> \
  --output <iOS data directory>
```

- Retarget every UnityFS bundle from Android (13) to iOS (9).
- With `--ipa`, replace bundled shaders with same-name Metal shaders from the IPA, or an approximate substitute; otherwise materials appear magenta. UnityPy must parse each bundle, so this is slow.
- Copy other files unchanged; rekey the catalog and master databases as above (`--keep-source-key` disables this). A rerun skips already converted files and reuses the first run's keys.

Pass the resulting `container/` to `tools/ios_device_setup.py --backup`.

## Check cache coverage

```sh
python3 tools/resources/hailstorm.py coverage --catalog <catalog.json> --cache <directory containing D/>
```

The tool reports, by category, how many catalog entries are present and how many gigabytes are missing. Failed downloads leave 0–28-byte placeholders, which count as missing. `catalog.json` is the Hailstorm asset catalog, for example one saved while using inspix-hailstorm.

Encrypted raw assets (ResourceType 192), such as `story_main_*.txt` scripts, can be decrypted for inspection:

```sh
python3 tools/resources/hailstorm.py decrypt --catalog <catalog.json> --cache <directory> \
  --label story_main_10250101.txt --output story.txt
```
