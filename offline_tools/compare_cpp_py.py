# -*- coding: utf-8 -*-
"""对比 C++ 端点结果与 Python 侧基准（P0-1 验收：相对误差 < 1%）。"""
import json
import sys
from pathlib import Path

OFF = Path(__file__).resolve().parent


def main(spot):
    cpp = json.load(open(OFF / f"predict_cpp_result_{spot}.json", encoding="utf-8"))["data"]
    py = json.load(open(OFF / f"predict_python_result_{spot}.json", encoding="utf-8"))
    if cpp["baseTime"] != py["baseTime"]:
        print(f"FAIL: baseTime 不一致 cpp={cpp['baseTime']} py={py['baseTime']}")
        return 1
    worst = 0.0
    for a, b in zip(cpp["ticks"], py["ticks"]):
        if a["time"] != b["time"]:
            print(f"FAIL: tick 时间不一致 {a['time']} vs {b['time']}")
            return 1
        for q in ("q10", "q50", "q90"):
            rel = abs(a[q] - b[q]) / max(abs(b[q]), 1e-6)
            worst = max(worst, rel)
            if rel >= 0.01:
                print(f"  超差 {a['time']} {q}: cpp={a[q]:.4f} py={b[q]:.4f} rel={rel:.6%}")
    verdict = "PASS (<1%)" if worst < 0.01 else "FAIL"
    print(f"{spot}: max 相对误差 = {worst:.6%}  -> {verdict}")
    return 0 if worst < 0.01 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "spot_01"))
