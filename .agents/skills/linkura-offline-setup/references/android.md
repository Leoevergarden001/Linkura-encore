# Android setup

Use the current [Android guide](../../../../docs/en/android.md) and [resource guide](../../../../docs/en/resources.md) for detailed requirements.

## Prepare

Run `adb devices -l` and select the user's device by its exact serial. Confirm that it is online and has enough space for the extracted cache. Check whether the game is already installed and whether replacing its APK would preserve data. The offline APK has a different signature from the official APK; do not automatically uninstall the official app to work around `INSTALL_FAILED_UPDATE_INCOMPATIBLE`.

If the user has an offline APK, use it as supplied after checking its version and hash when available. To build one from the user's client:

```sh
cd android
JAVA_HOME=<JDK 17 path> ./gradlew :app:assembleDebug
cd ..
python3 -m tools.linkura_offline android-repack \
  --java <Java 21+ executable> \
  --source <user's 5.1.0 APK> \
  --output <offline APK>
```

The module output is `android/app/build/outputs/apk/debug/app-debug.apk`. The repacker uses the repository's LSPatch loader by default. Inspect its neighboring build manifest and the output APK before installation.

If the user already has a resource pack, point the installer at its extracted pack directory. The installer does not accept `.tar.zst` directly. If building a new pack from the user's own snapshots, use the current `android-resource-pack-build` command and keep its rekeying default:

```sh
python3 -m tools.linkura_offline android-resource-pack-build \
  --snapshot <external-cache.tar> \
  --private <private-files.tar> \
  --output <new private pack directory>
```

Check the output's `files/D/`, both `files/M/` databases, and `files/linkura-local/storage-key.json` without displaying key values. Keep source snapshots unchanged.

## Install and verify

```sh
adb -s <serial> install -r <offline APK>
python3 -m tools.linkura_offline android-resource-pack-install \
  --pack <extracted pack directory> --serial <serial>
adb -s <serial> logcat -s LinkuraOffline LinkuraLocal-Native
```

The pack installer launches the app once, transfers the files with ADB, and checks file counts. It does not prove every asset's content hash or gameplay coverage. If the user can operate the device, test a cold start in airplane mode, title-screen LOGIN, and Home. Record the observed result and any missing-asset or API errors. Leave existing player data in place.
