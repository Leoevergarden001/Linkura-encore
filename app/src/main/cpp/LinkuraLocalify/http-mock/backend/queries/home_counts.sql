SELECT (SELECT COUNT(*) FROM museum_card_ownership) AS card_num,
       (SELECT COUNT(*) FROM museum_music_ownership) AS music_num,
       (SELECT COUNT(*) FROM collection_music) AS music_max_num,
       (SELECT COUNT(*) FROM museum_sticker_ownership) AS sticker_num,
       (SELECT COUNT(*) FROM museum_gallary_ownership) AS gallary_num,
       (SELECT COUNT(*) FROM quest_stage) AS standard_live_max_num;
