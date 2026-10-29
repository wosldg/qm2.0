# -*- coding: utf-8 -*-
"""
P0-1 验收数据准备：把仿真训练集中的一段连续历史灌进后端 DB 的 flow_record。

做法：
  - 从 sim_scenic_flow_train.csv 取每个景点连续 108 个 tick（96 历史 + 12 真值）
  - 时间整体平移，使最后一个 tick 对齐"当前时间"的 5 分钟边界
    （模型只消费 month/dayofweek/is_weekend 类别与归一化连续值，绝对时间不影响
     C++ vs Python 数值一致性验收；flow_record 保留 7 天，不用真实时间会被清理）
  - 清空 flow_record 后写入（该表本来就是可再生的仿真数据）

用法: python seed_predict_history.py [--start-tick 20000]
"""

import argparse
import sqlite3
from datetime import datetime, timedelta
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
OFFLINE = ROOT / "offline_tools"
DB = ROOT / "backend" / "data" / "scenicflow.db"

HISTORY = 96
HORIZON = 12
NEED = HISTORY + HORIZON


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--start-tick", type=int, default=20000,
                        help="仿真数据中截取窗口的起始 tft_time_idx")
    args = parser.parse_args()

    df = pd.read_csv(OFFLINE / "sim_scenic_flow_train.csv",
                     usecols=["spot_id", "record_time", "current_visitors", "arrival_rate",
                              "departure_rate", "tft_time_idx"])
    df = df.rename(columns={"record_time": "ts"})
    win = df[(df.tft_time_idx >= args.start_tick) & (df.tft_time_idx < args.start_tick + NEED)]
    spots = sorted(win.spot_id.unique())
    assert len(spots) == 8, f"窗口内景点数不对: {spots}"

    # 时间平移：窗口最后 tick -> 当前时间向下取整到 5 分钟
    now = datetime.now()
    aligned = now.replace(second=0, microsecond=0)
    aligned = aligned - timedelta(minutes=aligned.minute % 5)
    last_ts = pd.to_datetime(win.ts.max())
    offset = aligned - last_ts
    win = win.copy()
    win["ts_shifted"] = (pd.to_datetime(win.ts) + offset).dt.strftime("%Y-%m-%d %H:%M:%S")
    print(f"==> window tft_time_idx [{args.start_tick}, {args.start_tick + NEED}) "
          f"原始 {win.ts.min()} ~ {win.ts.max()}")
    print(f"    平移后 {win.ts_shifted.min()} ~ {win.ts_shifted.max()}")

    conn = sqlite3.connect(DB)
    cur = conn.cursor()
    cur.execute("DELETE FROM flow_record")
    rows = [
        (r.ts_shifted, r.spot_id, int(r.current_visitors), int(r.current_visitors),
         "NORMAL", float(r.arrival_rate), float(r.departure_rate), 0)
        for r in win.itertuples()
    ]
    cur.executemany(
        "INSERT INTO flow_record(record_time, spot_id, current_visitors, cumulative_visitors,"
        " congestion_level, arrival_rate, departure_rate, is_deleted)"
        " VALUES(?,?,?,?,?,?,?,?)", rows)
    conn.commit()
    n = cur.execute("SELECT COUNT(*) FROM flow_record").fetchone()[0]
    conn.close()
    print(f"==> flow_record 已重置为 {n} 行（8 景点 × {NEED} tick）")
    print(f"    同步 scenic_spot.current_visitors 快照...")

    # 让页面/规则引擎看到的快照与 flow_record 最新值一致
    conn = sqlite3.connect(DB)
    latest = win[win.tft_time_idx == args.start_tick + NEED - 1]
    for r in latest.itertuples():
        conn.execute("UPDATE scenic_spot SET current_visitors=?, arrival_rate=?, "
                     "departure_rate=? WHERE spot_id=?",
                     (int(r.current_visitors), float(r.arrival_rate),
                      float(r.departure_rate), r.spot_id))
    conn.commit()
    conn.close()
    print("    done")


if __name__ == "__main__":
    main()
