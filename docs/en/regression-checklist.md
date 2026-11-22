<!-- Translation source: 1a59167456efd164e163fbb74cc42e36365b3580 (docs/zh/regression-checklist.md) -->

# Regression checklist

After changing code and installing a new build, run this checklist to confirm that screens open, work or close, retain state after a restart, and do not crash. A full pass takes about 45–60 minutes. For a change limited to one module, do at least **A: baseline** and that module's checks. The steps use Android terminology but also apply to iOS (read `Documents/LinkuraOffline/server.log`).

## 0. Preparation

1. Back up the player database before touching the device:
   - Android: `adb exec-out run-as com.oddno.lovelive cat files/HasuKikaisann.sqlite3 > backup.sqlite3`
   - iOS: `xcrun devicectl device copy from ... --source "Library/Application Support/LinkuraOffline/HasuKikaisann.sqlite3"`
2. Install the new package over the existing app (Android `adb install -r`, iOS `tools/ios_device_setup.py`). Do not uninstall or clear data.
3. Enable airplane mode and confirm that offline play still works.

Each check passes when the screen behaves as expected and the logs contain no `FATAL EXCEPTION`, `Fatal signal`, or new `no mock file or registered route` (on iOS, `no route for`).

Known harmless unmatched route: `/v1/activity_record/notify_month_displayed` (notification only).

## A: Baseline (always)

| # | Action | Expected |
|---|---|---|
| A1 | Cold-start game → tap LOGIN on title | Home opens; logs show `user/login` then `home/get_home`. |
| A2 | Check diamond count at top right of Home | Matches the gacha screen (currently about 868,799). |
| A3 | Bottom menu → locking of six entries | Mission, プレゼント, ショップ, お知らせ, and フレンド open; サークル is locked. |
| A4 | Force-stop and log in again | Return to Home; A2 value is unchanged. |

## B: Home entries

| # | Action | Expected |
|---|---|---|
| B1 | Mission → switch デイリー / ウィークリー / メイン tabs → bottom-bar back | All six tabs display; the Member Fan Lv mission in メイン is CLEAR; Home menu still works after returning. |
| B1b | Skip any Daily stage twice → return to Mission | デイリー「ライブを3回クリア」 and ウィークリー「ライブを50回クリア」 each increase by 2; diamonds do not change simply from opening Mission. |
| B2 | Mission → any 「挑戦」 → ステージ → bottom-bar back | Return to Mission; tabs remain switchable and the app does **not hang**. |
| B3 | Home ↔ Mission three times | Both screens remain tappable; Gacha still opens. |
| B4 | プレゼント → 履歴 → back | Empty list; closes normally. |
| B5 | ショップ → アイテム交換所 → 通常アイテム交換 → exchange one item | Button works; product +1, material decreases; return works. |
| B6 | お知らせ → close | Closes normally. |
| B7 | フレンド → ID search dialog → back | Closes normally. |
| B8 | Tap NAME at top of Home | From the second opening onward, プロフィール opens within one second; return to Home works. |
| B9 | Tap Chapter RANK | Top bar shows SS; 「Chapter対象期間外です」 appears, with no download or error (Chapter assets were never saved, so a hook intercepts this). |
| B10 | Menu page 2: アイテム, サウンド設定 (cancel), 各種設定 (cancel), メニュー → ミュートリスト → back | Each opens and closes. |
| B10b | キャッシュ削除 / ログアウト / ユーザー削除 in title menu and 各種設定 | Only 「オフライン版では使用できません」 appears; no action runs. In-game メニュー logout/user deletion only advises returning to title. |
| B11 | コレクション → ステッカー / 四コマ漫画 / メンバー, returning after each | Lists display normally. |

## C: Gacha

| # | Action | Expected |
|---|---|---|
| C1 | Open gacha from the Home banner | Seven pools are switchable; backgrounds display. |
| C2 | Single pull → swipe the pack open → result | Animation and card art display; diamonds −200, points +1. |
| C3 | Ten-pull → skip → result | Ten cards; the number marked `is_new` equals new rows in the ownership table. |
| C4 | ガチャ履歴 | Shows the recent pulls. |
| C5 | With ≥200 points, tap 「ポイントで引き換える」 → redeem one card | Acquisition animation plays; points −200; an owned card is not marked NEW. |
| C6 | Force-restart and open gacha again | Diamond and point balances persist. |

## D: スクールアイドルステージ (スクステ)

Play at least one full match in each mode (auto mode is fine). Check: enter → performance → LIVE CLEAR/FAILED → result → return to stage.

| # | Mode and path | Additional check |
|---|---|---|
| D1 | Daily Live → any クエスト → Rank 1 → 決定 → song selection → LIVE開始 | Attempts −1; result shows drops under 「取得アイテム」. |
| D2 | Daily Live → スキップ once | Attempts −1; skip result shows drop quantities. |
| D3 | Daily Live → top-right 「+」 to restore attempts | Attempts increase. |
| D4 | Quest Live → any area → any stage → 決定 → song selection → LIVE開始 | Stage shows materials under 「獲得可能アイテム」; result has drops; プレイPt. decreases by stage cost (e.g. −10). |
| D4b | Quest stage → スキップ three times | Skip dialog opens; プレイPt. −30; each skip yields drops. |
| D5 | Learning Live → any song → I → 決定 → LIVE開始; also スキップ once | Results complete; skip result works. |
| D6 | Grade Live → Grade Quest → 103期 SPRING → ステージランク1 → 決定 → tap next map tile → LIVE開始 | Map advances, action Pt decreases; result returns to map. |
| D7 | Dream Live → any member → ドリームスタイル解放クエスト → 決定 → LIVE開始 | Result and return work; if LOVE is below the stage requirement, LIVE CLEAR is not shown. |
| D8 | Live Grand Prix → ステージ選択へ → ステージA → ランク1 → 決定 → LIVE開始 (two confirmation dialogs) | Stage shows プレイ済 and today's Pt; 「合計グランプリPt.」 and Top ranking score equal the sum of each stage's best score today. |
| D9 | During 選曲, open song list → pick song → tap cover to close | List closes and LIVE開始 appears. |

## E: Rhythm game (スクールアイドルSHOW)

| # | Action | Expected |
|---|---|---|
| E1 | Home SHOW banner → 決定 → 編成 screen → LIVE開始 → no-friend-card confirmation OK | Game starts in landscape. |
| E2 | Wait for end → LIVE CLEAR → 次へ → 選曲画面へ | Mastery increases; return to song selection. |
| E3 | Top-right Home button → OK | Return to portrait Home. |

## F: Cards and 編成

| # | Action | Expected |
|---|---|---|
| F1 | ステージ → 育成 → any card → 演出・ボイス → 入手時 animation | Video plays and finishes. |
| F2 | ステージ → 編成 → place a card in any member's SIDE slot → bottom-bar back | Leave normally; no endless Now Loading. |
| F3 | Force-restart → return to 編成 | F2 change persists. |
| F4 | 育成 → base card obtained by gacha (Lv1) → 強化 to cap | Cap follows rarity (mUR: 60); スタイルPt. decreases according to Master. |
| F5 | Same card → 特訓 → 特訓する | Animation plays; details show ○○+ and a higher cap; evolution materials decrease. |
| F6 | Same card → 解放 once; force-restart and check again | Count +1, 解放 Pt −100 (mUR); level, evolution, and unlock persist. |

## G: Stories and video (known unavailable; only test exit)

| # | Action | Expected |
|---|---|---|
| G1 | 活動記録 → chapter 1 → any episode → PART → OK → download error → ログイン画面へ → LOGIN | Home can be entered again. |
| G2 | スクールアイドルコネクト → ARCHIVES → any replay → OK | Silently returns to list, without hanging. |

## Finish

1. Recount unmatched routes and crash markers (see section 0).
2. Compare player databases before and after: ownership, diamonds, points, and 編成 should match the actions taken; `PRAGMA integrity_check` should be ok.
3. Record the results in a dated verification log with screenshot numbers, log filenames, APK SHA-256, and localify commit.
