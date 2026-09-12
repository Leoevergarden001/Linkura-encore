SELECT item.response_json, inventory.item_num
FROM item INNER JOIN museum_item_inventory AS inventory USING(d_item_datas_id)
WHERE item.d_item_datas_id = ?;
