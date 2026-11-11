UPDATE museum_music_ownership
SET latest_play_time = strftime('%Y-%m-%dT%H:%M:%SZ', 'now')
WHERE musics_id = ?;
