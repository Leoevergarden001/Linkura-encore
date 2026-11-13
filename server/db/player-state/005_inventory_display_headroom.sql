-- Executed inside the caller's transaction, once per save.
-- Exchange confirm popups print the material holding next to the item name in
-- a field sized for about three digits; five-digit holdings (99999 after 004,
-- the membership exchange tickets' seed) run into the name. Lower holdings
-- above 999 to 999 once, as 002 did for the 9999 cap. Point currencies spent
-- in large amounts keep their balance: SIsCa (1001001 paid, 1002001 free,
-- mirrored from the gacha wallet), Grade Pt. (1005000-1005099), スタイルPt.
-- (3004000) and ペタルコイン (3007001). ステッカー交換Pt. is an exchange
-- material and is lowered too.
UPDATE museum_item_inventory SET item_num = 999
WHERE item_num > 999
  AND d_item_datas_id NOT IN (
      SELECT d_item_datas_id FROM item
      WHERE item_id IN (1001001, 1002001, 3004000, 3007001)
         OR item_id BETWEEN 1005000 AND 1005099)
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'INVENTORY_DISPLAY_HEADROOM');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('INVENTORY_DISPLAY_HEADROOM');
