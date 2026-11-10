"""Build the archive cheer ranking (応援ランキング) catalog for the offline server.

Source: preserved Master CheerRankings / CheerRewards / CheerRewardDatas.
The server-side ranking tables (players, cheer points) were not preserved,
so the offline response shows each member's ranking period and reward tiers
with an empty ranking.
"""
from __future__ import annotations

import json
from pathlib import Path

import yaml

TOOLS_DIR = Path(__file__).resolve().parent
MASTER_DIR = TOOLS_DIR / "link-like-diff"
if not (MASTER_DIR / "CheerRankings.yaml").exists():
    MASTER_DIR = TOOLS_DIR.parents[8] / "link-like-diff"
BUILTIN_DIR = TOOLS_DIR.parent.parent / "builtin"


def _load(name: str):
    return yaml.safe_load((MASTER_DIR / name).read_text(encoding="utf-8")) or []


def _time(value) -> str:
    return value.strftime("%Y-%m-%dT%H:%M:%SZ") if hasattr(value, "strftime") else str(value)


def build() -> list[dict]:
    datas: dict[int, list[dict]] = {}
    for row in sorted(_load("CheerRewardDatas.yaml"), key=lambda r: r["Id"]):
        datas.setdefault(row["CheerRewardsId"], []).append({
            "cheer_reward_datas_id": row["Id"], "item_type": row["RewardType"],
            "item_id": row["RewardItemId"], "num": row["RewardNum"]})
    rewards: dict[int, list[dict]] = {}
    for row in sorted(_load("CheerRewards.yaml"), key=lambda r: r["MinTargetNum"]):
        rewards.setdefault(row["CheerRankingsId"], []).append({
            "cheer_rewards_id": row["Id"], "min_target_num": row["MinTargetNum"],
            "max_target_num": row["MaxTargetNum"], "reward_datas": datas.get(row["Id"], [])})
    return [
        {"id": row["Id"], "member_id": row["CheerMembersId"],
         "start_time": _time(row["StartTime"]), "end_time": _time(row["EndTime"]),
         "cheer_rewards": rewards.get(row["Id"], [])}
        for row in sorted(_load("CheerRankings.yaml"), key=lambda r: (r["CheerMembersId"], _time(r["StartTime"])))
    ]


def generate_json() -> None:
    catalog = build()
    output_path = BUILTIN_DIR / "cheer_rankings.json"
    output_path.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"  Written -> {output_path} ({len(catalog)} rankings)")


if __name__ == "__main__":
    generate_json()
