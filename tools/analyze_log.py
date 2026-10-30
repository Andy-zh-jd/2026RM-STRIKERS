# -*- coding: utf-8 -*-
"""分析 read_serial.py --log 录下来的串口日志。

用法：
    python tools\\read_serial.py --log logs\\kin.csv    # 先录（做动作）
    python tools\\analyze_log.py logs\\kin.csv          # 再分析

会把每一行解析成 q0..q3 / t1..t4 / r1..r4 / sL / sR，然后按"哪个轴超出死区"
把样本分成 前进 / 横移 / 自转 三段，输出每段的目标与回读符号，用来判断：
  1. 同一动作下四个轮子的 t 符号是否符合运动学
  2. r 是否跟得上 t
  3. 有没有残留目标（松手后 t 应该立刻变 0）
"""
import re
import sys
from collections import defaultdict

FIELDS = ["q0", "q1", "q2", "q3", "t1", "t2", "t3", "t4",
          "r1", "r2", "r3", "r4", "sL", "sR", "col"]
RE = {k: re.compile(k + r"=(-?\d+(?:\.\d+)?)") for k in FIELDS}

# 死区 0.10 × 671 ≈ 67，取 80 作为"明显推杆"的阈值
THR = 80


def parse(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "r1=" not in line:
                continue
            d = {}
            for k, r in RE.items():
                m = r.search(line)
                d[k] = float(m.group(1)) if m else None
            if all(d[k] is not None for k in ("q0", "q2", "q3", "t1", "r1")):
                rows.append(d)
    return rows


def classify(d):
    """按摇杆偏离量判断这一帧是哪种动作"""
    if abs(d["q2"]) > THR and abs(d["q0"]) <= THR and abs(d["q3"]) <= THR:
        return "前进/后退"
    if abs(d["q0"]) > THR:
        return "横移(右推)"
    if abs(d["q3"]) > THR:
        return "自转(右推)"
    if abs(d["q2"]) <= THR and abs(d["q0"]) <= THR and abs(d["q3"]) <= THR:
        return "静止"
    return "混合"


def sign(x, eps=0.5):
    if x is None or abs(x) < eps:
        return " 0"
    return "+" if x > 0 else "-"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/kin.csv"
    rows = parse(path)
    if not rows:
        print("[!] 没解析到数据，检查文件路径")
        return
    print(f"[+] {path}: {len(rows)} 帧")

    groups = defaultdict(list)
    for d in rows:
        groups[classify(d)].append(d)

    for name in ["静止", "前进/后退", "横移(右推)", "自转(右推)", "混合"]:
        g = groups.get(name)
        if not g:
            continue
        n = len(g)
        mt = [sum(d[f"t{i}"] for d in g) / n for i in range(1, 5)]
        mr = [sum(d[f"r{i}"] for d in g) / n for i in range(1, 5)]
        mq = [sum(d[k] for d in g) / n for k in ("q0", "q2", "q3")]
        print(f"\n=== {name}  {n} 帧   q0={mq[0]:+.0f} q2={mq[1]:+.0f} q3={mq[2]:+.0f}")
        print("    t 平均: " + "  ".join(f"t{i+1}={mt[i]:+7.2f}({sign(mt[i])})" for i in range(4)))
        print("    r 平均: " + "  ".join(f"r{i+1}={mr[i]:+7.2f}({sign(mr[i])})" for i in range(4)))
        ok = all(sign(mt[i]) == sign(mr[i]) for i in range(4))
        print("    t/r 符号一致: " + ("是 ✅" if ok else "否 ❌"))


if __name__ == "__main__":
    main()
