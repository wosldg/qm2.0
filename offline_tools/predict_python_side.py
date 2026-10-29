# -*- coding: utf-8 -*-
"""
P0-1 验收（Python 侧基准）：独立实现与 C++ Predictor 相同的特征组装契约，
从后端 DB 读同样的 96 tick 历史，onnxruntime 推理，输出 12×3 分位数。

与 C++ 侧的一致性要求（验收标准：同输入误差 < 1%）：
  - encoder_cat: [month, dayofweek, is_weekend]，pandas 约定（Mon=0..Sun=6），
    编码查 categorical_maps，未见过的类别回退 0（与 C++ 一致）
  - encoder_cont: [season_factor, current_visitors, arrival_rate, departure_rate,
    utilization]，z-score 归一化
  - decoder_cat: 未来 12 tick 的真实时间类别；decoder_cont: season_factor 真实值，
    其余 4 列填 0
  - target_scale = [0, 1]；lengths = [96] / [12]
  - 输出反归一化: v * std + mean（current_visitors 的统计量）

用法: python predict_python_side.py --spot spot_01 [--model config 下正式模型]
输出: 终端打印 + predict_python_result_{spot}.json
"""

import argparse
import json
import sqlite3
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
OFFLINE = ROOT / "offline_tools"
DB = ROOT / "backend" / "data" / "scenicflow.db"
CONFIG = ROOT / "backend" / "config"

ENC_LEN, DEC_LEN, TICK_MIN = 96, 12, 5
CONT_COLS = ["season_factor", "current_visitors", "arrival_rate", "departure_rate", "utilization"]


def encode_cat(cat_maps, column, value):
    return cat_maps.get(column, {}).get(str(value), 0)  # 未见类别回退 0，同 C++


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spot", default="spot_01")
    parser.add_argument("--model", default=str(CONFIG / "tft_model.onnx"))
    parser.add_argument("--prep", default=str(CONFIG / "preprocessing_params.json"))
    parser.add_argument("--sim", default=str(CONFIG / "sim_config.json"))
    args = parser.parse_args()

    import onnxruntime as ort

    prep = json.load(open(args.prep, encoding="utf-8"))
    sim = json.load(open(args.sim, encoding="utf-8"))
    norm = prep["norm_params"]
    cat_maps = prep["categorical_maps"]
    season = {int(k): v for k, v in sim["season_factors"].items()}

    conn = sqlite3.connect(DB)
    rows = conn.execute(
        "SELECT record_time, current_visitors, arrival_rate, departure_rate "
        "FROM flow_record WHERE spot_id=? AND is_deleted=0 "
        "ORDER BY record_time DESC, id DESC LIMIT ?", (args.spot, ENC_LEN)).fetchall()
    cap = conn.execute(
        "SELECT instant_capacity FROM scenic_spot WHERE spot_id=?", (args.spot,)).fetchone()
    conn.close()
    assert len(rows) == ENC_LEN, f"历史不足: {len(rows)}"
    rows = rows[::-1]
    capacity = max(1.0, float(cap[0]))

    def z(col, v):
        return (v - norm[col]["mean"]) / norm[col]["std"]

    hist_t = [datetime.strptime(r[0], "%Y-%m-%d %H:%M:%S") for r in rows]
    enc_cat, enc_cont = [], []
    for t, r in zip(hist_t, rows):
        dow = t.weekday()  # Mon=0..Sun=6，pandas 一致
        enc_cat.append([encode_cat(cat_maps, "month", t.month),
                        encode_cat(cat_maps, "dayofweek", dow),
                        encode_cat(cat_maps, "is_weekend", int(dow >= 5))])
        cur = float(r[1])
        enc_cont.append([z("season_factor", season.get(t.month, 1.0)),
                         z("current_visitors", cur),
                         z("arrival_rate", float(r[2])),
                         z("departure_rate", float(r[3])),
                         z("utilization", cur / capacity)])

    dec_cat, dec_cont, tick_times = [], [], []
    for j in range(1, DEC_LEN + 1):
        t = hist_t[-1] + timedelta(minutes=TICK_MIN * j)
        dow = t.weekday()
        dec_cat.append([encode_cat(cat_maps, "month", t.month),
                        encode_cat(cat_maps, "dayofweek", dow),
                        encode_cat(cat_maps, "is_weekend", int(dow >= 5))])
        dec_cont.append([z("season_factor", season.get(t.month, 1.0)), 0.0, 0.0, 0.0, 0.0])
        tick_times.append(t.strftime("%Y-%m-%d %H:%M:%S"))

    feed = {
        "encoder_cat": np.array([enc_cat], dtype=np.int64),
        "encoder_cont": np.array([enc_cont], dtype=np.float32),
        "encoder_lengths": np.array([ENC_LEN], dtype=np.int64),
        "decoder_cat": np.array([dec_cat], dtype=np.int64),
        "decoder_cont": np.array([dec_cont], dtype=np.float32),
        "decoder_lengths": np.array([DEC_LEN], dtype=np.int64),
        "target_scale": np.array([[0.0, 1.0]], dtype=np.float32),
    }

    sess = ort.InferenceSession(args.model, providers=["CPUExecutionProvider"])
    valid = {i.name for i in sess.get_inputs()}
    out = sess.run(["prediction"], {k: v for k, v in feed.items() if k in valid})[0][0]

    dm, ds = prep["denormalize_target"]["mean"], prep["denormalize_target"]["std"]
    result = {
        "spotId": args.spot,
        "baseTime": rows[-1][0],
        "ticks": [{"time": tick_times[j],
                   "q10": float(out[j, 0] * ds + dm),
                   "q50": float(out[j, 1] * ds + dm),
                   "q90": float(out[j, 2] * ds + dm)} for j in range(DEC_LEN)],
    }
    out_path = OFFLINE / f"predict_python_result_{args.spot}.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)

    print(f"==> {args.spot} base={result['baseTime']}")
    for t in result["ticks"]:
        print(f"    {t['time']}  q10={t['q10']:8.1f}  q50={t['q50']:8.1f}  q90={t['q90']:8.1f}")
    print(f"==> saved: {out_path}")


if __name__ == "__main__":
    main()
