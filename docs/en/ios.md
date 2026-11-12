<!-- Translation source: 501289cf3b617f15507c0039ea96fbc2e157595c (docs/zh/ios.md) -->

# iOS / iPadOS: build and install

On a non-jailbroken iOS device, runtime hooks are unavailable. Offline operation therefore uses both data and code changes:

- `LinkuraOffline.dylib` is bundled into the IPA's `Frameworks/` and loaded through `LC_LOAD_DYLIB` in the main executable. It runs the offline server at `127.0.0.1:52680`.
- API and asset URLs in IL2CPP metadata are changed to local addresses.
- UnityFramework receives static patches with original-byte verification; see [iOS patching](patching-ios.md).
- Info.plist receives a separate Bundle ID, allows cleartext HTTP, and enables file sharing.

## 1. Build the dylib

```sh
ios/build.sh                 # -> ios/build/LinkuraOffline.dylib
LINKURA_REMOTE=0 ios/build.sh  # omit development-only remote tap/screenshot tools
```

## 2. Create an unsigned IPA

```sh
python3 -m tools.linkura_offline ios-offline-stage \
  --source <decrypted 5.0.1 IPA> \
  --show-preview-bgm-fallback --disable-lyric-videos \
  --output Linkura-iOS-5.0.1-offline-unsigned.ipa
```

The default Bundle ID is `org.linkura.preservation.probe`; change it with `--bundle-id`. Run `--help` for the default patch groups and switches. Each patch checks the original bytes before writing; a mismatch stops packaging rather than producing a partial IPA.

## 3. Sign, install, and transfer assets in one step

Connect the device to a Mac by cable, trust the computer, and enable Developer Mode (Settings → Privacy & Security → Developer Mode). The device must be included in a provisioning profile. Run any app with the same Bundle ID and your Team once from Xcode; Xcode registers the device and creates a profile.

```sh
python3 tools/ios_device_setup.py \
  --backup <iOS data directory containing container/> \
  --unsigned Linkura-iOS-5.0.1-offline-unsigned.ipa \
  --device "<device name or UDID>" --launch --port 8791
```

The script selects an unexpired profile that includes the device and an unambiguous signing identity. It installs the app, transfers the asset cache, master database, and storage key, compares each file size, launches the app, and checks that the offline server is listening.

- Assets are about 21 GB; leave at least 22 GB free. Keep the device unlocked and awake during transfer. A rerun skips files already transferred.
- Use `--dry-run` to inspect the plan or `--verify-only` to verify without copying.
- Player progress is not transferred by default. Use `--include-progress` to transfer it; if the destination already has a database, first pull it back and then use `--overwrite-progress`.
- Free signing expires after seven days. Rerun the same command to reinstall over the existing app and retain its data.

An iOS data directory can be converted from an Android cache; see [Prepare assets](resources.md#ios-from-android).

To sign without installing: `tools/ios_sign_ipa.sh <unsigned.ipa> <profile.mobileprovision> "<signing identity>" <signed.ipa>`.

## Debugging

The dylib writes runtime information to `Documents/LinkuraOffline/` inside the app. File sharing exposes it through Finder and the Files app:

| File | Purpose |
|---|---|
| `server.log` | Each request's path and status, unmatched routes (`no route for`), and missing asset requests |
| `config.json` | Optional: `{"port": 52680, "resVersion": "..."}` |
| `storage-key.json` | Storage key written to PlayerPrefs before startup |
| `mock_api/v1/<path>.json` (and `.headers`) | Optional response override by path for quick experiments |

The player database is at `Library/Application Support/LinkuraOffline/HasuKikaisann.sqlite3`. The first launch generates a museum save automatically.
