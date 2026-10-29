# -*- coding: utf-8 -*-
"""探针：验证导出的 ONNX 模型对不同景点产出不同预测（确认无景点信息被烘焙成常量）。"""
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from train_tft import prepare_dataset  # noqa: E402

OFFLINE = Path(__file__).resolve().parent


def main():
    import onnxruntime as ort
    import pytorch_forecasting as PF

    config = json.load(open(OFFLINE / "sim_config.json", encoding="utf-8"))
    df = pd.read_csv(OFFLINE / "sim_scenic_flow_train.csv")
    df = df[df["tft_time_idx"] < 3 * 288].copy()
    df, spot_to_id, norm_params, kc, kcont, obs = prepare_dataset(df, config)

    def make_loader(spot):
        sub = df[df["spot_id"] == spot].copy()
        ds = PF.TimeSeriesDataSet(
            sub, time_idx="time_idx", target="current_visitors", group_ids=["series_id"],
            static_categoricals=[], max_encoder_length=96, max_prediction_length=12,
            time_varying_known_categoricals=kc, time_varying_known_reals=kcont,
            time_varying_unknown_reals=obs, target_normalizer=None,
            allow_missing_timesteps=False,
        )
        x, _ = next(iter(ds.to_dataloader(train=False, batch_size=1)))
        return x

    sess = ort.InferenceSession(str(OFFLINE / "tft_model_smoke.onnx"))
    valid = {i.name for i in sess.get_inputs()}

    def infer(x):
        feed = {k: v.numpy() for k, v in x.items() if k in valid}
        return sess.run(["prediction"], feed)[0][0, :, 1]  # q50 曲线

    out_a = infer(make_loader("spot_01"))
    out_b = infer(make_loader("spot_08"))
    print(f"spot_01 q50: {np.round(out_a, 3)}")
    print(f"spot_08 q50: {np.round(out_b, 3)}")
    diff = np.abs(out_a - out_b).max()
    print(f"max |A-B| = {diff:.6f} -> {'OK: 景点间输出有区分度' if diff > 1e-4 else 'FAIL: 输出相同，疑似烘焙'}")


if __name__ == "__main__":
    main()
