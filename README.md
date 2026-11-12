**English** | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

<!-- Translation source: 32d73002987861dc6785db22505959c69dcba5f3 (README.zh-CN.md) -->

# linkura-offline

An **unofficial** offline project for Link! Like! Love Live! (リンクラ). After the official service ended on June 30, 2026, it lets you continue using a client and asset cache you already have, and provides a base for further fixes and development.

> This project is not affiliated with the official game. It does not provide or distribute game installers, assets, or account data. Use only a client and data that you lawfully possess.

## Features

- **Offline server** (`server/`): answers the game's APIs on the device at `127.0.0.1`. Player progress is stored in local SQLite. Android and iOS share the same server code.
- **Android 5.1.0** (`android/`): an LSPatch module that intercepts API calls, fixes offline scene transitions, and disables actions that could erase irreplaceable assets.
- **iOS / iPadOS 5.0.1** (`ios/` + `tools/`): bundles the offline server as a dylib into an IPA and applies byte-checked static patches. No jailbreak is required; sign the result yourself before installing.
- **Tools** (`tools/`): packaging, signing, one-step iOS installation, Android resource pack creation and import, asset cache coverage analysis, and reverse-engineering helpers.

Login, Home, SHOW (リズムゲーム), Dream Live, gacha, card development, 編成, missions, profile, and collection are usable offline. Content that was never cached, such as story voices, lyric MVs, and archive videos, is covered under [known limitations](docs/en/known-limitations.md).

## Requirements

| | Android | iOS / iPadOS |
|---|---|---|
| Client | Your own 5.1.0 APK | Your own decrypted 5.0.1 IPA |
| Assets | The game's asset cache from your device (`files/D`, `files/M`) and its storage key | The same; an Android cache can be converted |
| Environment | Android SDK/NDK, JDK 17, Java 21+ for LSPatch | Xcode and an Apple account (a free Personal Team works) |
| Resource pack preparation | SQLCipher 4 CLI (`brew install sqlcipher`) to replace source storage keys with fresh ones | Same |

## Quick start

```sh
git clone --recurse-submodules <this repo>
```

- [Getting started and supported versions](docs/en/getting-started.md)
- [Build and install on Android](docs/en/android.md) · [Build and install on iOS](docs/en/ios.md) · [Prepare assets](docs/en/resources.md)

## Development

- [Architecture](docs/en/architecture.md)
- [Offline server: routes, databases, and generators](docs/en/server.md)
- [Finding and adding iOS patches](docs/en/patching-ios.md) · [Android hooks](docs/en/hooking-android.md)
- [Regression checklist](docs/en/regression-checklist.md)
- [Terminology](docs/glossary.md)

```sh
server/build-host.sh && python3 server/tests/smoke_test.py   # host-server smoke test
python3 -m unittest discover -s tools/tests -t .                # tool tests
python3 -m unittest discover -s server/generators/tests         # generator tests (requires PyYAML)
```

## License

GPL-3.0; see [LICENSE](LICENSE). This project derives from [ChocoLZS/linkura-localify](https://github.com/ChocoLZS/linkura-localify) and retains its commit history. See [NOTICE.md](NOTICE.md) for origins and third-party components.
