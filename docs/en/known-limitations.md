<!-- Translation source: 501289cf3b617f15507c0039ea96fbc2e157595c (docs/zh/known-limitations.md) -->

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
