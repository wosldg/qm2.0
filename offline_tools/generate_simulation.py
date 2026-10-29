# -*- coding: utf-8 -*-
"""
读 sim_config.json → 生成连续 5 分钟粒度的 8 景点仿真时序数据集。

生成逻辑：
  1. 全日内曲线（hour → peak_ratio）用线性插值扩展到 5 分钟粒度
  2. 每个时间点的全园瞬时客流 = total_instant_capacity × daily_curve(t) × season_factor(m) × weekend_ratio × (1 + noise)
  3. 全园客流按 spot.weight 拆分到 8 个景点
  4. arrival_rate / departure_rate 由瞬时客流的导数 + 随机扰动生成
  5. utilization = current_visitors / instant_capacity，映射为拥挤度标签

输出: sim_scenic_flow_train.csv（连续时序，用于 TFT 训练）
"""

import json
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
OFFLINE = ROOT / "offline_tools"


def interpolate_daily_curve(anchor: dict, interval_minutes: int = 5) -> np.ndarray:
    """
    把 hour -> ratio 的锚点，扩展到全天 288 个 5 分钟点（0:00~23:55）。
    运营窗口外强制为 0。
    """
    total = 24 * 60 // interval_minutes  # 288
    t_all = np.linspace(0, 24, total + 1)[:total]  # hours as float
    hours = sorted(anchor.keys())
    ratios = [anchor[h] for h in hours]
    curve = np.interp(t_all, hours, ratios)
    # 边界：闭园后强制 0（interp 可能给负值）
    curve = np.clip(curve, 0, None)
    return curve


def generate_one_day(
    day_start: pd.Timestamp,
    daily_curve: np.ndarray,
    config: dict,
    day_idx: int,
    rng: np.random.Generator,
) -> pd.DataFrame:
    """生成某一天 288 个时间点 × 8 个景点的数据。"""
    spots = config["spots"]
    weekend_ratio = config["weekend_ratio"]
    season_factors = config["season_factors"]
    noise_cfg = config["noise"]
    thresholds = config["crowding_thresholds"]
    interval = config["time_interval_minutes"]
    n_ticks = len(daily_curve)

    is_weekend = day_start.dayofweek >= 5
    season = season_factors.get(day_start.month) or season_factors.get(str(day_start.month), 1.0)
    we = weekend_ratio if is_weekend else 1.0

    # 全天各 tick 的全园瞬时客流（单位：人）
    total_inst = config["total_instant_capacity"]
    base = total_inst * daily_curve * season * we

    # 叠加噪声：高斯 + 偶尔尖峰
    noise = rng.normal(0, noise_cfg["base_cv"], n_ticks)
    noisy = base * (1 + noise)

    # 尖峰：按概率随机放大某些 tick
    spike_mask = rng.random(n_ticks) < noise_cfg["spike_probability"] * 0.05  # 每天约 5% tick
    noisy[spike_mask] *= noise_cfg["spike_factor"]

    noisy = np.clip(noisy, 0, None)

    # 计算 arrival_rate / departure_rate（人/分钟）
    # arrival 是 current 的上升部分，departure 是下降部分
    # 用差分 + 平滑
    diff = np.diff(noisy, prepend=noisy[0])
    arrivals = np.clip(diff, 0, None) / interval  # 人/分钟
    departures = np.clip(-diff, 0, None) / interval

    # 叠加小噪声
    arrivals *= 1 + rng.normal(0, 0.1, n_ticks)
    departures *= 1 + rng.normal(0, 0.1, n_ticks)
    arrivals = np.clip(arrivals, 0, None)
    departures = np.clip(departures, 0, None)

    # 构建 DataFrame：先全园 → 按 spot 权重拆分
    rows = []
    for si, spot in enumerate(spots):
        spot_inst = spot["instant_capacity"]
        w = spot["weight"]

        # 这个景点的瞬时客流
        cur = noisy * w
        cur = np.clip(cur, 0, spot_inst)

        # arrival / departure 也按权重拆
        arr = arrivals * w
        dep = departures * w

        # utilization 和拥挤度
        util = cur / spot_inst
        level = np.select(
            [
                util < thresholds["EMPTY"],
                util < thresholds["NORMAL"],
                util < thresholds["MODERATE"],
                util < thresholds["CROWDED"],
            ],
            ["EMPTY", "NORMAL", "MODERATE", "CROWDED"],
            default="OVERLOADED",
        )

        for t in range(n_ticks):
            records = {
                "spot_id": spot["spot_id"],
                "spot_name": spot["name"],
                "record_time": day_start + pd.Timedelta(minutes=interval * t),
                "current_visitors": int(round(cur[t])),
                "arrival_rate": round(float(arr[t]), 2),
                "departure_rate": round(float(dep[t]), 2),
                "utilization": round(float(util[t]), 4),
                "congestion_level": level[t],
                "month": day_start.month,
                "dayofweek": day_start.dayofweek,
                "is_weekend": int(is_weekend),
                "season_factor": season,
                "instant_capacity": spot_inst,
                "spot_weight": w,
                "tft_time_idx": day_idx * n_ticks + t,  # 全局单调索引
            }
            rows.append(records)

    return pd.DataFrame(rows)


def main():
    rng = np.random.default_rng(42)

    with open(OFFLINE / "sim_config.json", "r", encoding="utf-8") as f:
        config = json.load(f)

    interval = config["time_interval_minutes"]

    print("==> Building daily curve ({}min interval)...".format(interval))
    daily_curve = interpolate_daily_curve(config["daily_curve_anchor"], interval)
    n_ticks_per_day = len(daily_curve)
    print(f"    {n_ticks_per_day} ticks/day, curve area ≈ {daily_curve.mean():.3f}")

    # 生成 2 年数据（2024-01-01 ~ 2025-12-31），共约 730 天
    days = pd.date_range("2024-01-01", "2025-12-31", freq="D")
    print(f"==> Generating {len(days)} days × {len(config['spots'])} spots × {n_ticks_per_day} ticks ...")

    chunks = []
    for i, day in enumerate(days):
        df_day = generate_one_day(day, daily_curve, config, i, rng)
        chunks.append(df_day)
        if (i + 1) % 100 == 0:
            print(f"    day {i+1}/{len(days)} ...")

    df = pd.concat(chunks, ignore_index=True)
    print(f"==> Generated {len(df)} rows ({len(df)/1e6:.2f}M)")

    out_path = OFFLINE / "sim_scenic_flow_train.csv"
    df.to_csv(out_path, index=False)
    print(f"==> Saved to {out_path}")

    # 基本统计检查
    print("\n==> Quick sanity check ==")
    print(df.groupby("spot_name").agg(
        avg_cur=("current_visitors", "mean"),
        peak_cur=("current_visitors", "max"),
        avg_util=("utilization", "mean"),
        n_level=("congestion_level", lambda x: x.value_counts().to_dict()),
    ).to_string())

    print(f"\nweekend_ratio in data = "
          f"{df[df.is_weekend==1].current_visitors.mean() / max(df[df.is_weekend==0].current_visitors.mean(), 1):.2f}")
    print(f"season_factor range = {df.season_factor.min()} ~ {df.season_factor.max()}")


if __name__ == "__main__":
    main()
