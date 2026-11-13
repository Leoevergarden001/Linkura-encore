-- Executed inside the caller's transaction, once per save.
-- The Museum inventory seed also holds many items at 999999, the holding cap
-- (Items.LimitNum) of materials such as pieces and ユメノカケラ. The client
-- disables an exchange whose product is at its cap (GetMaxCount needs
-- LimitNum - held >= ProductItemNum), so lower those holdings to 99999 once,
-- as 002 did for the 9999 cap. SIsCa (1001001 paid, 1002001 free) mirror the
-- gacha wallet and are left alone.
UPDATE museum_item_inventory SET item_num = 99999
WHERE item_num >= 999999
  AND d_item_datas_id NOT IN (SELECT d_item_datas_id FROM item WHERE item_id IN (1001001, 1002001))
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'INVENTORY_TOP_CAP_HEADROOM');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('INVENTORY_TOP_CAP_HEADROOM');
