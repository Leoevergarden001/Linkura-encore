# iOS / iPadOS setup

Use the current [iOS guide](../../../../docs/en/ios.md) and [resource guide](../../../../docs/en/resources.md) for detailed requirements.

## Prepare

Use a Mac with Xcode tools. Connect and trust the intended iPhone or iPad, enable Developer Mode, and identify its device name or UDID. Confirm that the signing profile covers that device and the offline Bundle ID. Keep the device unlocked and awake during the transfer and check that it has enough free space for the cache.

If the user has an offline unsigned IPA and a matching `container/` tree, use those inputs without rebuilding. If building from the user's decrypted 5.0.1 IPA:

```sh
LINKURA_REMOTE=0 ios/build.sh
python3 -m tools.linkura_offline ios-offline-stage \
  --source <user's decrypted 5.0.1 IPA> \
  --show-preview-bgm-fallback --disable-lyric-videos \
  --output <offline unsigned IPA>
```

The staged IPA is unsigned. Check the selected Bundle ID and the patch tool's original-byte verification result. If the cache came from Android, first convert it using `tools/resources/android_to_ios.py` as described in the resource guide; pointing the iOS installer at unconverted Android bundles is insufficient.

## Install and verify

The backup path must contain `container/`. Preview the plan, then sign, install, transfer, and verify:

```sh
python3 tools/ios_device_setup.py \
  --backup <directory containing container/> \
  --unsigned <offline unsigned IPA> \
  --device "<device name or UDID>" --dry-run
python3 tools/ios_device_setup.py \
  --backup <directory containing container/> \
  --unsigned <offline unsigned IPA> \
  --device "<device name or UDID>" --launch --port 8791
python3 tools/ios_device_setup.py \
  --backup <directory containing container/> \
  --device "<device name or UDID>" --skip-install --verify-only
```

The setup tool compares transferred file sizes and checks that the offline server listens after launch. It skips player progress by default. Do not add `--include-progress` or `--overwrite-progress` unless the user specifically wants that transfer. `--overwrite-progress` pulls the device's existing progress database back first; pass `--progress-backup-dir <private directory>` so that copy lands somewhere the user knows, and confirm it exists before continuing. Free development signing may expire after seven days; reinstall the same Bundle ID and signing identity over the existing app to retain its data.

When using a nondefault Bundle ID, pass the same `--bundle-id` to the `--verify-only` command. The progress-page `--port` option is separate from the offline server's port.

If the device is available for interaction, verify an offline title-screen LOGIN and Home after the file checks. Distinguish a signed IPA, a successful install, transferred assets, an offline-server check, and actual gameplay observations in the report.
