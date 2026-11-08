-- Executed inside the caller's transaction, once per save.
-- The Museum inventory seed copies item counts of 9999 for many items, which is
-- the client's holding cap: the original client then disables every exchange
-- whose product is already at the cap (all "交換" buttons grey). Lower capped
-- holdings to 999 once so exchanges remain usable. Later counts are player
-- state and are never reset by restarts.
UPDATE museum_item_inventory SET item_num = 999
WHERE item_num = 9999
  AND NOT EXISTS(SELECT 1 FROM museum_bootstrap_state WHERE domain = 'INVENTORY_HEADROOM');
INSERT OR IGNORE INTO museum_bootstrap_state(domain) VALUES('INVENTORY_HEADROOM');
