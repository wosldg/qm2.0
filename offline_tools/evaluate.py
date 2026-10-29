# -*- coding: utf-8 -*-
"""
离线评估：用训练好的模型在留出测试窗口上做预测，计算：
  - MAE / RMSE / MAPE（点预测）
  - 置信区间覆盖率（lower ≤ actual ≤ upper 的比例，应接近 80%）
  - 分位数校准（actual 落在 quantile 预测位置的分布）

同时对比 naive baseline（直接用过去的 current_visitors 当未来预测）。

用法：训练完成后 python evaluate.py
"""

import json
from pathlib import Path

import numpy as np
import pandas as pd
import torch

ROOT = Path(__file__).resolve().parent.parent
OFFLINE = ROOT / "offline_tools"


def load_preprocessing():
    with open(OFFLINE / "preprocessing_params.json", "r", encoding="utf-8") as f:
        return json.load(f)


def denormalize(values, norm_param):
    """把归一化的值还原回原始尺度。"""
    return values * norm_param["std"] + norm_param["mean"]


def naive_predict(df, spot_id, target_col, horizon):
    """Baseline：用过去最后一个 current_visitors 作为未来所有步的预测。"""
    sub = df[df.spot_id == spot_id].sort_values("tft_time_idx")
    last = sub[target_col].iloc[-1]
    return np.full(horizon, last)


def evaluate_point(y_true, y_pred):
    """计算 MAE / RMSE / MAPE。"""
    y_true = np.asarray(y_true, dtype=float)
    y_pred = np.asarray(y_pred, dtype=float)
    mask = y_true > 1.0  # 过滤接近 0 的点（避免 MAPE 爆炸）
    mae = float(np.mean(np.abs(y_true - y_pred)))
    rmse = float(np.sqrt(np.mean((y_true - y_pred) ** 2)))
    if mask.sum() > 0:
        mape = float(np.mean(np.abs((y_true[mask] - y_pred[mask]) / y_true[mask])) * 100)
    else:
        mape = 0.0
    return {"mae": mae, "rmse": rmse, "mape_pct": mape}


def evaluate_quantile(y_true, y_lo, y_hi):
    """置信区间覆盖率。"""
    y_true = np.asarray(y_true)
    y_lo = np.asarray(y_lo)
    y_hi = np.asarray(y_hi)
    covered = np.mean((y_lo <= y_true) & (y_true <= y_hi))
    width = np.mean(y_hi - y_lo)
    width_ratio = np.mean((y_hi - y_lo) / (np.maximum(y_true, 1.0)))
    return {
        "coverage": float(covered),
        "avg_interval_width": float(width),
        "avg_width_ratio": float(width_ratio),
    }


def main():
    # preprocessing_params.json 是 train_tft.py 的产物，训练前不存在时用 sim_config.json 的 target_norm 默认值
    config = json.load(open(OFFLINE / "sim_config.json", "r", encoding="utf-8"))

    prep_path = OFFLINE / "preprocessing_params.json"
    if prep_path.exists():
        with open(prep_path, "r", encoding="utf-8") as f:
            prep = json.load(f)
        target_norm = prep["norm_params"]["current_visitors"]
    else:
        prep = None
        target_norm = {"mean": 0.0, "std": 1.0}

    horizon = config["tft"]["prediction_horizon"]

    print("==> Loading data ...")
    df = pd.read_csv(OFFLINE / "sim_scenic_flow_train.csv")
    df = df.sort_values(["spot_id", "tft_time_idx"]).reset_index(drop=True)

    # 留出最后 10% 的时间窗口做评估
    max_idx = df["tft_time_idx"].max()
    split = int(max_idx * 0.9)
    test_df = df[df["tft_time_idx"] > split].copy()
    train_df = df[df["tft_time_idx"] <= split].copy()
    print(f"    train={len(train_df)}, test={len(test_df)}")

    # 1. Naive baseline 评估（快，不依赖模型）
    print("\n==> Naive baseline evaluation ...")
    all_true, all_pred = [], []
    for spot_id in test_df.spot_id.unique():
        sub = test_df[test_df.spot_id == spot_id].sort_values("tft_time_idx")
        hist = train_df[train_df.spot_id == spot_id].sort_values("tft_time_idx")
        if len(hist) < horizon or len(sub) < horizon:
            continue
        for i in range(len(sub) - horizon):
            y_true = sub.current_visitors.iloc[i : i + horizon].values
            y_pred = naive_predict(hist, spot_id, "current_visitors", horizon)
            all_true.append(y_true)
            all_pred.append(y_pred)

    all_true = np.concatenate(all_true)
    all_pred = np.concatenate(all_pred)
    naive_metrics = evaluate_point(all_true, all_pred)
    print(f"    Naive: {naive_metrics}")

    # 2. 模型预测评估（需要 onnxruntime 或 PyTorch）
    # 检查 onnxruntime 是否可用
    try:
        import onnxruntime as ort
        print("\n==> ONNX model evaluation ...")
        model_path = OFFLINE / "tft_model.onnx"
        if not model_path.exists():
            print("    ONNX not found, skipping model eval (run train_tft.py first)")
            model_metrics = None
        else:
            session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
            print(f"    Input: {[i.name for i in session.get_inputs()]}")
            print(f"    Output: {[o.name for o in session.get_outputs()]}")

            # ONNX 输入构造比较复杂，这里简化：
            # 用 naive baseline 作为 fallback，等模型实际导出后再接入真实推理
            print("    (ONNX inference input construction pending — using stats instead)")
            model_metrics = {
                "note": "ONNX inference TBD after successful export",
                "target_norm": target_norm,
            }
    except ImportError:
        print("\n==> onnxruntime not installed, skipping model eval")
        model_metrics = None

    # 3. 汇总
    summary = {
        "dataset": {
            "total_rows": len(df),
            "spots": int(df.spot_id.nunique()),
            "split_time_idx": split,
            "horizon_ticks": horizon,
            "horizon_minutes": horizon * config["time_interval_minutes"],
        },
        "naive_baseline": naive_metrics,
        "model": model_metrics,
        "recommended": {
            "model_target": "MAE < 5% of avg visitors",
            "coverage_target": "0.75 ~ 0.85 (80% nominal)",
            "width_ratio_target": "< 0.5",
        },
    }

    with open(OFFLINE / "eval_summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    print(f"\n==> Summary written to {OFFLINE}/eval_summary.json")
    print(f"    Naive baseline MAE = {naive_metrics['mae']:.0f}, "
          f"MAPE = {naive_metrics['mape_pct']:.1f}%")
    print(f"    Target: MAE < 5% of avg ({df.current_visitors.mean()*0.05:.0f})")


if __name__ == "__main__":
    main()
