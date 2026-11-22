<!-- Translation source: 1a59167456efd164e163fbb74cc42e36365b3580 (docs/zh/getting-started.md) -->

# Getting started

## Supported versions

| Platform | Client | Asset version | Notes |
|---|---|---|---|
| Android | 5.1.0 (versionCode 109563) | `R2606002@RQMqABAxdj0b5c8TKh4+PNDUYQ==` | Last online release before shutdown. The module finds classes and methods by name, not by address. |
| iOS / iPadOS | 5.0.1 | Same (uses 5.1.0 assets) | Patches use fixed addresses and verify original bytes one by one. Only this build is supported (decrypted IPA SHA-256 starts with `5d65a125b7701f51…`). |

The asset version determines which cache the client accepts; it must match your cache. The default is in `android/app/src/main/assets/offline-config/client-res.json`.

## What you need to provide

1. **Client:** an Android 5.1.0 APK or a **decrypted** iOS 5.0.1 IPA. An IPA downloaded from the App Store is encrypted.
2. **Asset cache:** the game's previously downloaded `D/` assets and `M/` databases from its external files directory, plus the matching **storage key** in PlayerPrefs. Without the key, the client treats the cache as belonging to another installation and ignores it. See [Prepare assets](resources.md).
3. **Toolchain:**
   - Host build of the offline server: a C++23 compiler (clang 16+ / gcc 13+) and CMake (only to generate embedded-data headers).
   - Android: Android SDK, NDK 26.3, JDK 17; LSPatch repacking requires Java 21+.
   - iOS: Xcode with the iPhoneOS SDK and an Apple account. A free Personal Team works, but its signing lasts seven days.
   - Python 3.10+; generators need PyYAML, asset tools need `lz4` and `cryptography`, and iOS asset conversion also needs `UnityPy`.

## Get the code

```sh
git clone --recurse-submodules <repo>
# If already cloned:
git submodule update --init
```

`third_party/link-like-diff` contains game master data in YAML. It is needed only when regenerating embedded data.

## Next steps

- Android: [Build and install](android.md)
- iOS / iPadOS: [Build and install](ios.md)
- To inspect the offline server on a computer first, run `server/build-host.sh && python3 server/tests/smoke_test.py`; see [Offline server](server.md).

## Privacy

Exported PlayerPrefs, device backups, and their `M/` databases belong to the source installation. The `app` database also contains the original account's player ID. By default, the packaging tool replaces the catalog and master database storage keys with fresh ones and omits the `app` database (see [Prepare assets](resources.md)). Keep the original exports and backups on your own storage. Resource packs still contain game content and should not be shared. Do not commit any of these files: `.gitignore` cannot protect files placed in the wrong location, so inspect each commit yourself.
