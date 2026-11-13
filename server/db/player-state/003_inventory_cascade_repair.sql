-- Executed inside the caller's transaction, once per save.
-- A re-seed with foreign keys on (2026-11-13 upgrades, before the seed ran
-- with the checks off) replaced every item row and its ON DELETE CASCADE
-- emptied museum_item_inventory. Restore the rows that went missing with the
-- Museum starting values (capped 9999 holdings at 999, as 002 did) and the
-- SIsCa items from the gacha wallet, which was not affected. Rows that exist
-- are player state and are left alone; changes made since bootstrap are lost.
INSERT OR IGNORE INTO museum_item_inventory(d_item_datas_id, item_num)
SELECT d_item_datas_id, CASE WHEN item_num = 9999 THEN 999 ELSE MAX(item_num, 0) END FROM item
WHERE EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'ITEM')
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'INVENTORY_CASCADE_REPAIR');
UPDATE museum_item_inventory
SET item_num = (SELECT paid_jewels FROM museum_gacha_state WHERE series_id = 1055003)
WHERE d_item_datas_id = (SELECT d_item_datas_id FROM item WHERE item_id = 1001001 LIMIT 1)
  AND EXISTS(SELECT 1 FROM museum_gacha_state WHERE series_id = 1055003)
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'INVENTORY_CASCADE_REPAIR');
UPDATE museum_item_inventory
SET item_num = (SELECT free_jewels FROM museum_gacha_state WHERE series_id = 1055003)
WHERE d_item_datas_id = (SELECT d_item_datas_id FROM item WHERE item_id = 1002001 LIMIT 1)
  AND EXISTS(SELECT 1 FROM museum_gacha_state WHERE series_id = 1055003)
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'INVENTORY_CASCADE_REPAIR');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('INVENTORY_CASCADE_REPAIR');
