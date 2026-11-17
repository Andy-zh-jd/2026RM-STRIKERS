# -*- coding: utf-8 -*-
"""读取固件里的车载 RAM 日志（下地测试之后用）。

为什么需要它：下地跑车时不能连电脑（USB 线会把车拽住），所以固件把
t1~t4 / r1~r4 / gz / 饱和比例 / 实际 ω / 航向误差 每 20 ms 记一帧到 RAM
（1024 帧 ≈ 20.5 秒）。跑完插上 ST-Link，用本脚本读出来分析。

⚠️ 必须用 **attach 模式**连接（不能 reset），否则 RAM 会被清空、日志丢失。

用法：
    python tools\\read_log.py              # 打印全部已记录帧 + 统计
    python tools\\read_log.py -n 50        # 只打印最后 50 帧
    python tools\\read_log.py --raw        # 按 CSV 输出（方便 paste 到 Excel / pandas）
    python tools\\read_log.py --yaw-sign -1  # 陀螺轴正方向与车体逆时针相反时用 -1

固件侧定义（ChassisTask.cpp / ChassisTask.h）：
    int16_t  g_wheelLog[WHEEL_LOG_N][WHEEL_LOG_COLS];   // 12 列 int16，×10 存
    volatile uint16_t g_wheelLogIdx;                    // 下一个写入位置
    volatile uint32_t g_wheelLogTotal;                  // 累计帧数

各列（t/r 均为**车体坐标系**：正 = 车前进方向）：
    [0..3] t1..t4 目标输出轴 rpm ×10
    [4..7] r1..r4 实测输出轴 rpm ×10
    [8]    yaw 轴陀螺 °/s ×10（原始轴，未乘 SIGN）
    [9]    饱和归一化比例 ×1000（1000 = 没限幅）
    [10]   实际下发等效自转 ω rad/s ×1000
    [11]   航向锁误差 ° ×10（未开航向锁时无意义）
"""
import argparse
import math
import os
import re
import subprocess
import sys

from pyocd.core.helpers import ConnectHelper

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "01_firmware", "RM2027_School_Comp_Frame", "build", "RM_Frame_C.elf")
PERIOD_MS = 20
LOG_N = 1024          # 与固件 WHEEL_LOG_N 保持一致
LOG_COLS = 12         # 与固件 WHEEL_LOG_COLS 保持一致
K_GEOM = 0.39         # (轮距 0.40 + 轴距 0.38)/2，与固件一致
# 轮径 0.1525 m：输出轴 rpm -> 轮子地面线速度 (m/s)
MPS_PER_RPM = math.pi * 0.1525 / 60.0

COL_NAMES = ["t1", "t2", "t3", "t4", "r1", "r2", "r3", "r4",
             "gz", "scale", "omega", "yaw_err"]


def sym_addr(name):
    try:
        out = subprocess.run(["arm-none-eabi-nm", "-C", ELF],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return None
    m = re.search(r"^([0-9a-fA-F]{8})\s+\S\s+%s$" % re.escape(name), out, re.M)
    return int(m.group(1), 16) if m else None


def read_frames():
    a_log = sym_addr("g_wheelLog")
    a_idx = sym_addr("g_wheelLogIdx")
    a_tot = sym_addr("g_wheelLogTotal")
    if a_log is None or a_idx is None or a_tot is None:
        print("[!] ELF 里找不到日志符号，先 build 一次")
        sys.exit(1)
    print("[+] g_wheelLog=0x%08X  idx=0x%08X  total=0x%08X" % (a_log, a_idx, a_tot))

    session = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx",
        options={"connect_mode": "attach", "resume_on_disconnect": True})
    if session is None:
        print("[!] 没有探针")
        sys.exit(1)

    def read_u16(t, addr):
        """读 uint16（pyocd 的 read32 要求字对齐，所以先对齐再取对应半字）"""
        al = addr & ~3
        w = t.read32(al)
        return (w >> ((addr - al) * 8)) & 0xFFFF

    with session:
        t = session.target
        t.halt()
        idx = read_u16(t, a_idx)
        total = t.read32(a_tot)
        print("[+] 累计 %d 帧（环形缓冲保留最近 %d 帧 = %.1f 秒）"
              % (total, min(total, LOG_N), min(total, LOG_N) * PERIOD_MS / 1000.0))
        # 整块读出来（LOG_N*LOG_COLS 个 int16，4 字节对齐）
        n_half = LOG_N * LOG_COLS
        aligned = a_log & ~3
        skip = (a_log - aligned) // 2
        words = list(t.read_memory_block32(aligned, (n_half + skip + 1) // 2))
        t.resume()

    flat = []
    for w in words:
        flat.append(w & 0xFFFF)
        flat.append((w >> 16) & 0xFFFF)
    flat = flat[skip:skip + n_half]
    frames = []
    for i in range(LOG_N):
        frames.append([x - 65536 if x >= 32768 else x for x in flat[i * LOG_COLS:(i + 1) * LOG_COLS]])

    # 环形重排：累计超过一圈时，最早的帧在 idx 位置
    n = min(total, LOG_N)
    start = idx if total >= LOG_N else 0
    return [frames[(start + i) % LOG_N] for i in range(n)]


def body_from_wheels(rpm4):
    """四轮输出轴 rpm（车体系）→ 车体 (vx, vy, ω)。与固件正解公式一致。"""
    v = [x * MPS_PER_RPM for x in rpm4]
    vx = (v[0] + v[1] + v[2] + v[3]) / 4.0
    vy = (-v[0] + v[1] + v[2] - v[3]) / 4.0
    w = (-v[0] + v[1] - v[2] + v[3]) / (4.0 * K_GEOM)
    return vx, vy, w


def summary(rows, yaw_sign):
    """控制台统计：把一次下地测试变成可对比的数字。"""
    if not rows:
        print("[!] 没有日志帧（可能这次上电后没动过车）")
        return
    print("\n[+] 共 %d 帧 = %.1f s" % (len(rows), len(rows) * PERIOD_MS / 1000.0))

    mov = [v for v in rows if sum(abs(v[i]) for i in range(4)) > 40]
    if mov:
        n = len(mov)
        mt = [sum(v[i] for v in mov) / n / 10.0 for i in range(4)]
        mr = [sum(v[4 + i] for v in mov) / n / 10.0 for i in range(4)]
        print("    目标均值 t1..t4 = %s rpm" % " ".join("%+7.1f" % x for x in mt))
        print("    实测均值 r1..r4 = %s rpm" % " ".join("%+7.1f" % x for x in mr))
        # 跟踪率：各轮 |r|/|t|，只统计目标足够大的帧，避免除零
        track = []
        for i in range(4):
            pairs = [(abs(v[4 + i]), abs(v[i])) for v in mov if abs(v[i]) > 20]
            if pairs:
                track.append(100.0 * sum(r for r, _ in pairs) / max(1e-9, sum(t for _, t in pairs)))
            else:
                track.append(float("nan"))
        print("    跟踪率 = %s %%" % " ".join("%5.1f" % x for x in track))
        dev = max(mr) - min(mr)
        print("    四轮实测极差 = %.1f rpm（>8 说明某轮被拖/轮径不一致）" % dev)
        # 饱和比例：小于 1000 的帧说明归一化在起作用
        sat = [v[9] for v in mov if 0 < v[9] < 1000]
        print("    饱和归一化生效帧 = %d / %d（有就是好事：说明超能力时保住了方向）"
              % (len(sat), n))
        if sat:
            print("    归一化比例最小 = %.3f" % (min(sat) / 1000.0))

        # ---- 指令 vs 实测：车体速度方向 / 自转（这是"斜推侧移"的直接判据）----
        # 只统计推杆明显、平移为主的帧，避免斜坡起步段方向无意义
        dirs_t, dirs_r, w_t, w_r = [], [], [], []
        straight = []
        for v in mov:
            tv = [v[i] / 10.0 for i in range(4)]
            rv = [v[4 + i] / 10.0 for i in range(4)]
            vtx, vty, vtw = body_from_wheels(tv)
            vrx, vry, vrw = body_from_wheels(rv)
            if math.hypot(vtx, vty) > 0.3:
                dirs_t.append(math.degrees(math.atan2(vty, vtx)))
                dirs_r.append(math.degrees(math.atan2(vry, vrx)))
                w_t.append(vtw)
                w_r.append(vrw)
            if abs(vtw) < 0.3 and math.hypot(vtx, vty) > 0.5:
                straight.append((v, tv, rv))
        if dirs_t:
            n2 = len(dirs_t)
            mt_dir = sum(dirs_t) / n2
            mr_dir = sum(dirs_r) / n2
            ddir = (mr_dir - mt_dir + 180.0) % 360.0 - 180.0
            print("    平移方向：指令 %+6.1f°，实测 %+6.1f°，差 %+5.2f°"
                  "（斜推测试应 <3°；%d 帧）" % (mt_dir, mr_dir, ddir, n2))
            print("    自转：指令 %+6.2f rad/s，实测 %+6.2f rad/s（%d 帧）"
                  % (sum(w_t) / n2, sum(w_r) / n2, n2))

        # ---- WHEEL_TRIM 标定提示（只挑"直行"帧：指令自转≈0、在前进）----
        if len(straight) >= 20:
            n3 = len(straight)
            rel = []
            for i in range(4):
                num = sum(s[2][i] for s in straight)
                den = sum((s[2][0] + s[2][1] + s[2][2] + s[2][3]) / 4.0 for s in straight)
                rel.append((num / n3) / max(1e-9, den / n3) - 1.0)
            print("    直行帧各轮实测相对偏差：FL%+5.2f%% FR%+5.2f%% RL%+5.2f%% RR%+5.2f%%"
                  "（%d 帧）" % (rel[0] * 100, rel[1] * 100, rel[2] * 100, rel[3] * 100, n3))
            if max(abs(x) for x in rel) > 0.005:
                hint = ["%s%+0.2f%%" % (nm, -r * 100)
                        for nm, r in zip(["FL", "FR", "RL", "RR"], rel)
                        if abs(r) > 0.005]
                print("    → 建议 WHEEL_TRIM 调整（把偏慢的轮子目标调大）：%s"
                      "（每次只调 0.3%% 以内，改完重测）" % "  ".join(hint))

    # ---- 航向/横向漂移估计 ----
    # 说明：这是"轮速+陀螺"的估算，不是外部真值。用于对比"改前/改后"。
    heading = 0.0
    lat = 0.0            # 横向位移估计（m）：按车体前进速度 × sin(航向)
    gz_int = 0.0
    for v in rows:
        gz = v[8] / 10.0 * yaw_sign          # °/s
        dt = PERIOD_MS / 1000.0
        heading += gz * dt
        gz_int += gz * dt
        vx = (v[4] + v[5] + v[6] + v[7]) / 4.0 / 10.0 * MPS_PER_RPM
        lat += vx * math.sin(math.radians(heading)) * dt
    print("    陀螺积分航向变化 = %+.1f°（yaw_sign=%+d）" % (gz_int, yaw_sign))
    print("    由航向估算的横向偏移 = %+.0f mm（正负取决于转向；直线测试应≈0）" % (lat * 1000.0))

    # 航向锁误差（只有开了 CHASSIS_IMU_YAW_EN 才有意义）
    ye = [abs(v[11]) / 10.0 for v in rows]
    if max(ye) > 0.0:
        print("    航向锁误差：最大 %.1f°，平均 %.1f°" % (max(ye), sum(ye) / len(ye)))
    else:
        print("    航向锁误差：全 0（说明 CHASSIS_IMU_YAW_EN 没开，或没进直行锁）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--tail", type=int, default=0, help="只打印最后 N 帧")
    ap.add_argument("--raw", action="store_true", help="CSV 输出")
    ap.add_argument("--yaw-sign", type=int, default=1, choices=[1, -1],
                    help="陀螺 yaw 轴正方向：与车体逆时针相同为 +1，相反为 -1")
    args = ap.parse_args()

    rows = read_frames()
    if args.tail:
        rows = rows[-args.tail:]

    if args.raw:
        print("t_ms," + ",".join(COL_NAMES))
        for i, v in enumerate(rows):
            print("%d,%s" % (i * PERIOD_MS,
                             ",".join("%.1f" % (x / 10.0) for x in v)))
        return

    print(" 帧  时间     t1     t2     t3     t4  |   r1     r2     r3     r4  |"
          "   gz   scale    omega   yawErr")
    for i, v in enumerate(rows):
        print("%3d %5.1fs %6.1f %6.1f %6.1f %6.1f | %6.1f %6.1f %6.1f %6.1f |"
              " %5.1f %5.3f %7.2f %6.1f"
              % (i, i * PERIOD_MS / 1000.0,
                 v[0] / 10, v[1] / 10, v[2] / 10, v[3] / 10,
                 v[4] / 10, v[5] / 10, v[6] / 10, v[7] / 10,
                 v[8] / 10, v[9] / 1000.0, v[10] / 1000.0, v[11] / 10))

    summary(rows, args.yaw_sign)


if __name__ == "__main__":
    main()
