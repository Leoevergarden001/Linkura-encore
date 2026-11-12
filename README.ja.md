[English](README.md) | [简体中文](README.zh-CN.md) | **日本語**

<!-- Translation source: 63fcabf5fea9a916d302ae6e09b78dd91a651a3f (README.zh-CN.md) -->

# linkura-offline

「Link！Like！ラブライブ！蓮ノ空スクールアイドルクラブ」（リンクラ）の**非公式**オフライン実行プロジェクトです。公式サービスが 2026 年 6 月 30 日に終了した後も、手元にあるクライアントとアセットキャッシュを使い続けられるようにし、今後の開発と修正の基盤を提供します。

> 本プロジェクトは公式とは無関係です。ゲームのインストーラー、アセット、アカウントデータの提供・配布は行いません。ご自身が適法に保有するクライアントとデータだけを使用してください。

## できること

- **オフラインサーバー**（`server/`）：端末内の `127.0.0.1` でゲームの API に応答します。進行状況はローカルの SQLite に保存します。Android と iOS で同じコードを使います。
- **Android 5.1.0**（`android/`）：LSPatch モジュールで API 呼び出しを受け持ち、オフライン時の画面遷移を修正し、再取得できないアセットを消す操作を無効にします。
- **iOS / iPadOS 5.0.1**（`ios/` + `tools/`）：オフラインサーバーを dylib として IPA に組み込み、元のバイト列を検証する静的パッチを適用します。脱獄は不要で、インストール前にご自身で署名します。
- **ツール**（`tools/`）：再パッケージ化、署名、iOS の一括インストール、Android のリソースパック作成・導入、アセットキャッシュの網羅率分析、解析補助を提供します。

ログイン、Home、SHOW（リズムゲーム）、Dream Live、ガチャ、カード育成、編成、ミッション、プロフィール、コレクションなどの主な機能をオフラインで利用できます。保存されていないストーリーボイス、歌詞 MV、アーカイブ動画などについては[既知の制限](docs/ja/known-limitations.md)をご覧ください。

## 用意するもの

| | Android | iOS / iPadOS |
|---|---|---|
| クライアント | ご自身の 5.1.0 APK | ご自身の復号済み 5.0.1 IPA |
| アセット | 端末に保存されたゲームのアセットキャッシュ（`files/D`、`files/M`）とストレージキー | 同じもの。Android のキャッシュを変換できます |
| 環境 | Android SDK/NDK、JDK 17、LSPatch 用の Java 21+ | Xcode、Apple アカウント（無料の Personal Team でも可） |
| リソースパックの準備 | 元のストレージキーを新しいものに置き換えるための SQLCipher 4 CLI（`brew install sqlcipher`） | 同じ |

## クイックスタート

```sh
git clone --recurse-submodules <this repo>
```

- [はじめに・対応バージョン](docs/ja/getting-started.md)
- [Android のビルドとインストール](docs/ja/android.md) ・ [iOS のビルドとインストール](docs/ja/ios.md) ・ [アセットの準備](docs/ja/resources.md)

## 開発

- [全体構成](docs/ja/architecture.md)
- [オフラインサーバー：API・データベース・生成ツール](docs/ja/server.md)
- [iOS パッチの調査と追加](docs/ja/patching-ios.md) ・ [Android Hook](docs/ja/hooking-android.md)
- [回帰確認チェックリスト](docs/ja/regression-checklist.md)
- [用語集](docs/glossary.md)

```sh
server/build-host.sh && python3 server/tests/smoke_test.py   # ホスト版サーバーのスモークテスト
python3 -m unittest discover -s tools/tests -t .                # ツールのテスト
python3 -m unittest discover -s server/generators/tests         # 生成ツールのテスト（PyYAML が必要）
```

## ライセンス

GPL-3.0 です（[LICENSE](LICENSE)）。本プロジェクトは [ChocoLZS/linkura-localify](https://github.com/ChocoLZS/linkura-localify) から派生し、そのコミット履歴を保持しています。出典とサードパーティ製コンポーネントは [NOTICE.md](NOTICE.md) をご覧ください。
