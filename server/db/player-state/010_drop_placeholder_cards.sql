-- Executed inside the caller's transaction, on every start (idempotent).
-- Master card series 1010400 (Rarity 4) and 1010500 (Rarity 5) are named "？？？"
-- and have no card art. 001 grants every series, so the card list and every
-- picker built from it (friend cards, decks, both スクステ and スクショウ tabs)
-- showed two blank cards. get_custom_setting already leaves them out of the
-- Home wallpaper pieces (generate_custom_setting.py). Drop the ownership here;
-- the catalog rows in card_detail stay, so ids of other cards do not move.

-- Same definition as HttpMockBackend creates on demand (follow/set_friend_card).
CREATE TABLE IF NOT EXISTS offline_friend_card (card_type INTEGER NOT NULL PRIMARY KEY, d_card_datas_id TEXT NOT NULL);

CREATE TEMP TABLE IF NOT EXISTS placeholder_cards (d_card_datas_id TEXT NOT NULL PRIMARY KEY);
DELETE FROM placeholder_cards;
INSERT INTO placeholder_cards(d_card_datas_id)
SELECT o.d_card_datas_id FROM museum_card_ownership AS o
INNER JOIN card_detail AS c ON c.d_card_datas_id = o.d_card_datas_id
WHERE c.card_datas_id / 10 IN (1010400, 1010500)
  -- A card a deck or a running quest still names stays owned rather than
  -- leaving a dangling id behind; the player can swap it out and restart.
  AND NOT EXISTS(SELECT 1 FROM deck
                 WHERE deck.ace_card = o.d_card_datas_id
                    OR instr(deck.deck_cards_json, o.d_card_datas_id) > 0)
  AND NOT EXISTS(SELECT 1 FROM rhythm_game_deck
                 WHERE instr(rhythm_game_deck.deck_card_list_json, o.d_card_datas_id) > 0)
  AND NOT EXISTS(SELECT 1 FROM quest_runtime_session
                 WHERE instr(quest_runtime_session.start_payload_json, o.d_card_datas_id) > 0
                    OR instr(coalesce(quest_runtime_session.finish_payload_json, ''), o.d_card_datas_id) > 0);

-- An empty id is how the friend card slot stores "not set".
UPDATE offline_friend_card SET d_card_datas_id = ''
WHERE d_card_datas_id IN (SELECT d_card_datas_id FROM placeholder_cards);

DELETE FROM museum_card_state WHERE d_card_datas_id IN (SELECT d_card_datas_id FROM placeholder_cards);
DELETE FROM offline_card_growth WHERE d_card_datas_id IN (SELECT d_card_datas_id FROM placeholder_cards);
DELETE FROM museum_card_ownership WHERE d_card_datas_id IN (SELECT d_card_datas_id FROM placeholder_cards);
DELETE FROM placeholder_cards;
