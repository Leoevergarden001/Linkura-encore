SELECT catalog.response_json
FROM collection_gallary AS catalog
INNER JOIN museum_gallary_ownership AS owned USING(comics_id)
ORDER BY catalog.comics_id;
