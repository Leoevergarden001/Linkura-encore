<!-- Translation source: 85ba8e531f12739a3a402a5389f6af013d337e19 (docs/zh/server.md) -->

# オフラインサーバーの開発

## パソコン上での実行

```sh
server/build-host.sh                     # -> server/build/host/linkura-offline-server
python3 server/tests/smoke_test.py       # 一時ディレクトリで起動し、主な API を確認
```

サーバーを変更した後は、スモークテストで素早く確認できます。API の動作を示す生きた文書でもあり、各 `check(...)` がゲーム内で確認した動作に対応します。

ホスト版では iOS のリモートアセットプロキシもテストできます（`--remote-asset-base`、`--remote-asset-key` など）。[リモートアセットサーバー](remote-assets.md#パソコンでのテスト)をご覧ください。

## API の追加と変更

1. 端末のログから未登録パス（Android は `no mock file or registered route`、iOS は `no route for`）を探し、クライアントが期待するレスポンス構造を調べます。DTO のフィールド名は IL2CPP の `Org.OpenAPITools.Model.*` クラスで確認できます（`tools/re/il2cpp_re.py find`）。
2. 静的レスポンスなら JSON を `server/data/builtin/` に置き、`server/src/RouteRegistry.cpp` の `RegisterStaticJson` で登録します。
3. 状態を持つレスポンスなら `HttpMockBackend` で SQLite の読み書きを実装し、`RegisterBackend` で登録します。新しい表は `server/db/schema/NNN_*.sql` に追加し、既存のプレイヤーDBの更新処理は `server/db/player-state/` に置きます。
4. enum の値を推測しないでください。[iOS パッチ](patching-ios.md)のツールで metadata のフィールド既定値を読むか、既存コードのコメントを参照してください。
5. `server/tests/smoke_test.py` に確認項目を追加し、通過後に端末で検証します。
6. 再パッケージ化せずに試す場合、Android と iOS の両方でパス別の上書きファイルを使えます。各プラットフォームのデバッグ節を参照してください。

## データベース

プレイヤーDBは一つの SQLite ファイルです。Android ではゲームの `files/HasuKikaisann.sqlite3`、iOS では `Library/Application Support/LinkuraOffline/` にあります。初回起動時に `schema/` から表を作成して `seed/` を読み込み、全カードを所持し育成済みの「ミュージアムセーブデータ」を生成します。その後の編成、育成、ガチャ、ミッション、SHOW の成績は同じデータベースに保存します。

## 内蔵データの生成

`server/data/builtin` と `server/db/seed` の大部分は、スクリプトでマスターデータから生成します。

```sh
pip install pyyaml
git submodule update --init third_party/link-like-diff
python3 server/generators/generate_all.py
# マスターデータが別の場所にある場合：LINKURA_MASTER_DIR=<path> python3 server/generators/generate_all.py
python3 -m unittest discover -s server/generators/tests
```

- パスは `server/generators/paths.py` にまとめています。
- SQL のシードにはオンラインサーバーから取得したアーカイブ詳細（`server/generators/data/archive_archive_details.json`、リポジトリ外）も含みます。これがない場合、`generate_all.py` は JSON だけを再生成します。
- `user_card_get_list.json` のカード 1 枚（キャラクター 1053）は、取得時の `generations_id` 値 0 を保持しています。生成器なら 100 になりますが、リポジトリ内の値を優先してください。
- ガチャプール定義の `server/src/backend/MuseumGachaCatalog.hpp` は `generate_gacha_catalog.py` で生成します。`server/GACHA_MUSEUM.md` を参照してください。
- スクールアイドルコネクト：アーカイブ一覧 `archive_get_archive_list.json`（398 件）、With×STATION の一覧と詳細 `archive_get_with_station_list.json`・`archive_with_station_details.json`（5 回）、アーカイブの公式動画 URL `archive_video_urls.json`（`motionReplays` を無効にしたときに使用）。ホームの「Recent」には最新の With×STATION を表示します。
- アーカイブ詳細は、起動のたびに `server/db/player-state/008_archive_replay_urls.sql`（モーションリプレイのエントリ）と `009_mrs_video_replays.sql`（5.0.1 クライアントは mrs のモーションデータを再生できないため、With×MEETS の mrs アーカイブは動画で再生）で補正します。
