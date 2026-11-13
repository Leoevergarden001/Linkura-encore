-- Executed inside the caller's transaction, once per save.
-- GP PRIZE交換チケット (3011001) pays 200-500 per card in GP PRIZE交換所, and
-- 002/005 left it at 999. Raise it once to its holding cap (Items.LimitNum
-- 9999); four digits fit the exchange screens. Higher holdings are kept.
UPDATE museum_item_inventory SET item_num = 9999
WHERE item_num < 9999
  AND d_item_datas_id = (SELECT d_item_datas_id FROM item WHERE item_id = 3011001 LIMIT 1)
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'GP_PRIZE_TICKET_TOPUP');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('GP_PRIZE_TICKET_TOPUP');
