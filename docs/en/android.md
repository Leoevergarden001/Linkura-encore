<!-- Translation source: 1a59167456efd164e163fbb74cc42e36365b3580 (docs/zh/android.md) -->

# Android: build and install

The module uses Xposed/LSPatch. LSPatch bundles it with the game APK. After installation, it needs neither root nor a separate manager app.

## 1. Build the module

```sh
cd android
JAVA_HOME=<JDK 17> ./gradlew :app:assembleDebug
# Output: android/app/build/outputs/apk/debug/app-debug.apk
```

The native build includes the offline server (`server/src`) and embedded data (`server/data`, `server/db`) in `android/app/src/main/cpp`.

## 2. Repack the game APK

```sh
python3 -m tools.linkura_offline android-repack \
  --java <Java 21+ java> \
  --source <your 5.1.0 APK> \
  --output Linkura-5.1.0-offline.apk
```

- By default, the tool uses `android/lspatch/lspatch_loaderfix.jar`: LSPatch 0.7 plus a static `xposed.dummy` stub to prevent a crash on the first launch after a fresh install. See `android/lspatch/loaderfix/build.sh`.
- `--source` may also be an APK already repacked with LSPatch; the tool extracts its original APK and repacks it again.
- A neighboring `.json` file records hashes of the original APK, module, and output.

## 3. Install

```sh
adb install -r Linkura-5.1.0-offline.apk
```

If the official client is already installed, its signature differs. You must back up its data before uninstalling it; uninstalling removes that data.

<a id="android-import-resources"></a>

## 4. Import assets

See [Prepare assets](resources.md) to make a resource pack. Then import it:

```sh
python3 -m tools.linkura_offline android-resource-pack-install --pack <resource pack directory> --serial <adb serial>
```

The tool launches the game once to let the module create cache directories (on Android 11+, adb cannot create new subdirectories inside another app's external directory), then pushes the files with ordinary adb and checks the file count. The transfer takes roughly 11 minutes.

## 5. Play

Optionally enable airplane mode, start the game, and tap LOGIN on the title screen. On a cold start, the title screen can take more than ten seconds to respond.

## Configuration and debugging

- Built-in configuration is in the module APK at `assets/offline-config/`: `l4-config.json` (switches), `client-res.json` (client and asset versions), and `archive-config.json` (save data).
- To override without repacking, put files of the same names in the game's `files/linkura-local/config/` directory. Those take precedence.
- Logs: `adb logcat -s LinkuraOffline LinkuraLocal-Native`. Enable `dbgMode` in `l4-config.json` for more logs; `dumpHttpMockJson` records every response.
- The player database is `files/HasuKikaisann.sqlite3`. Write `reset` to `files/db_cmd` to erase it and regenerate a museum save on the next launch, or `rebuild` to regenerate from embedded data while preserving player state.

## Scope of the slimmed-down module

This module keeps only what offline play needs. The original linkura-localify free camera, translation and font replacement, rendering settings, story visibility switch, settings app, and floating window have been removed. The game therefore uses its default resolution and font.
