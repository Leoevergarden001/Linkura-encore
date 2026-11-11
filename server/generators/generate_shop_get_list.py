"""Generate the offline shop launcher from preserved 5.1 Shops MasterData."""
from __future__ import annotations

import json
from pathlib import Path

import yaml
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import BUILTIN_DIR, MASTER_DIR


TOOLS_DIR = Path(__file__).resolve().parent
BUILTIN_DIR = BUILTIN_DIR


def generate_json() -> None:
    rows = yaml.safe_load((MASTER_DIR / "Shops.yaml").read_text(encoding="utf-8"))
    # Only Item Exchange has a local product catalog and purchase transaction.
    # Omitting other shops avoids advertising grey, non-functional tiles.
    catalog = sorted(
        (row for row in rows if row["ShopType"] == 3),
        key=lambda row: row["OrderId"],
    )
    response = {
        "shop_list": [{
            "shop_type": row["ShopType"],
            "name": row["Name"],
            "order_id": row["OrderId"],
            "is_maintenance": False,
            "is_exist_products": True,
        } for row in catalog],
        "is_convertible_limit_break_material": False,
    }
    path = BUILTIN_DIR / "shop_get_list.json"
    path.write_text(json.dumps(response, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"  Written -> {path}")


if __name__ == "__main__":
    generate_json()
