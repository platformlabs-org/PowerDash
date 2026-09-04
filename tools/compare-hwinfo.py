#!/usr/bin/env python3
"""compare-hwinfo.py —— PowerDash CSV v3 与 HWiNFO 采样 CSV 的逐列比对。

用法:
    python tools/compare-hwinfo.py <hwinfo.csv> <ours.csv> [--rows N]

两份 CSV 均以首行为表头;按"列名完全一致"取交集,逐列统计:
    列均值 | 双方行数 | ours 有效率 | 均值差 | 状态
状态:
    PASS   —— 同名列且均值差在容差内(或双方均值都接近 0)
    DIFF   —— 同名列但均值差超容差(温度 ≤2°C / 功率 ≤max(10%,0.5W) /
             时钟 ≤5% / 百分比列 ≤3pp / 其余 ≤10%)
    ZERO   —— ours 该列全为空(未采集到/不支持)
    ONLY-HW / ONLY-OURS —— 单侧存在的列(缺列审计用)

仅做区间/均值级别的结构性比对(两份采样不同步,逐行比对无意义)。
"""
import argparse
import csv
import sys
from pathlib import Path


def load(path: Path):
    with open(path, newline="", encoding="utf-8-sig", errors="replace") as f:
        rows = list(csv.reader(f))
    if not rows:
        raise SystemExit(f"empty csv: {path}")
    header = rows[0]
    data = [r for r in rows[1:] if any(c.strip() for c in r)]
    return header, data


def to_float(cell: str):
    cell = (cell or "").strip()
    if not cell:
        return None
    try:
        v = float(cell.replace(",", ""))
        return v
    except ValueError:
        return None


def mean(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else None


def tolerance(name: str, hw: float):
    """按列名单位给出绝对容差;返回 None 表示用相对 10%。"""
    low = name.lower()
    if "[°c]" in low or "[癈]" in low or "temperature" in low:
        return 2.0
    if "[w]" in low:
        return max(0.1 * hw, 0.5)
    if "[mhz]" in low or "clock" in low:
        return max(0.05 * abs(hw), 100.0) if abs(hw) > 10 else 20.0
    if "[%]" in low:
        return 3.0
    return None  # 相对 10%


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hwinfo")
    ap.add_argument("ours")
    ap.add_argument("--rows", type=int, default=0, help="只比前 N 行(默认全部)")
    args = ap.parse_args()

    hw_header, hw_data = load(Path(args.hwinfo))
    our_header, our_data = load(Path(args.ours))
    if args.rows:
        hw_data, our_data = hw_data[: args.rows], our_data[: args.rows]

    hw_idx = {n: i for i, n in enumerate(hw_header)}
    our_idx = {n: i for i, n in enumerate(our_header)}

    common = [n for n in hw_header if n in our_idx and n.strip()]
    only_hw = [n for n in hw_header if n not in our_idx and n.strip()]
    only_ours = [n for n in our_header if n not in hw_idx and n.strip()]

    print(f"hwinfo rows={len(hw_data)} cols={len(hw_header)}   "
          f"ours rows={len(our_data)} cols={len(our_header)}   common={len(common)}")
    print(f"{'status':7} {'diff':>10}  {'hw-mean':>10} {'our-mean':>10} {'valid%':>6}  column")
    npass = ndiff = nzero = 0
    for n in common:
        hw_col = [to_float(r[hw_idx[n]]) for r in hw_data if hw_idx[n] < len(r)]
        our_col = [to_float(r[our_idx[n]]) for r in our_data if our_idx[n] < len(r)]
        hw_m, our_m = mean(hw_col), mean(our_col)
        valid = (len(our_col) - our_col.count(None)) / max(len(our_col), 1)
        if our_m is None:
            print(f"{'ZERO':7} {'-':>10}  {hw_m if hw_m is not None else '-':>10} "
                  f"{'-':>10} {valid:6.0%}  {n}")
            nzero += 1
            continue
        if hw_m is None:
            print(f"{'HW-TEXT':7} {'-':>10}  {'-':>10} {our_m:>10.3f} {valid:6.0%}  {n}")
            continue
        d = our_m - hw_m
        tol = tolerance(n, hw_m)
        ok = abs(d) <= (tol if tol is not None else 0.1 * abs(hw_m) + 1e-9)
        status = "PASS" if ok else "DIFF"
        npass, ndiff = npass + ok, ndiff + (not ok)
        print(f"{status:7} {d:>+10.3f}  {hw_m:>10.3f} {our_m:>10.3f} {valid:6.0%}  {n}")

    print(f"\nsummary: PASS={npass} DIFF={ndiff} ZERO={nzero} "
          f"only-hwinfo={len(only_hw)} only-ours={len(only_ours)}")
    if only_hw:
        print("\ncolumns only in HWiNFO (审计缺失用):")
        for n in only_hw:
            print(f"  {n}")
    if only_ours:
        print("\ncolumns only in ours:")
        for n in only_ours:
            print(f"  {n}")
    return 1 if ndiff else 0


if __name__ == "__main__":
    sys.exit(main())
