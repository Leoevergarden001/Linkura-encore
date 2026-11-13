-- Purchase counts of the shops served from shop_catalog.json (GP PRIZE交換所
-- 'gp_prize', ステッカー交換所 'sticker', SIsCaストア 'sisca_store'), for their
-- Master LimitNum. Item Exchange keeps offline_item_exchange_purchase.
CREATE TABLE IF NOT EXISTS offline_shop_purchase (
    kind TEXT NOT NULL,
    id INTEGER NOT NULL,
    purchased_num INTEGER NOT NULL DEFAULT 0 CHECK (purchased_num >= 0),
    PRIMARY KEY (kind, id)
);
