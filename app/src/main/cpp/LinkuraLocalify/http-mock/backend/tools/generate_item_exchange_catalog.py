"""Embed preserved ItemExchanges Master IDs for the offline shop browser."""
from __future__ import annotations

import json
from pathlib import Path

import yaml


TOOLS_DIR = Path(__file__).resolve().parent
MASTER_DIR = TOOLS_DIR.parents[8] / "link-like-diff"
BUILTIN_DIR = TOOLS_DIR.parent.parent / "builtin"


def generate_json() -> None:
    rows = yaml.safe_load((MASTER_DIR / "ItemExchanges.yaml").read_text(encoding="utf-8"))
    catalog = [{
        "id": row["Id"],
        "category_list_id": row.get("CategoryListId", 0),
        "product_item_id": row.get("ProductItemId", 0),
        "product_item_num": row.get("ProductItemNum", 0),
        "material_item_id": row.get("MaterialItemId", 0),
        "material_item_num": row.get("MaterialItemNum", 0),
        "limit_num": row.get("LimitNum", 0),
        "unlock_conditions": row.get("UnlockConditions", 0),
    } for row in rows]
    path = BUILTIN_DIR / "item_exchange_catalog.json"
    path.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"  Written -> {path}")


if __name__ == "__main__":
    generate_json()
