# -*- coding: utf-8 -*-
"""
训练 TFT (Temporal Fusion Transformer) 时序预测模型，导出 ONNX。

流程：
  1. 加载 sim_scenic_flow_train.csv（连续 5 分钟粒度仿真数据）
  2. 做归一化（保存 preprocessing_params.json，C++ 推理时复用）
  3. 构造 TFT 训练数据集（input_length=96，prediction_horizon=12）
  4. 训练 TFT，输出 quantiles [0.1, 0.5, 0.9]（点预测 + 置信区间）
  5. 导出 onnxruntime 可推理的 tft_model.onnx
  6. 保存 preprocessing_params.json（归一化统计量、TFT 配置）

输出:
  - tft_model.onnx               : ONNX 模型（C++ 侧 onnxruntime 加载）
  - preprocessing_params.json    : 归一化统计量 + TFT 配置
  - training_summary.json        : 训练指标（val MAE / RMSE / MAPE）
  - tensorboard/                 : 训练日志（可选）
"""

import json
import warnings
from pathlib import Path

import numpy as np
import pandas as pd
import torch

warnings.filterwarnings("ignore")

ROOT = Path(__file__).resolve().parent.parent
OFFLINE = ROOT / "offline_tools"


def prepare_dataset(df: pd.DataFrame, config: dict):
    """
    把 sim_scenic_flow_train.csv 转成 TFT 要求的多变量时序格式。

    TFT 输入特征分三类：
      - known_categorical  : 预先已知的类别（month, dayofweek, is_weekend）
      - known_continuous   : 预先已知的连续量（season_factor）
      - observed           : 历史观测值 + 目标（current_visitors, arrival_rate, departure_rate）

    目标：current_visitors 未来 12 个 tick（60 分钟）
    """
    # 按 spot 分组、按时间排序
    df = df.sort_values(["spot_id", "tft_time_idx"]).reset_index(drop=True)

    # numeric_id 列：TFT 要求每个 time series 有一个数值 id
    spot_ids = df["spot_id"].unique().tolist()
    spot_to_id = {s: i for i, s in enumerate(spot_ids)}
    df["series_id"] = df["spot_id"].map(spot_to_id).astype(int)

    # time_idx：单调递增的全局时间索引
    df["time_idx"] = df.groupby("spot_id").cumcount()

    known_categorical = ["month", "dayofweek", "is_weekend"]
    known_continuous = ["season_factor"]
    observed = ["current_visitors", "arrival_rate", "departure_rate", "utilization"]

    # 归一化 observed（训练前保存 mean/std 到 preprocessing_params.json）
    norm_params = {}
    for col in observed:
        m = df[col].mean()
        s = df[col].std()
        if s < 1e-8:
            s = 1.0
        df[col] = (df[col] - m) / s
        norm_params[col] = {"mean": float(m), "std": float(s)}

    # 把 season_factor 也归一化（虽然它是 known 变量但范围不大）
    for col in known_continuous:
        m = df[col].mean()
        s = df[col].std()
        if s < 1e-8:
            s = 1.0
        df[col] = (df[col] - m) / s
        norm_params[col] = {"mean": float(m), "std": float(s)}

    # categorical 列：先转 string 再转 category（TFT 要求 categorified string）
    for col in known_categorical:
        df[col] = df[col].astype(str).astype("category")

    df["series_id"] = df["series_id"].astype(str).astype("category")

    return df, spot_to_id, norm_params, known_categorical, known_continuous, observed


def build_model(df: pd.DataFrame, config: dict, known_categorical, known_continuous, observed):
    """构建 TFT 模型 + DataModule。"""
    import pytorch_forecasting as PF
    from pytorch_forecasting.metrics import QuantileLoss

    pred_horizon = config["tft"]["prediction_horizon"]
    input_len = config["tft"]["input_length"]

    # TFT 数据集：用 TimeSeriesDataSet 自动生成训练/验证窗口
    max_pred_idx = df["time_idx"].max()
    val_size = int(max_pred_idx * 0.1)  # 最后 10% 时间窗口做验证

    training = PF.TimeSeriesDataSet(
        df[df["time_idx"] <= max_pred_idx - val_size],
        time_idx="time_idx",
        target="current_visitors",
        group_ids=["series_id"],
        static_categoricals=[],  # 不做景点 embedding：trace 导出时会被常量折叠成单一景点
        max_encoder_length=input_len,
        max_prediction_length=pred_horizon,
        time_varying_known_categoricals=known_categorical,
        time_varying_known_reals=known_continuous,
        time_varying_unknown_categoricals=[],
        time_varying_unknown_reals=observed,
        target_normalizer=None,  # 我们已手动归一化
        allow_missing_timesteps=False,
    )

    validation = PF.TimeSeriesDataSet.from_dataset(
        training, df, predict=True, stop_randomization=True
    )

    train_loader = training.to_dataloader(train=True, batch_size=64, num_workers=0)
    val_loader = validation.to_dataloader(train=False, batch_size=256, num_workers=0)

    model = PF.TemporalFusionTransformer.from_dataset(
        training,
        learning_rate=1e-3,
        hidden_size=64,
        attention_head_size=2,
        dropout=0.1,
        hidden_continuous_size=32,
        output_size=3,  # quantiles
        loss=QuantileLoss(quantiles=[0.1, 0.5, 0.9]),  # 显式3个分位数，与output_size匹配
        log_interval=10,
        reduce_on_plateau_patience=4,
    )

    return model, train_loader, val_loader, training, validation


def train_model(model, train_loader, val_loader, max_epochs=15):
    """用 PyTorch Lightning Trainer 训练。"""
    import lightning.pytorch as pl

    trainer = pl.Trainer(
        max_epochs=max_epochs,
        accelerator="cpu",
        devices=1,
        enable_progress_bar=True,
        enable_model_summary=True,
        log_every_n_steps=50,
    )
    trainer.fit(model, train_dataloaders=train_loader, val_dataloaders=val_loader)
    return trainer, model


def export_onnx(model, training_dataset, out_path: Path):
    """
    把 TFT 模型导出成 onnxruntime 可加载的格式。

    pytorch_forecasting 的 TFT forward 签名是 forward(x: dict) -> dict，
    需要包一层 wrapper：
      - 输入：dict（trace 时展平为多个命名输入）
      - 输出：只取 prediction 张量 (batch, decoder_len, 3)，3 = quantiles
    """
    import onnx

    class OnnxWrapper(torch.nn.Module):
        """forward(*tensors)：按 keys 顺序接收展平张量，内部重建 dict 喂给 TFT。"""

        def __init__(self, inner, keys):
            super().__init__()
            self.inner = inner
            self.keys = list(keys)

        def forward(self, *args):
            x = dict(zip(self.keys, args))
            return self.inner(x)["prediction"]

    model.eval()

    # 取一条样本做 dummy input（batch=1：C++ 侧逐景点逐条推理，固定 batch 更简单）
    x, _ = next(iter(training_dataset.to_dataloader(train=False, batch_size=1)))
    input_names = list(x.keys())
    wrapper = OnnxWrapper(model, input_names)

    # 先确认 wrapper 输出形状
    with torch.no_grad():
        probe = wrapper(*x.values())
    print(f"    wrapper output shape: {tuple(probe.shape)}  (batch, decoder_len, quantiles)")
    print(f"    inputs: {input_names}")

    try:
        torch.onnx.export(
            wrapper,
            tuple(x.values()),
            str(out_path),
            input_names=input_names,
            output_names=["prediction"],
            opset_version=17,
            dynamo=False,  # 走经典 trace 导出器，兼容 dict 重建
        )
        # 验证导出的 onnx 能加载
        onnx.checker.check_model(str(out_path))
        print(f"==> ONNX model exported and verified: {out_path}")

        # 再用 onnxruntime 跑一遍，与 PyTorch 输出对比。
        # 注意：trace 会剪掉推理时未使用的输入（如 decoder_target），
        # 只能 feed 图中实际存在的输入名，这份清单同时供 C++ 侧使用。
        import onnxruntime as ort
        sess = ort.InferenceSession(str(out_path), providers=["CPUExecutionProvider"])
        sess_inputs = {i.name: i for i in sess.get_inputs()}
        print(f"    onnx actual inputs: {sorted(sess_inputs.keys())}")
        feed = {k: v.numpy() for k, v in x.items() if k in sess_inputs}
        ort_out = sess.run(["prediction"], feed)[0]
        diff = np.abs(ort_out - probe.numpy()).max()
        print(f"    onnxruntime vs pytorch max abs diff: {diff:.6f}")
        return diff < 0.01
    except Exception as e:
        print(f"!! ONNX export failed: {e}")
        return False


def main():
    import argparse

    parser = argparse.ArgumentParser(description="Train TFT on simulated scenic flow")
    parser.add_argument("--days", type=int, default=0,
                        help="只取前 N 天数据做快速试跑（0 表示全量）")
    parser.add_argument("--epochs", type=int, default=15, help="训练轮数")
    parser.add_argument("--tag", type=str, default="",
                        help="输出文件名后缀，如 quick → tft_model_quick.onnx")
    args = parser.parse_args()

    rng_seed = 42
    torch.manual_seed(rng_seed)
    np.random.seed(rng_seed)

    with open(OFFLINE / "sim_config.json", "r", encoding="utf-8") as f:
        config = json.load(f)

    print("==> Loading simulation dataset ...")
    df = pd.read_csv(OFFLINE / "sim_scenic_flow_train.csv")
    print(f"    {df.shape}, spots={df.spot_id.nunique()}, "
          f"time_idx={df.tft_time_idx.min()} ~ {df.tft_time_idx.max()}")

    ticks_per_day = 24 * 60 // config["time_interval_minutes"]
    if args.days > 0:
        limit = args.days * ticks_per_day
        df = df[df["tft_time_idx"] < limit].copy()
        print(f"    [quick mode] keep first {args.days} days "
              f"(tft_time_idx < {limit}) → {df.shape}")

    print("==> Preparing dataset & normalizing ...")
    df, spot_to_id, norm_params, known_cat, known_cont, observed = prepare_dataset(df, config)

    # 归一化参数保存
    prep = {
        "metadata": {
            "generated_by": "train_tft.py",
            "note": "C++ 侧推理时必须加载此文件，用相同的 mean/std 做归一化",
        },
        "spot_to_id": spot_to_id,
        "norm_params": norm_params,
        "tft_config": config["tft"],
        "column_names": {
            "known_categorical": known_cat,
            "known_continuous": known_cont,
            "observed": observed,
            "target": "current_visitors",
        },
        "denormalize_target": norm_params["current_visitors"],  # 预测值还原用
    }
    suffix = f"_{args.tag}" if args.tag else ""
    prep_path = OFFLINE / f"preprocessing_params{suffix}.json"
    with open(prep_path, "w", encoding="utf-8") as f:
        json.dump(prep, f, ensure_ascii=False, indent=2)
    print(f"    Saved {prep_path.name}")

    # 类别编码表：C++ 侧必须把 month/dayofweek/is_weekend 映射成相同的整数 id。
    # prepare_dataset 里 df[col] 已是 pandas category，cat.categories 顺序即 codes，
    # PF 对 category 输入直接沿用其 codes（冒烟环节会打印比对验证）。
    cat_maps = {col: {str(c): int(i) for i, c in enumerate(df[col].cat.categories)}
                for col in known_cat}
    prep["categorical_maps"] = cat_maps
    with open(prep_path, "w", encoding="utf-8") as f:
        json.dump(prep, f, ensure_ascii=False, indent=2)
    print(f"    categorical_maps: { {k: len(v) for k, v in cat_maps.items()} }")

    print("==> Building TFT model ...")
    model, train_loader, val_loader, training_ds, validation_ds = build_model(
        df, config, known_cat, known_cont, observed
    )
    print(f"    {sum(p.numel() for p in model.parameters())/1e6:.2f}M params")

    # 验证：dataloader 的 encoder_cat 与 df 的 cat.codes 一致
    _x, _ = next(iter(train_loader))
    print(f"    encoder_cat[0,0,:]={_x['encoder_cat'][0,0,:].tolist()} "
          f"vs df codes={[int(df[c].cat.codes.iloc[0]) for c in known_cat]}")

    print(f"==> Training ({args.epochs} epochs) ...")
    trainer, model = train_model(model, train_loader, val_loader, max_epochs=args.epochs)

    # 训练指标
    metrics = {
        "best_val_loss": float(trainer.callback_metrics.get("val_loss", -1)),
        "epochs_trained": trainer.current_epoch,
        "days_used": args.days if args.days > 0 else "all",
        "rows_used": int(len(df)),
    }
    print(f"    val_loss = {metrics['best_val_loss']:.4f}")

    print("==> Exporting to ONNX ...")
    onnx_path = OFFLINE / f"tft_model{suffix}.onnx"
    exported = export_onnx(model, training_ds, onnx_path)
    metrics["onnx_exported"] = bool(exported)

    # 无论导出成败都保存权重，便于之后不重训直接重试导出
    pt_path = OFFLINE / f"tft_model{suffix}.pt"
    torch.save(model.state_dict(), str(pt_path))
    print(f"    Saved weights: {pt_path.name}")

    with open(OFFLINE / f"training_summary{suffix}.json", "w", encoding="utf-8") as f:
        json.dump(metrics, f, ensure_ascii=False, indent=2)

    print(f"\n==> All done. Artifacts in {OFFLINE}/")
    for name in [f"tft_model{suffix}.onnx", f"preprocessing_params{suffix}.json",
                 f"training_summary{suffix}.json"]:
        p = OFFLINE / name
        if p.exists():
            print(f"    {p.name} ({p.stat().st_size/1024:.0f} KB)")


if __name__ == "__main__":
    main()
