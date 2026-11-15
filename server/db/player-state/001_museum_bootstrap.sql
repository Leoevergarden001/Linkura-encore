-- Executed inside the caller's transaction, after the immutable catalogs exist.
-- Each domain is initialized once. Restarting must not restore removed ownership
-- or overwrite an imported player's inventory/progression with Museum defaults.
-- One record per card series (card id / 10), the fully developed one: the
-- lower base records EnsureMuseumGachaCardsLocked adds are only for draws.
INSERT OR IGNORE INTO museum_card_ownership(d_card_datas_id, acquired_at, acquisition_source)
SELECT d_card_datas_id, '2023-04-15T00:00:00Z', 'MUSEUM' FROM card_detail AS card
WHERE NOT EXISTS(SELECT 1 FROM card_detail AS higher
                 WHERE higher.card_datas_id / 10 = card.card_datas_id / 10
                   AND higher.card_datas_id > card.card_datas_id)
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'CARD');
INSERT OR IGNORE INTO museum_card_state(d_card_datas_id, style_level, limit_break_times, is_evolve_max)
SELECT card.d_card_datas_id, card.style_level, card.limit_break_times, card.is_evolve_max
FROM card_detail AS card INNER JOIN museum_card_ownership AS owned USING(d_card_datas_id)
WHERE NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'CARD');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('CARD');

INSERT OR IGNORE INTO museum_item_inventory(d_item_datas_id, item_num)
SELECT d_item_datas_id, MAX(item_num, 0) FROM item
WHERE NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'ITEM');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('ITEM');

INSERT OR IGNORE INTO museum_music_ownership(musics_id)
SELECT musics_id FROM collection_music
WHERE NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'MUSIC');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('MUSIC');

INSERT OR IGNORE INTO museum_sticker_ownership(stickers_id, requirement_progress,
                                            variant_requirement_progress, is_available_variant)
SELECT stickers_id, COALESCE(json_extract(response_json, '$.requirement_progress'), 0),
       COALESCE(json_extract(response_json, '$.variant_requirement_progress'), 0),
       COALESCE(json_extract(response_json, '$.is_available_variant'), 0)
FROM collection_sticker
WHERE COALESCE(json_extract(response_json, '$.is_owned'), 0) = 1
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'STICKER');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('STICKER');

INSERT OR IGNORE INTO museum_gallary_ownership(comics_id)
SELECT comics_id FROM collection_gallary
WHERE NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'GALLARY');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('GALLARY');
