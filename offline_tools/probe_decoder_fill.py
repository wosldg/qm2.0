# -*- coding: utf-8 -*-
"""
探针：验证 TFT ONNX 模型输出对 decoder_cont 中 unknown 列填充值的敏感度。

背景：
  predict 模式下 pytorch_forecasting 会把 df 中的真实未来值填进 decoder_cont
  的 unknown 列（current_visitors/arrival_rate/departure_rate/utilization）。
  C++ 实时推理时没有未来真实值，必须确定一个填充策略。

对比三种填充下 onnx 输出（prediction [1,12,3]）的差异：
  A. 原始（真实未来值，Python 侧 predict 数据集的行为）
  B. 全 0
  C. encoder 最后一个值延续（forward-fill）

若 B/C 与 A 完全一致（或差异在 1e-6 量级），说明 TFT 推理路径不使用
decoder unknown reals，C++ 侧直接填 0 即可。

用法: python probe_decoder_fill.py [--model tft_model_smoke.onnx]
"""

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
OFFLINE = ROOT / "offline_tools"

# decoder_cont 列顺序（训练脚本 prepare_dataset 定义）：
#   [season_factor(known), current_visitors, arrival_rate, departure_rate, utilization]
KNOWN_CONT_COUNT = 1  # season_factor 是 known，保持真实值；其余 4 列为 unknown


def build_predict_sample(days=3):
    """复刻 train_tft.py 的数据准备流程，取一条 predict 模式样本（batch=1）。"""
    import pytorch_forecasting as PF
    from train_tft import prepare_dataset

    with open(OFFLINE / "sim_config.json", "r", encoding="utf-8") as f:
        config = json.load(f)

    df = pd.read_csv(OFFLINE / "sim_scenic_flow_train.csv")
    ticks_per_day = 24 * 60 // config["time_interval_minutes"]
    df = df[df["tft_time_idx"] < days * ticks_per_day].copy()

    df, _, _, known_cat, known_cont, observed = prepare_dataset(df, config)

    training = PF.TimeSeriesDataSet(
        df,
        time_idx="time_idx",
        target="current_visitors",
        group_ids=["series_id"],
        static_categoricals=[],
        max_encoder_length=config["tft"]["input_length"],
        max_prediction_length=config["tft"]["prediction_horizon"],
        time_varying_known_categoricals=known_cat,
        time_varying_known_reals=known_cont,
        time_varying_unknown_categoricals=[],
        time_varying_unknown_reals=observed,
        target_normalizer=None,
        allow_missing_timesteps=False,
    )
    predict_ds = PF.TimeSeriesDataSet.from_dataset(
        training, df, predict=True, stop_randomization=True
    )
    x, _ = next(iter(predict_ds.to_dataloader(train=False, batch_size=1)))
    return {k: v.numpy() for k, v in x.items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="tft_model_smoke.onnx")
    args = parser.parse_args()

    import onnxruntime as ort

    model_path = OFFLINE / args.model
    print(f"==> model: {model_path.name}")
    sample = build_predict_sample()

    sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    valid_names = {i.name for i in sess.get_inputs()}

    dec = sample["decoder_cont"].copy()
    enc = sample["encoder_cont"]
    print(f"    decoder_cont shape: {dec.shape}, encoder_cont shape: {enc.shape}")
    print(f"    decoder unknown 列(原始, 前3步):\n{dec[0, :3, KNOWN_CONT_COUNT:]}")

    def run(feed_dict):
        feed = {k: v for k, v in feed_dict.items() if k in valid_names}
        return sess.run(["prediction"], feed)[0]

    # A: 原始（真实未来值）
    out_a = run(sample)

    # B: unknown 列全 0
    feed_b = dict(sample)
    dec_b = dec.copy()
    dec_b[:, :, KNOWN_CONT_COUNT:] = 0.0
    feed_b["decoder_cont"] = dec_b
    out_b = run(feed_b)

    # C: forward-fill（encoder 最后值延续）
    feed_c = dict(sample)
    dec_c = dec.copy()
    last = enc[:, -1:, KNOWN_CONT_COUNT:]  # [1, 1, 4]
    dec_c[:, :, KNOWN_CONT_COUNT:] = np.repeat(last, dec_c.shape[1], axis=1)
    feed_c["decoder_cont"] = dec_c
    out_c = run(feed_c)

    diff_ab = np.abs(out_a - out_b).max()
    diff_ac = np.abs(out_a - out_c).max()
    print(f"\n==> max|A(真实) - B(全0)|        = {diff_ab:.10f}")
    print(f"==> max|A(真实) - C(forward-fill)| = {diff_ac:.10f}")

    tol = 1e-5
    if diff_ab < tol and diff_ac < tol:
        print("\n结论: 输出对 decoder unknown 列填充值不敏感（TFT 推理不使用它们）。")
        print("      C++ 侧直接填 0 即可。")
    else:
        print(f"\n结论: 输出对填充值敏感（超过 {tol}）！需进一步分析哪一列有影响。")
        # 逐列定位敏感列
        for i in range(KNOWN_CONT_COUNT, dec.shape[2]):
            feed_i = dict(sample)
            dec_i = dec.copy()
            dec_i[:, :, i] = 0.0
            d = np.abs(out_a - run(feed_i)).max()
            col = ["season_factor", "current_visitors", "arrival_rate",
                   "departure_rate", "utilization"][i]
            print(f"    单列置零 {col:<18} max diff = {d:.10f}")


if __name__ == "__main__":
    main()
