"""Generate the offline shop launcher from preserved Shops MasterData."""
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
    # Shops with a local catalog and purchase transaction: Item Exchange (3),
    # GP PRIZE (7), select tickets (8), stickers (9) and the SIsCa store (10).
    # GachaExchange (4) lives on the Gacha screen. The server sets
    # is_convertible_limit_break_material per player.
    catalog = sorted(
        (row for row in rows if row["ShopType"] in (3, 7, 8, 9, 10)),
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
