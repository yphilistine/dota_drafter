"""
export_model_data.py — конвертация draft_helper_abstract_data.pkl → _data.db

Использование:
    python export_model_data.py [--pkl PATH] [--out PATH]

По умолчанию:
    --pkl  ../datafetcher/draft_helper_abstract_data.pkl  (или рядом с скриптом)
    --out  draft_helper_abstract_data.db
"""

import argparse
import os
import pickle
import sqlite3
import sys

PRIOR = 100


def smoothed_wr(wins, games, prior=PRIOR):
    return (wins + prior * 0.5) / (games + prior) if (games + prior) > 0 else 0.5


def raw_wr(wins, games):
    return wins / games if games > 0 else 0.5


def export(pkl_path: str, out_path: str):
    with open(pkl_path, "rb") as f:
        data = pickle.load(f)

    global_raw, vs_raw, with_raw = data["matchup_stats"]
    modal_pos = data["modal_pos"]
    hero_pos_raw = data["hero_pos_wr"]
    hero_map = data["hero_map"]
    pick_rates = data["pick_rates"]

    if os.path.exists(out_path):
        os.remove(out_path)

    db = sqlite3.connect(out_path)
    db.execute("PRAGMA journal_mode=WAL")

    # ── global_wr ──
    db.execute("CREATE TABLE global_wr (hero_id INTEGER PRIMARY KEY, wr REAL)")
    db.executemany(
        "INSERT INTO global_wr VALUES (?,?)",
        [(hid, smoothed_wr(w, g)) for hid, (w, g) in global_raw.items()],
    )

    # ── vs_wr (raw pairwise WR) ──
    db.execute(
        "CREATE TABLE vs_wr (hero_id INTEGER, opp_hero_id INTEGER, wr REAL, "
        "PRIMARY KEY (hero_id, opp_hero_id))"
    )
    db.executemany(
        "INSERT INTO vs_wr VALUES (?,?,?)",
        [(h, o, raw_wr(w, g)) for (h, o), (w, g) in vs_raw.items()],
    )

    # ── with_wr (raw pairwise WR) ──
    db.execute(
        "CREATE TABLE with_wr (hero_id INTEGER, ally_hero_id INTEGER, wr REAL, "
        "PRIMARY KEY (hero_id, ally_hero_id))"
    )
    db.executemany(
        "INSERT INTO with_wr VALUES (?,?,?)",
        [(h, a, raw_wr(w, g)) for (h, a), (w, g) in with_raw.items()],
    )

    # ── modal_pos ──
    db.execute("CREATE TABLE modal_pos (hero_id INTEGER PRIMARY KEY, pos INTEGER)")
    db.executemany(
        "INSERT INTO modal_pos VALUES (?,?)", list(modal_pos.items())
    )

    # ── hero_pos_wr ──
    db.execute(
        "CREATE TABLE hero_pos_wr (hero_id INTEGER, pos INTEGER, wr REAL, "
        "PRIMARY KEY (hero_id, pos))"
    )
    db.executemany(
        "INSERT INTO hero_pos_wr VALUES (?,?,?)",
        [(h, p, raw_wr(w, g)) for (h, p), (w, g) in hero_pos_raw.items()],
    )

    # ── immortalherostats (raw counts for GUI display) ──
    db.execute(
        "CREATE TABLE immortalherostats (hero_id INTEGER, pos INTEGER, "
        "games INTEGER, wins INTEGER, PRIMARY KEY (hero_id, pos))"
    )
    db.executemany(
        "INSERT INTO immortalherostats VALUES (?,?,?,?)",
        [(h, p, g, w) for (h, p), (w, g) in hero_pos_raw.items()],
    )

    # ── pick_rates (NEW) ──
    db.execute(
        "CREATE TABLE pick_rates (hero_id INTEGER PRIMARY KEY, rate REAL)"
    )
    db.executemany(
        "INSERT INTO pick_rates VALUES (?,?)", list(pick_rates.items())
    )

    db.commit()

    # ── Summary ──
    counts = {}
    for table in ["global_wr", "vs_wr", "with_wr", "modal_pos",
                   "hero_pos_wr", "immortalherostats", "pick_rates"]:
        counts[table] = db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
    db.close()

    print(f"Exported to {out_path}:")
    for t, c in counts.items():
        print(f"  {t}: {c} rows")
    print(f"File size: {os.path.getsize(out_path):,} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_pkl = os.path.join(
        os.path.dirname(script_dir), "datafetcher", "draft_helper_abstract_data.pkl"
    )
    parser.add_argument("--pkl", default=default_pkl)
    parser.add_argument(
        "--out",
        default=os.path.join(script_dir, "draft_helper_abstract_data.db"),
    )
    args = parser.parse_args()

    if not os.path.exists(args.pkl):
        print(f"ERROR: pkl not found: {args.pkl}", file=sys.stderr)
        sys.exit(1)

    export(args.pkl, args.out)
