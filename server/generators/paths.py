"""Locations used by the generators.

Master data (the YAML tables of the game's MasterData) is read from
``third_party/link-like-diff`` (git submodule) or from the directory named by
the ``LINKURA_MASTER_DIR`` environment variable.  Generated files go to
``server/data/builtin`` (JSON responses) and ``server/db`` (SQL), which the
Android and iOS builds embed.
"""
from __future__ import annotations

import os
from pathlib import Path

GENERATORS_DIR = Path(__file__).resolve().parent
SERVER_DIR = GENERATORS_DIR.parent
REPO_DIR = SERVER_DIR.parent
BUILTIN_DIR = SERVER_DIR / "data" / "builtin"
DB_DIR = SERVER_DIR / "db"
MASTER_DIR = Path(os.environ.get("LINKURA_MASTER_DIR") or REPO_DIR / "third_party" / "link-like-diff")
