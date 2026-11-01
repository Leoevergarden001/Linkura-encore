# Offline Gacha Museum route

The 5.1.0 client calls `/v1/gacha/get_series_list` before opening the scene and
`/v1/gacha/set_purchase` after confirmation. Both routes are handled by the
local SQLite backend. `/v1/gacha/get_history` reads the same persistent draw
history. Home and Gacha response headers report the wallet from that database.

The only enabled series is 1055003 (`DREAM INTERLUDE COLLECTION -BRIGHT SEEDS-`).
The original server probability table has not been recovered. Museum mode
therefore chooses uniformly among five featured cards from the preserved
`GachaSeries` and `CardSeries` Master records: 19415014, 19425014, 19434014,
19315024, and 19525014. This is a local playable approximation and must not
be presented as the official rate table.

One draw costs 200 local SIsCa; ten draws cost 2000. Wallet deduction, card
ownership, exchange points, and history commit together. The local client
request contains no operation ID, so repeated accepted requests count as
separate draws. Ticket purchases and daily paid discounts are not implemented.
The Museum bootstrap owns all catalogued cards, so these featured cards appear
as duplicates until a narrower preserved player inventory is imported.

The scene and one static purchase DTO were verified on the existing author
emulator before this backend was added. The compiled backend still needs a
device smoke test of single draw, ten draw, history, restart persistence, and
wallet display. Remove any `mock_api/v1/gacha/*.json` overrides before that
test: file mocks take precedence over registered backend routes.
