# -*- coding: utf-8 -*-
"""读取固件里的车载 RAM 日志（下地测试之后用）。

为什么需要它：下地跑车时不能连电脑（USB 线会把车拽住），所以固件把
t1~t4 / r1~r4 / gz 每 20 ms 记一帧到 RAM（512 帧 ≈ 10.2 秒）。
跑完插上 ST-Link，用本脚本读出来分析。

⚠️ 必须用 **attach 模式**连接（不能 reset），否则 RAM 会被清空、日志丢失。
   pyocd 的 attach 模式：options={"connect_mode": "attach"}

用法：
    python tools\\read_log.py              # 读全部已记录帧
    python tools\\read_log.py -n 50        # 只打印最后 50 帧
    python tools\\read_log.py --raw        # 按 CSV 输出（时间, t1..t4, r1..r4, gz）

固件侧定义（ChassisTask.cpp）：
    int16_t g_wheelLog[512][9];        // 每帧 9 个 int16，值都是 ×10
    volatile uint16_t g_wheelLogCnt;   // 已记录帧数
"""
import argparse
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
LOG_N = 1024        # 与固件 WHEEL_LOG_N 保持一致


def sym_addr(name):
    try:
        out = subprocess.run(["arm-none-eabi-nm", "-C", ELF],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return None
    m = re.search(r"^([0-9a-fA-F]{8})\s+\S\s+%s$" % re.escape(name), out, re.M)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--tail", type=int, default=0, help="只打印最后 N 帧")
    ap.add_argument("--raw", action="store_true", help="CSV 输出")
    args = ap.parse_args()

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
        # 整块读出来（9216 字节，4 字节对齐）
        aligned = a_log & ~3
        skip = (a_log - aligned) // 2
        words = list(t.read_memory_block32(aligned, (LOG_N * 9 + skip + 1) // 2))
        t.resume()

    flat = []
    for w in words:
        flat.append(w & 0xFFFF)
        flat.append((w >> 16) & 0xFFFF)
    flat = flat[skip:skip + LOG_N * 9]
    frames = []
    for i in range(LOG_N):
        frames.append([x - 65536 if x >= 32768 else x for x in flat[i * 9:(i + 1) * 9]])

    # 环形重排：最早的帧在 total>=512 时是 idx，否则是 0
    n = min(total, LOG_N)
    start = idx if total >= LOG_N else 0
    rows = [frames[(start + i) % LOG_N] for i in range(n)]
    if args.tail:
        rows = rows[-args.tail:]

    if args.raw:
        print("t_ms,t1,t2,t3,t4,r1,r2,r3,r4,gx,gy,gz")
        for i, v in enumerate(rows):
            print("%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f"
                  % (i * PERIOD_MS, v[0] / 10, v[1] / 10, v[2] / 10, v[3] / 10,
                     v[4] / 10, v[5] / 10, v[6] / 10, v[7] / 10, v[8] / 10, v[9] / 10, v[10] / 10))
        return

    print(" 帧  时间    t1     t2     t3     t4  |   r1     r2     r3     r4  |   gz")
    for i, v in enumerate(rows):
        print("%3d %5.1fs %6.1f %6.1f %6.1f %6.1f | %6.1f %6.1f %6.1f %6.1f | %6.1f"
              % (i, i * PERIOD_MS / 1000.0, v[0] / 10, v[1] / 10, v[2] / 10, v[3] / 10,
                 v[4] / 10, v[5] / 10, v[6] / 10, v[7] / 10, v[8] / 10, v[9] / 10, v[10] / 10))

    if rows:
        # 简单统计：有驱动的帧里，四个轮子的平均回读
        mov = [v for v in rows if abs(v[0]) + abs(v[1]) + abs(v[2]) + abs(v[3]) > 40]
        if mov:
            n = len(mov)
            avg = [sum(v[4 + k] for v in mov) / n / 10 for k in range(4)]
            print("\n[+] 有驱动帧 %d 个，四轮平均回读: r1=%.1f r2=%.1f r3=%.1f r4=%.1f"
                  % (n, *avg))
            print("    最大偏差 %.1f rpm（大于 8 说明某轮被拖/轮径不一致）"
                  % (max(avg) - min(avg)))


if __name__ == "__main__":
    main()


