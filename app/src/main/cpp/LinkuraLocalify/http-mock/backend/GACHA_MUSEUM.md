# Offline Gacha Museum route

The 5.1.0 client calls `/v1/gacha/get_series_list` before opening the scene and
`/v1/gacha/set_purchase` after confirmation. Both routes are handled by the
local SQLite backend. `/v1/gacha/get_history` reads the same persistent draw
history. Home and Gacha response headers report the wallet from that database.

Three preserved series are enabled: 1055003 (BRIGHT SEEDS), 1055002
(FILM B.G.P.), and 1055001 (PERENNIAL). The original server probability
table has not been recovered. Museum mode chooses uniformly among a small
set of featured base CardDatas IDs for each series. The result animation
resolves the base ID (ending in 0), while Collection uses the corresponding
fully evolved record (ending in 4). Two 94 records with missing
CardGetMovieSettings use matching 95 character records. These are explicit
preservation substitutions, not official prize pools or rates.

One draw costs 200 local SIsCa; ten draws cost 2000. Wallet deduction, card
ownership, exchange points, and history commit together. The local client
request contains no operation ID, so repeated accepted requests count as
separate draws. Ticket purchases and daily paid discounts are not implemented.
The Museum bootstrap owns all catalogued cards, so these featured cards appear
as duplicates until a narrower preserved player inventory is imported.

On the author emulator, the third series' ten draw traversed SetPhase 1,
GetCardAnimation, ten distinct card displays, SetPhase 2, and Result.InitUI.
The opening movie took several minutes including touch-to-advance sections;
white or black transition frames during that sequence do not by themselves
indicate a deadlock. The wallet and exchange points updated in the result.
This validates that path, not every possible prize or asset. Remove any
`mock_api/v1/gacha/*.json` overrides before testing: file mocks take
precedence over registered backend routes.
