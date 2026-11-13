SELECT (SELECT COUNT(*) FROM museum_card_ownership) AS card_num,
       -- プロフィール「ドリームスタイル所持数」: owned DR (rarity 8) cards.
       (SELECT COUNT(*) FROM museum_card_ownership AS owned
            INNER JOIN card_detail USING(d_card_datas_id)
            INNER JOIN card_rarity ON card_rarity.card_datas_id = card_detail.card_datas_id
        WHERE card_rarity.rarity = 8) AS dream_style_num,
       (SELECT COUNT(*) FROM museum_music_ownership) AS music_num,
       (SELECT COUNT(*) FROM collection_music) AS music_max_num,
       (SELECT COUNT(*) FROM museum_sticker_ownership) AS sticker_num,
       (SELECT COUNT(*) FROM museum_gallary_ownership) AS gallary_num,
       (SELECT COUNT(*) FROM quest_stage) AS standard_live_max_num;
