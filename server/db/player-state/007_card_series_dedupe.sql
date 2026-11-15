-- Executed inside the caller's transaction, once per save.
-- The live service kept one card per series: a card drawn again became
-- limit-break material. Saves could still hold two records of one card: the
-- fully developed Museum record and the base record (…0, or …1 for BR) that
-- EnsureMuseumGachaCardsLocked adds so draws can name animation-compatible
-- cards. 001 used to grant those base records too, and draws/exchanges tested
-- "held" on the exact base record, so the card list showed a Lv1 copy next to
-- the developed card. Keep each series' highest owned record, point decks,
-- the friend card and a running quest at it, then drop the lower records.
CREATE TEMP TABLE IF NOT EXISTS card_series_dedupe (
    n INTEGER PRIMARY KEY,
    dup TEXT NOT NULL UNIQUE,
    keep TEXT NOT NULL
);
DELETE FROM card_series_dedupe;
INSERT INTO card_series_dedupe(dup, keep)
SELECT o.d_card_datas_id,
       (SELECT o2.d_card_datas_id FROM museum_card_ownership AS o2
        INNER JOIN card_detail AS c2 ON c2.d_card_datas_id = o2.d_card_datas_id
        WHERE c2.card_datas_id / 10 = c.card_datas_id / 10
        ORDER BY c2.card_datas_id DESC LIMIT 1)
FROM museum_card_ownership AS o
INNER JOIN card_detail AS c ON c.d_card_datas_id = o.d_card_datas_id
WHERE NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'CARD_SERIES_DEDUPE')
  AND EXISTS(SELECT 1 FROM museum_card_ownership AS o2
             INNER JOIN card_detail AS c2 ON c2.d_card_datas_id = o2.d_card_datas_id
             WHERE c2.card_datas_id / 10 = c.card_datas_id / 10
               AND c2.card_datas_id > c.card_datas_id);

-- Card ids are fixed-length UUID strings, so a plain text replacement of each
-- mapped id rewrites every reference in a JSON column.
UPDATE deck SET
    ace_card = coalesce((SELECT keep FROM card_series_dedupe WHERE dup = deck.ace_card), ace_card),
    deck_cards_json = (
        WITH RECURSIVE step(i, v) AS (
            SELECT 0, deck.deck_cards_json
            UNION ALL
            SELECT i + 1, replace(v, m.dup, m.keep) FROM step INNER JOIN card_series_dedupe AS m ON m.n = i + 1
        )
        SELECT v FROM step ORDER BY i DESC LIMIT 1)
WHERE EXISTS(SELECT 1 FROM card_series_dedupe AS m
             WHERE instr(deck.deck_cards_json, m.dup) > 0 OR deck.ace_card = m.dup);

UPDATE rhythm_game_deck SET deck_card_list_json = (
    WITH RECURSIVE step(i, v) AS (
        SELECT 0, rhythm_game_deck.deck_card_list_json
        UNION ALL
        SELECT i + 1, replace(v, m.dup, m.keep) FROM step INNER JOIN card_series_dedupe AS m ON m.n = i + 1
    )
    SELECT v FROM step ORDER BY i DESC LIMIT 1)
WHERE EXISTS(SELECT 1 FROM card_series_dedupe AS m
             WHERE instr(rhythm_game_deck.deck_card_list_json, m.dup) > 0);

-- Same definition as HttpMockBackend creates on demand (follow/set_friend_card).
CREATE TABLE IF NOT EXISTS offline_friend_card (card_type INTEGER NOT NULL PRIMARY KEY, d_card_datas_id TEXT NOT NULL);
UPDATE offline_friend_card
SET d_card_datas_id = (SELECT keep FROM card_series_dedupe WHERE dup = offline_friend_card.d_card_datas_id)
WHERE d_card_datas_id IN (SELECT dup FROM card_series_dedupe);

UPDATE quest_runtime_session SET
    start_payload_json = (
        WITH RECURSIVE step(i, v) AS (
            SELECT 0, quest_runtime_session.start_payload_json
            UNION ALL
            SELECT i + 1, replace(v, m.dup, m.keep) FROM step INNER JOIN card_series_dedupe AS m ON m.n = i + 1
        )
        SELECT v FROM step ORDER BY i DESC LIMIT 1),
    finish_payload_json = (
        WITH RECURSIVE step(i, v) AS (
            SELECT 0, quest_runtime_session.finish_payload_json
            UNION ALL
            SELECT i + 1, replace(v, m.dup, m.keep) FROM step INNER JOIN card_series_dedupe AS m ON m.n = i + 1
        )
        SELECT v FROM step ORDER BY i DESC LIMIT 1)
WHERE EXISTS(SELECT 1 FROM card_series_dedupe AS m
             WHERE instr(quest_runtime_session.start_payload_json, m.dup) > 0
                OR instr(coalesce(quest_runtime_session.finish_payload_json, ''), m.dup) > 0);

DELETE FROM museum_card_state WHERE d_card_datas_id IN (SELECT dup FROM card_series_dedupe);
DELETE FROM offline_card_growth WHERE d_card_datas_id IN (SELECT dup FROM card_series_dedupe);
DELETE FROM museum_card_ownership WHERE d_card_datas_id IN (SELECT dup FROM card_series_dedupe);
DELETE FROM card_series_dedupe;
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('CARD_SERIES_DEDUPE');
