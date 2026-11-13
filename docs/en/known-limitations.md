<!-- Translation source: ee6bfb636aa01ac79083723f3be1b8d535bf5fa0 (docs/zh/known-limitations.md) -->

# Known limitations

The offline edition depends on assets saved before shutdown. Missing assets cannot be recovered. In one fairly complete cache, about 40,500 of the final catalog's 54,503 files are usable; roughly 13.7 GB are missing, mostly story and video content. Check your own cache with `tools/resources/hailstorm.py coverage`.

| Area | Current behavior | Reason |
|---|---|---|
| 活動記録 story playback | The confirmation closes without playback | Story voices and most 3D scenes and motions were not saved. Script text (`story_main_*.txt`) is usually complete and can be inspected with `hailstorm.py decrypt`. |
| SHOW lyric MV | Option forced off (iOS packaging option `--disable-lyric-videos`) | Most MV videos were not saved. |
| Chapter RANK | Top bar shows SS; tapping displays 「Chapter対象期間外です」 | Chapter page assets were never saved; the last Chapter ended June 30, 2026 according to master data. |
| With×STATION and Fes×Live archives | Empty lists | Videos were not saved. |
| お知らせ | Blank | Notices came from the server and were not saved. |
| キャッシュ削除, ログアウト, ユーザー削除 | No effect | Offline, these would erase assets or local identity that cannot be downloaded again. |
| SIsCa dialog number | Last digit is clipped | Width limit in the original UI. |
| プレイPt. | Maximum 999 | The top bar has room for only three digits; the client rejects larger response values. |
| SHOW | A score of 0 still displays LIVE CLEAR; some character skills have no voice | Original client behavior: no failure state, skill voices do not overlap, and cards with a 「除外」 skill trigger only once per run. |
| Display | Game default resolution and font | Rendering settings and font replacement from the original linkura-localify were removed. |
| Player ID | Fixed at 000000000 | Offline play uses one local save. The resource pack includes only the catalog and master databases with fresh storage keys, without the original account's ID or keys. |
| Grade | Starts at Grade 1 with 0 Pt; only Grade Quest rewards add Grade Pt. Season Grade and its rank stay 0 | Live Grand Prix pays Grade Pt as ranking rewards, and Grade Challenge and Season Grade count over periods that have ended; offline there are no other players or periods |
| Live Grand Prix | The 69 Grand Prix of the Master run in order, each for its original number of days (days change at 04:00 JST). Pt = score × stage rank bonus × card bonus (rarity and limit break, averaged over the deck). Ranks are simulated from Pt, and rewards follow the original rank ranges when a run ends. Circle battles use the personal rules; the character bonus is not applied; first-clear and other stage rewards are unavailable; the SHOW Grand Prix is not implemented | The original Pt formula ran on the server and was not preserved; ranks depend on other players; season fan levels and the stage reward tables were not preserved |
| Shops | アイテム交換所, GP PRIZE交換所, セレクトチケット交換所 (tickets are exchanged for cards on the Gacha screen), ステッカー交換所, SIsCaストア (items bought with SIsCa) and 解放Pt.変換 work; buying SIsCa does not. A card that is already held turns into its limit-break material. Item holdings above 999 (except SIsCa, Grade Pt., スタイルPt. and ペタルコイン) are lowered to 999 once. 解放Pt.変換 converts only the materials left over after a card's last limit break, up to 9999 of each ティア | Buying SIsCa needs App Store purchases; exchange popups leave room for three-digit holdings; ティア hold at most 9999 (Items.LimitNum) |
| Login bonus | The first home of each day shows the regular login bonus (定常ログインボーナス, a 7-day loop: 50 SIsCa on days 1-6, 100 on day 7); event login bonuses are not shown | The original login bonus response was not preserved and is rebuilt from the client's data model; event login bonuses ran in fixed periods that offline play has no counterpart for |
