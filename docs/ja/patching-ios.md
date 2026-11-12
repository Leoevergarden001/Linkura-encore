<!-- Translation source: 501289cf3b617f15507c0039ea96fbc2e157595c (docs/zh/patching-ios.md) -->

# iOS パッチ：調査と追加

iOS では実行時の Hook を使えないため、パッケージ化時に UnityFramework を静的に変更します。`tools/linkura_offline/ios_offline.py` のパッチ表は各項目を次の形式で定義します。

```python
(アドレス, "説明", "元のバイト列（リトルエンディアン hex）", "置換後のバイト列")
```

`patch_code` は元のバイト列を確認し、一致しなければエラーで停止します。検証済みの 5.0.1 ビルドだけに適用され、別バージョンを壊しません。

## 準備

```sh
# シンボル表：アドレス -> メソッド名（IPA の metadata と UnityFramework から取得）
python3 -m tools.linkura_offline ios-symbols --ipa <IPA> --output sym.json
unzip -j <IPA> 'Payload/*.app/Frameworks/UnityFramework.framework/UnityFramework' -d .
alias re='python3 tools/re/il2cpp_re.py -f UnityFramework -s sym.json'
```

## パッチ位置の調査（Chapter RANK の例）

1. **名前で入口を探します**：`re find 'ChapterPoint::OnPointerClick'`。async メソッドの本体はコンパイラーが作るステートマシンにあります：`re find '<OnPointerClick>d__6::MoveNext'`。
2. **逆アセンブルします**：`re dis <アドレス> 0x600`。呼び出し先が `-> Tecotec.CommonApiCache::get_CurrentChapterRank` のように表示され、その後に `cbz w0, <通知分岐>` が続きます。
3. **他の呼び出し元を調べます**：`re callers 'CommonApiCache::get_CurrentChapterRank$'`。この getter は上部パネルでも使うので、getter 自体ではなく呼び出し箇所の分岐を変えます。
4. **元のバイト列を読みます**：`re bytes 0x2eb0fe8` の結果は `00070034`（`cbz w0, +0xe0`）です。
5. **新しい命令を書きます**：無条件の `b +0xe0` は `0x14000000 | (0xe0 >> 2)` = `0x14000038` で、リトルエンディアンでは `38000014` です。
6. **パッチ表に加えます**：独立したグループ、関数、`stage_offline_ios_client` のスイッチ、CLI の `--allow-*` オプションを用意します。`tools/tests/test_ios_offline.py` には元と新しい命令の種類と分岐先のテストを追加します。
7. パッケージ化・インストールして端末で確認します。`server.log` で元のリクエストが消えたことも確認します。

## よく使う AArch64 命令

| 目的 | 書き方 |
|---|---|
| void 関数から即座に戻る | `ret` = `c0035fd6` |
| 何もしない | `nop` = `1f2003d5` |
| 無条件分岐 | `b off` = `0x14000000 \| ((off >> 2) & 0x3FFFFFF)` |
| 条件分岐先の算出 | `cbz/cbnz`：`imm19 = (word >> 5) & 0x7FFFF`、分岐先 = アドレス + `imm19 << 2` |

## 既存のパッチグループ

| グループ | 内容 | 無効化 |
|---|---|---|
| `COMPAT_PATCHES` | 5.1 アセットと 5.0.1 クライアントの互換性（Dream Live のメンバーパネル検索） | 無効化不可 |
| `LYRIC_VIDEO_PATCHES` | SHOW の歌詞 MV をオフ扱いにします（MV アセット未保存） | 既定ではオフ。`--disable-lyric-videos` で有効化 |
| `DESTRUCTIVE_ACTION_PATCHES` | キャッシュ削除、ログアウト、ユーザー削除など八つの入口から即座に戻ります | `--allow-destructive-actions` |
| `STORY_PLAYBACK_PATCHES` | ストーリー再生確認の OK でダイアログを閉じます | `--allow-story-playback` |
| `TITLE_ID_PATCHES` | タイトル画面のプレイヤー ID を隠します | — |
| `CHAPTER_PORTAL_PATCHES` | Chapter RANK タップ時に対象期間外の通知を出します | `--allow-chapter-portal` |
| `RES_VERSION_PATCHES` | 個別のプレイヤーストレージを読まず、キャッシュのアセットバージョン（`--res-version`）を返します | `--per-player-res-version` |
| `CATALOG_SYNC_PATCHES` | リソースカタログをダウンロードせず、キャッシュのカタログを保持します | `--allow-catalog-sync` |
| `PLAYER_ID_PATCHES` | Android と同じくプレイヤー ID を 000000000 に固定し、元のアカウントの PlayerPrefs を不要にします | `--keep-player-id` |

最後の三つのグループにより、iOS はカタログDBのストレージキーだけで動きます。文字列を返す必要がある場合は、実行時に使われない文字列定数（Mono BTLS のエラーメッセージ）に必要な値を書き、メソッドの metadata 初期化がそのスロットを参照するようにします。

metadata の文字列（API アドレス、BGM 名など）の変更も同じファイルの `rewrite_literals` と `rewrite_default_values` で行います。オフセットを動かさず、文字列をその場で短くします。
