SELECT catalog.response_json, COALESCE(mastery.is_mastery, 0),
       COALESCE(score.clear_count, 0), owned.latest_play_time
FROM collection_music AS catalog
INNER JOIN museum_music_ownership AS owned USING(musics_id)
LEFT JOIN music_mastery AS mastery ON mastery.music_id = catalog.musics_id
LEFT JOIN rhythm_music_score AS score ON score.music_id = catalog.musics_id
ORDER BY catalog.musics_id;
