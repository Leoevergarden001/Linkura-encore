**English** | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

<!-- Translation source: 85ba8e531f12739a3a402a5389f6af013d337e19 (README.zh-CN.md) -->

# linkura-offline

An **unofficial** offline project for 「Link！Like！ラブライブ！蓮ノ空スクールアイドルクラブ」 (リンクラ). After the official servers stopped operating, it lets you keep playing offline with a client and asset cache you already have, and provides a base for further development and fixes.

> This project is unaffiliated with the official game. It aims to preserve the experience after the service ended and to support personal research and technical exchange. The game's developers, publishers, and other rights holders have not endorsed or authorized it.
>
> The maintainers do not offer paid installation, asset sales, or official support. The repository code is licensed under [GPL-3.0](LICENSE); the project's preservation and research purpose does not change the rights or obligations under that license.
>
> The software is provided as is, without any guarantee of completeness, stability, or compatibility with a particular device. Back up your data before using it. To the extent permitted by applicable law, the authors and contributors provide no warranty and accept no liability for losses arising from use or inability to use the software. See the GPL-3.0 terms for details.

## Features

The original client connects to an offline service on your device and reads game assets you have already saved. LOGIN on the title screen enters local game state: it does not contact the official servers or restore your old account. Android and iOS share the same offline server and player-state logic.

- **Home and everyday actions**: enter Home and open cards, missions, profile, collection, restored menus, and shop entries. Some entries show only an empty state or a protective notice; see the limitations below.
- **SHOW and スクステ**: play and finish SHOW (the rhythm game), and play modes of スクールアイドルステージ（スクステ） such as Daily Live, Quest Live, Learning Live, Grade Live, Dream Live, and Live Grand Prix. Available songs, visuals, and performances depend on your asset cache.
- **ガチャ and cards**: draw from the local pools that have been implemented, review draw history and points, and use card development, 編成, and item exchange. These pools and rates do not represent the full historical official configuration.
- **Local progress**: draws, owned cards, 編成, and stage results are stored in an on-device SQLite database and remain after restart. The offline player ID is fixed at `000000000`; it is not your former official account.
- **Asset preparation and transfer**: tools can measure cache coverage, build and import Android resource packs, and convert a suitable Android cache for iOS. Conversion cannot recover content that was never saved.

These features require a cache matching your client version; some content remains unavailable because assets are missing.

## Supported clients

| Platform | Currently supported version | Installation |
|---|---|---|
| Android | 5.1.0 | LSPatch bundles the offline module into the APK; the target device needs neither root nor a separate module manager |
| iOS / iPadOS | 5.0.1 | Offline components and patches are added to an IPA, which you install with your own Apple signing identity; the device does not need a jailbreak |

Only these client builds have been targeted and verified. The iOS static patches check original bytes, so they cannot be applied directly to another version or build. Both platforms need a matching asset version; see [version details](docs/en/getting-started.md).

## What you need

This repository contains code, build tools, and offline service data. It does not contain an original game APK or IPA, a complete asset cache, or account data. Cloning the repository alone will not produce a playable client.

1. **Client**: an Android 5.1.0 APK or a decrypted iOS 5.0.1 IPA. An ordinary encrypted App Store IPA cannot be used directly by the current iOS patching workflow.
2. **Assets**: the `D/` cache saved on your device, the catalog and master databases in `M/`, and the storage keys paired with both databases. Asset files alone, without the databases or keys, cannot complete a fresh-device installation. See [prepare assets](docs/en/resources.md).
3. **Computer and device**: Android installation needs ADB. iOS installation needs a Mac with Xcode tools, a trusted device with Developer Mode enabled, and a provisioning profile covering that device. Building from source also needs the relevant SDKs, compilers, and Python dependencies; see [getting started](docs/en/getting-started.md).

An asset cache can take tens of gigabytes, and building or transferring a pack needs additional space for temporary files. Resource packs and device backups may contain private data. Keep original archives intact and back up existing progress before changing a device. By default, the pack builder generates fresh keys for the catalog and master databases and omits the source account's `app` database.

## Choose an installation path

Both paths use the installation tools in this repository. If you have not obtained the code yet:

```sh
git clone --recurse-submodules https://github.com/LeoEvergarden/Linkura-offline.git
cd Linkura-offline
```

### If you already have an offline package and resource pack

- **Android**: use an offline APK and Android resource pack matching this version. Verify the files' SHA-256 checksums. If the pack is a `.tar.zst` archive, inspect it and extract it into a new directory first. The installer's `--pack` argument takes a **directory** containing `files/D/`, `files/M/`, and `files/linkura-local/storage-key.json`; it cannot take the compressed archive itself. Back up an existing official or differently signed installation before changing it: APKs with different signatures cannot be installed over one another. See [Android installation](docs/en/android.md).
- **iOS / iPadOS**: use an unsigned offline IPA and an asset directory containing `container/`. Follow the [iOS installation guide](docs/en/ios.md) to sign it with your own identity, install it, and transfer assets. If you already have an IPA signed for that device, use the installer's `--ipa` option instead (see `python3 tools/ios_device_setup.py --help`). Free development signing usually needs periodic renewal.

A prepared package skips the corresponding client build, but version checks and asset transfer remain necessary; an unsigned iOS IPA still needs signing. A resource pack does not replace the APK or IPA.

### Build from your own client and cache

Check your inputs against [getting started and supported versions](docs/en/getting-started.md), then follow [build and install on Android](docs/en/android.md) or [build and install on iOS](docs/en/ios.md). See [prepare assets](docs/en/resources.md) for cache extraction, rekeying, and Android-to-iOS conversion. The repository also provides an [agent setup skill](.agents/skills/linkura-offline-setup/SKILL.md) that selects a workflow based on your inputs; you still need to supply the client, assets, and device.

## Verify your installation

1. Take the device offline or enable airplane mode. Cold-start the game, tap LOGIN on the title screen, and confirm that Home opens.
2. Open a cached SHOW song or スクステ stage, and confirm that you can start, finish, and return.
3. Perform an action that changes local state and confirm its result remains after a restart. Do not test this by deleting the cache, logging out, or deleting the user.
4. If you see a download error, blank screen, or inaccessible page, compare it with the limitations below, then use the [regression checklist](docs/en/regression-checklist.md) and platform logs.

These checks establish only that the tested paths work with the cache you used; they do not prove that every song, stage, or asset is complete.

## Known limitations

The offline build cannot download missing assets from the closed official CDN. Story voices, lyric MVs, and archive videos that were not saved cannot play. Some notice, social, and Chapter pages show only an empty state or a message. To protect assets that cannot be downloaded again, キャッシュ削除, ログアウト, and ユーザー削除 are intercepted. See [known limitations](docs/en/known-limitations.md) for the full list and reasons.

## Project layout and development

| Directory | Purpose |
|---|---|
| `server/` | Offline APIs, response templates, and local player state shared by Android and iOS |
| `android/` | Android hooks, LSPatch module, and client-side offline fixes |
| `ios/` | iOS offline component; client static patches and packaging tools are in `tools/` |
| `tools/` | Client packaging, device installation, asset processing, and analysis |

For development, start with [architecture](docs/en/architecture.md), the [offline server](docs/en/server.md), [Android hooks](docs/en/hooking-android.md), and [iOS patching](docs/en/patching-ios.md). iOS assets can also live on your own server and be downloaded by the app; see [remote asset host](docs/en/remote-assets.md). After changes, use the [regression checklist](docs/en/regression-checklist.md); see the [glossary](docs/glossary.md) for terminology.

```sh
server/build-host.sh && python3 server/tests/smoke_test.py
python3 -m unittest discover -s tools/tests -t .
python3 -m unittest discover -s server/generators/tests
```

The last command requires PyYAML. These commands check the host offline server, tools, and data generators; they do not replace a device or emulator regression.

## License and origins

The repository code is licensed under GPL-3.0; see [LICENSE](LICENSE). This project derives from [ChocoLZS/linkura-localify](https://github.com/ChocoLZS/linkura-localify) and retains its commit history. See [NOTICE.md](NOTICE.md) for origins, third-party components, and game-content rights.
