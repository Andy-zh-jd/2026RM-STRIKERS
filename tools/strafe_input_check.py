# -*- coding: utf-8 -*-
"""横移前后漂移诊断：记录遥控右摇杆两轴 + 四轮目标/实测，判断"前后偏移"是杆量混入还是机械漂移。

背景（2026-11-18）：前后直行/转弯都好，只剩纯左右平移时前后偏；而且实测是
"一边起步偏、一边收杆偏"的**左右不对称**模式。若为机械力不对称应左右对称，
所以先用本工具看：推横移杆时 `right_col`（前后通道）有没有被带出小尖峰。

用法（板子接 ST-Link、24V 上电、车能跑）：
    python tools\\strafe_input_check.py --seconds 60

做测试：
    SB 上档（驾驶模式）→ 右摇杆**纯左移**约 2 m → 回中停 1 s
    → **纯右移**约 2 m → 回中停 1 s；可重复 2~3 组。

输出：
    logs/strafe_input_<时间>.csv（逐样本）
    控制台实时行 + 结束后的判定摘要：
      · "纯横移段" = |lr| > 0.4 的连续区间；
      · 分段统计 段首 0.3 s / 中段 / 段尾 0.3 s 的 |前后杆量| 最大值；
      · 段首或段尾明显 > 中段 → **杆量混入**（固件 ⑧ 死区整形可修）；
      · 中段也一直有前后量 → 操作手在走斜线，不是漂移；
      · 全程前后杆量≈0 但车确实前后偏 → **机械/瞬态漂移**，改用 ⑦ K 或分方向补偿。
"""
import argparse
import csv
import datetime
import os
import re
import struct
import subprocess
import sys
import time

from pyocd.core.helpers import ConnectHelper

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "01_firmware", "RM2027_School_Comp_Frame", "build", "RM_Frame_C.elf")
LR_THR = 0.4          # |横移杆量| 超过它算"横移意图"
EDGE = 0.30           # 段首/段尾统计窗口，s


def sym(name):
    out = subprocess.run(["arm-none-eabi-nm", "-C", ELF],
                         capture_output=True, text=True, timeout=30).stdout
    m = re.search(r"^([0-9a-fA-F]{8})\s+\S\s+%s$" % re.escape(name), out, re.M)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60.0, help="记录时长 s（默认 60）")
    ap.add_argument("--out", default="", help="CSV 路径（默认 logs/strafe_input_<时间>.csv）")
    args = ap.parse_args()

    A = {n: sym(n) for n in ("RemoteControl::rc_ctrl", "wheelTargetRpm", "g_dbg")}
    if any(v is None for v in A.values()):
        print("[!] ELF 里找不到符号，先 build 一次:", A)
        sys.exit(1)

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.out or os.path.join(ROOT, "logs", "strafe_input_%s.csv" % stamp)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    session = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx", options={"resume_on_disconnect": True})
    if session is None:
        print("[!] 没有探针")
        sys.exit(1)

    rows = []   # (t, lr, fb, tgt_FL..BR, r_FL..BR)
    print("[+] 记录 %.0f s -> %s" % (args.seconds, out))
    print("    时间   横移lr   前后fb | 目标FL  FR  BL  BR | 实测FL  FR  BL  BR")
    f = open(out, "w", encoding="utf-8-sig", newline="")
    w = csv.writer(f)
    w.writerow(["t_s", "stick_lr", "stick_fb",
                "t_FL", "t_FR", "t_BL", "t_BR", "r_FL", "r_FR", "r_BL", "r_BR"])
    try:
        with session:
            target = session.target
            target.resume()
            t0 = time.monotonic()
            while True:
                t = time.monotonic() - t0
                if t >= args.seconds:
                    break
                target.resume()
                time.sleep(0.1)
                target.halt()
                raw = bytes(target.read_memory_block8(A["RemoteControl::rc_ctrl"], 28))
                ch = struct.unpack("<4i", raw[0:16])
                tgt = struct.unpack("<4f", bytes(target.read_memory_block8(A["wheelTargetRpm"], 16)))
                dbg = struct.unpack("<24i", bytes(target.read_memory_block8(A["g_dbg"], 96)))
                target.resume()
                t = time.monotonic() - t0
                lr = ch[0] / 671.0     # rcInfo.right_rol
                fb = ch[2] / 671.0     # rcInfo.right_col
                meas = [dbg[8] / 100.0, dbg[9] / 100.0, dbg[10] / 100.0, dbg[11] / 100.0]
                rows.append((t, lr, fb, *tgt, *meas))
                w.writerow(["%.3f" % t, "%.3f" % lr, "%.3f" % fb]
                           + ["%.1f" % x for x in tgt] + ["%.1f" % x for x in meas])
                f.flush()
                print("%6.1fs  %+6.2f  %+6.2f | %6.0f %6.0f %6.0f %6.0f | %6.1f %6.1f %6.1f %6.1f"
                      % (t, lr, fb, *tgt, *meas), flush=True)
    except KeyboardInterrupt:
        print("\n[!] 手动结束")
    finally:
        try:
            f.close()
        except Exception:
            pass
        try:
            session.target.resume()
        except Exception:
            pass

    analyze(rows)


def analyze(rows):
    if len(rows) < 20:
        print("[!] 样本太少（CSV 可能只有表头），检查 ST-Link 连接")
        return

    # 找"纯横移段"：|lr| > LR_THR 的连续区间（允许中间 0.3s 抖动合并）
    segs = []
    i = 0
    n = len(rows)
    while i < n:
        if abs(rows[i][1]) > LR_THR:
            s = i
            while i < n and abs(rows[i][1]) > LR_THR:
                i += 1
            segs.append((s, i - 1))
        else:
            i += 1
    print("\n[+] 找到 %d 个横移段（|横移杆量|>%.1f）" % (len(segs), LR_THR))
    if not segs:
        print("    [!] 没检测到纯横移动作。请推右摇杆左右到接近满杆，各走 2 m。")
        return

    def fbmax(s, e):
        vals = [abs(rows[k][2]) for k in range(max(0, s), min(n, e + 1))]
        return max(vals) if vals else 0.0

    any_mix = False
    print("    段#  方向   时长   段首|fb|max  中段|fb|max  段尾|fb|max  中段|目标fb|")
    for idx, (s, e) in enumerate(segs):
        dur = rows[e][0] - rows[s][0]
        if dur < 0.4:
            continue
        edge_n = max(1, int(EDGE / 0.1))
        head = fbmax(s, s + edge_n)
        tail = fbmax(e - edge_n, e)
        mid = fbmax(s + edge_n + 1, e - edge_n - 1)
        mid_tgt = [abs((rows[k][3] + rows[k][4] + rows[k][5] + rows[k][6]) / 4.0)
                   for k in range(s + edge_n, e - edge_n)] or [0.0]
        d = "+" if rows[s][1] > 0 else "-"
        print("    #%-2d  %s      %4.2fs   %8.2f    %8.2f    %8.2f    %8.1f rpm"
              % (idx + 1, "左" if d == "+" else "右", dur, head, tail, mid, max(mid_tgt)))
        if max(head, tail) > 0.06 and mid < max(head, tail) * 0.7:
            any_mix = True

    print()
    if any_mix:
        print("[判定] 段首/段尾的前后杆量明显大于中段 → **推横移杆时混入了前后分量**。")
        print("       → 固件 ⑧ CHASSIS_STRAFE_FB_DEAD_EN 的平滑死区整形就是针对这个的。")
    else:
        print("[判定] 横移段里前后杆量一直≈0，没有明显杆量混入。")
        print("       → 车如果仍然前后偏，属于**机械/瞬态漂移**：用 ⑦ CHASSIS_STRAFE_FB_COMP")
        print("          （按 2 m 横移偏移标定 K）或分方向/加速度补偿，需地面真值或试车反馈。")
    print("[!] 记录已存:", end=" ")
    print("logs/strafe_input_*.csv（可直接发给队友分析）")


if __name__ == "__main__":
    main()
