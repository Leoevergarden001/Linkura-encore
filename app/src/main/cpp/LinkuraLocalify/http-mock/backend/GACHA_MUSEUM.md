# Offline Gacha Museum route

The 5.1.0 client calls `/v1/gacha/get_series_list` before opening the scene and
`/v1/gacha/set_purchase` after confirmation. Both routes are handled by the
local SQLite backend. `/v1/gacha/get_history` reads the same persistent draw
history. Home and Gacha response headers report the wallet from that database.

Seven preserved series are enabled: 1055003 (BRIGHT SEEDS), 1055002
(FILM B.G.P.), 1055001 (PERENNIAL), and four graduation series (1050511,
1050509, 1050506, 1050505). The original server probability
table has not been recovered. The offline candidate catalog now includes
every non-placeholder, non-limited R/SR/UR base card present before all three
series started in the preserved R2606000 Master snapshot: 46 R, 42 SR and
73 UR. Each series also includes its four or five featured mUR cards, for
165 or 166 distinct candidates per series. This is a reproducible Museum
catalog, not proof of the retired server's exact per-series prize list.
`tools/generate_gacha_catalog.py` regenerates `MuseumGachaCatalog.hpp` from
`link-like-diff`; `--check` verifies that the checked-in catalog matches.

Until the original odds are recovered, Museum mode uses an explicit provisional
80% R / 17% SR / 3% UR tier split. Half of the UR tier chooses one of the
series' featured mUR cards uniformly; the other half chooses a standard UR
uniformly. There is no claim that these are official odds, and ten-draw
guarantees are not yet modeled. The result animation resolves the base ID
(ending in 0), while Collection uses the corresponding fully evolved record
(ending in 4). Two featured 94 records with missing CardGetMovieSettings use
matching 95 character records. These are explicit preservation substitutions.

One draw costs 200 local SIsCa; ten draws cost 2000. Wallet deduction, card
ownership, exchange points, and history commit together. The local client
request contains no operation ID, so repeated accepted requests count as
separate draws. Ticket purchases and daily paid discounts are not implemented.
The Museum bootstrap owns all catalogued cards, so these featured cards appear
as duplicates until a narrower preserved player inventory is imported.

On the author emulator, the second and third series' ten draws traversed
SetPhase 1, GetCardAnimation, ten card displays, SetPhase 2, and Result.InitUI.
The opening movie took several minutes including touch-to-advance sections;
white or black transition frames during that sequence do not by themselves
indicate a deadlock. The wallet and exchange points updated in the result.
This validates that path, not every possible prize or asset. Remove any
`mock_api/v1/gacha/*.json` overrides before testing: file mocks take
precedence over registered backend routes.

After the catalog expansion, emulator-5562 loaded the Gacha page with all
161 standard base-card DTOs present. A single draw returned standard SR
10214200, showed its own illustration, and reached Result.InitUI; a second
draw returned standard R 10423030, showed its own illustration, and also
reached Result.InitUI. The local database was backed up before installation.
These two draws do not establish that every possible card asset is cached.

The preserved ExchangePointRate Master records supply 25 card offers across
these series. `/v1/gacha/get_guarantee_point_list` builds the exchange list;
`/v1/gacha/set_guarantee_point_exchange` validates the selected rate and
atomically debits points, records the exchange, and grants the card. On
emulator-5562, a 200-point exchange of rate 10503195 returned the correct
card result with its own artwork. SQLite integrity remained `ok`; series
1055003 points changed from 290 to 90 and one exchange history row was added.
The earlier unexpected-error dialog was from an installed list-probe APK whose
submit route returned HTTP 501. The complete exchange APK no longer does so.

`museum_gacha_acquisition` distinguishes the first local draw/exchange from
the Museum bootstrap, so a first acquisition can set `is_new=true`. The
client displayed the `NEW` label in the exchange result. This does not yet
prove that every advanced card voice or acquisition movie plays: those need
per-card asset and client playback verification. The client displays a
numeric remaining-exchange count even for Master offers with no finite limit;
the current capped display value can overlap the nearby Detail button.

## Animation safety

`GachaSeries.card_list` now contains the complete Museum candidate set rather
than only locally owned cards. The old value was inconsistent for a new draw.
It is still a hypothesis that this field drives asset preloading: the client
has not been traced reading it for that purpose, and an emulator retest of the
specific failed animation has not yet been completed. Do not treat this change
alone as proof that the `CardAnimationPlayer` fault is fixed.

The preserved data still does not contain every historical animation
dependency. Card `10215210` is temporarily quarantined from future Museum
draws after a reproduced `Tecotec.CardAnimationPlayer.PlayAsync`
`NullReferenceException` on emulator-5562. The failing draw also contained
nine other cards, so its exact cause has not yet been isolated. It is not
deleted from ownership or Collection; it will be re-enabled after its asset
chain and playback are checked. This is deliberately a narrow runtime
safety exclusion, rather than silently treating the entire 73-card Master
catalog as animation-verified.

`tools/audit_gacha_assets.py` reports every current standard and featured
candidate, the Master rows needed by its animation, and the number/size of
files in the preserved external-resource archive. The 2026-10-25 full archive
contains 35,952 regular `D/` files totalling 17,460,984,601 bytes. Current
pool definitions reference 161 standard and 26 distinct featured cards;
none lacks the checked Master records. All 187 still require per-card asset
mapping and playback verification; the report marks `10215210` as an observed
failure. Opaque `D/` names cannot establish which specific cards have art,
movie, or voice available. Recover the client's catalog mapping first, then
verify those three asset classes and expand only with observed successful
playback.
