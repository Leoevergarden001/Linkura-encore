"""Embed the Master rows behind the offline shops other than Item Exchange.

shop_catalog.json carries, from the preserved MasterData:

* gp_prize        GpPrizeExchanges (GP PRIZE交換所)
* sticker         StickerExchanges (ステッカー交換所)
* sisca_store     SIsCaStoreDatas (SIsCaストア, items bought with SIsCa)
* select_ticket_series / select_ticket_rates
                  SelectTicketSeries / SelectTicketExchangeRate
                  (セレクトチケット交換所 and the Gacha screen's ticket exchange)
* limit_break_convert   LimitBreakMaterialConvertRate (解放Pt.変換)
* limit_break_material  LimitBreakMaterialRate (card series -> material item,
                  the material a duplicate card turns into)

Times are Unix seconds so the server can compare them with its clock.
"""
from __future__ import annotations

import datetime as dt
import json
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import BUILTIN_DIR, MASTER_DIR


def _rows(name: str) -> list[dict]:
    return yaml.safe_load((MASTER_DIR / name).read_text(encoding="utf-8")) or []


def _unix(value) -> int:
    if isinstance(value, dt.datetime):
        moment = value
    else:
        moment = dt.datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=dt.timezone.utc)
    return int(moment.timestamp())


def build_catalog() -> dict:
    item_limits = {row["Id"]: row["LimitNum"] for row in _rows("Items.yaml")}
    return {
        "gp_prize": [{
            "id": row["Id"],
            "product_item_type": row["ProductItemType"],
            "product_item_id": row["ProductItemId"],
            "product_item_num": row["ProductItemNum"],
            "material_item_id": row["MaterialItemId"],
            "material_item_num": row["MaterialItemNum"],
            "limit_num": row["LimitNum"],
            "reset_type": row["ResetType"],
            "start": _unix(row["StartTime"]),
            "end": _unix(row["EndTime"]),
        } for row in sorted(_rows("GpPrizeExchanges.yaml"), key=lambda r: (r["OrderId"], r["Id"]))],
        "sticker": [{
            "id": row["Id"],
            "product_sticker_id": row["ProductStickerId"],
            "material_item_id": row["MaterialItemId"],
            "material_item_num": row["MaterialItemNum"],
            "limit_num": row["LimitNum"],
            "start": _unix(row["StartTime"]),
            "end": _unix(row["EndTime"]),
        } for row in sorted(_rows("StickerExchanges.yaml"), key=lambda r: (r["OrderId"], r["Id"]))],
        "sisca_store": [{
            "id": row["Id"],
            "item_id": row["ItemId"],
            "item_num": row["ItemNum"],
            "price": row["Price"],
            "paid_only": bool(row["IsPaidSIsCaOnly"]),
            "limit_num": row["LimitNum"],
            "reset_type": row["ResetType"],
            "start": _unix(row["StartTime"]),
            "end": _unix(row["EndTime"]),
        } for row in sorted(_rows("SIsCaStoreDatas.yaml"), key=lambda r: (r["OrderId"], r["Id"]))],
        "select_ticket_series": [{
            "id": row["Id"],
            "ticket_item_id": row["ExchangeTicketId"],
            "start": _unix(row["StartTime"]),
            "end": _unix(row["EndTime"]),
        } for row in sorted(_rows("SelectTicketSeries.yaml"), key=lambda r: (r["OrderId"], r["Id"]))],
        "select_ticket_rates": [{
            "id": row["Id"],
            "series_id": row["SelectTicketSeriesId"],
            "item_type": row["ExchangeItemType"],
            "item_id": row["ExchangeItemId"],
            "quantity": row["ExchangeItemQuantity"],
        } for row in _rows("SelectTicketExchangeRate.yaml")],
        "limit_break_convert": [{
            "rarity": row["LimitBreakMaterialRarity"],
            "num": row["LimitBreakMaterialNum"],
            "product_item_id": row["ProductItemId"],
            "product_item_num": row["ProductItemNum"],
            # Items.LimitNum: a conversion stops at the product's holding cap.
            "product_limit": item_limits.get(row["ProductItemId"], 0),
        } for row in sorted(_rows("LimitBreakMaterialConvertRate.yaml"), key=lambda r: r["Id"])
            if row["ProductItemType"] == 1],
        "limit_break_material": [{
            "series_id": row["CardSeriesId"],
            "material_item_id": row["LimitBreakMaterialId"],
            "quantity": row["LimitBreakMaterialQuantity"],
        } for row in _rows("LimitBreakMaterialRate.yaml")],
    }


def generate_json() -> None:
    path = BUILTIN_DIR / "shop_catalog.json"
    path.write_text(json.dumps(build_catalog(), ensure_ascii=False, separators=(",", ":")) + "\n",
                    encoding="utf-8")
    print(f"  Written -> {path}")


if __name__ == "__main__":
    generate_json()
