"""Generate the offline shop launcher from preserved 5.1 Shops MasterData."""
from __future__ import annotations

import json
from pathlib import Path

import yaml


TOOLS_DIR = Path(__file__).resolve().parent
MASTER_DIR = TOOLS_DIR.parents[8] / "link-like-diff"
BUILTIN_DIR = TOOLS_DIR.parent.parent / "builtin"


def generate_json() -> None:
    rows = yaml.safe_load((MASTER_DIR / "Shops.yaml").read_text(encoding="utf-8"))
    catalog = sorted(rows, key=lambda row: row["OrderId"])
    response = {
        "shop_list": [{
            "shop_type": row["ShopType"],
            "name": row["Name"],
            "order_id": row["OrderId"],
            "is_maintenance": False,
            # Only the item exchange has a local product catalog and purchase handler.
            "is_exist_products": row["ShopType"] == 3,
        } for row in catalog],
        "is_convertible_limit_break_material": False,
    }
    path = BUILTIN_DIR / "shop_get_list.json"
    path.write_text(json.dumps(response, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"  Written -> {path}")


if __name__ == "__main__":
    generate_json()
