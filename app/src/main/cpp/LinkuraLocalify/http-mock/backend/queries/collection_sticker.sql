SELECT catalog.response_json, owned.stickers_id IS NOT NULL,
       COALESCE(owned.requirement_progress, 0),
       COALESCE(owned.variant_requirement_progress, 0),
       COALESCE(owned.is_available_variant, 0)
FROM collection_sticker AS catalog
LEFT JOIN museum_sticker_ownership AS owned USING(stickers_id)
ORDER BY catalog.stickers_id;
