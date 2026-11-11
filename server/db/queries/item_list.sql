SELECT item.d_item_datas_id, item.item_id, item.item_type, item.item_category,
       item.rarity, inventory.item_num, item.resource_file_name
FROM item INNER JOIN museum_item_inventory AS inventory USING(d_item_datas_id)
ORDER BY item.item_category, item.item_id;
