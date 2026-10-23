# -*- coding: utf-8 -*-
"""SWD 快照读取：完全绕开 USB CDC，用 ST-Link 直接读固件里的 g_dbg[]。

为什么需要它：IMU 的 SPI 读在 1ms 中断里死等时会把 USB 打死（口在、打不开），
这时只能用 SWD 直接读 RAM 看状态。

用法：
    python tools\\read_swd.py                 # 自动从 build\\RM_Frame_C.elf 里找 g_dbg 地址
    python tools\\read_swd.py -n 5            # 读 5 次
    python tools\\read_swd.py -a 0x20002700   # 手动指定地址（板上跑的是旧固件时用）

固件侧约定（UserTask.cpp 的 UserHandle，14 个槽）：
  [0] tgt 机构目标 rpm×100   [1] rpm 机构实测 rpm×100  [2] cur 电流原值  [3] stop
  [4] vbat×100              [5] sL  [6] sR  [7] col×100
  [8..11] r1..r4 底盘四轮 rpm×100   [12] 底盘 stopFlag  [13] 底盘目标 rpm×100

⚠️ 改代码后 g_dbg 地址会变：必须重新用 nm 查（本脚本已自动做）。
   arm-none-eabi-nm -C build\\RM_Frame_C.elf | findstr " g_dbg$"
"""
import argparse
import os
import re
import subprocess
import sys
import time

from pyocd.core.helpers import ConnectHelper

NAMES = ["tgt", "rpm", "cur", "stop", "vbat", "sL", "sR", "col",
         "r1", "r2", "r3", "r4", "cstop", "ctgt"]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "01_firmware", "RM2027_School_Comp_Frame", "build", "RM_Frame_C.elf")


def addr_from_elf(path):
    """用 arm-none-eabi-nm 查 g_dbg 地址；查不到返回 None"""
    if not os.path.isfile(path):
        return None
    try:
        out = subprocess.run(["arm-none-eabi-nm", "-C", path],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return None
    m = re.search(r"^([0-9a-fA-F]{8})\s+\S\s+g_dbg$", out, re.M)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--times", type=int, default=8)
    ap.add_argument("-a", "--addr", default="", help="手动指定 g_dbg 地址，如 0x20002700")
    args = ap.parse_args()

    if args.addr:
        addr = int(args.addr, 16)
        src = "手动指定"
    else:
        addr = addr_from_elf(ELF)
        src = f"从 ELF 自动解析（{os.path.basename(ELF)}）"
        if addr is None:
            addr = 0x20002714
            src = "ELF 没找到/解析失败，用默认值"

    print(f"[+] g_dbg = 0x{addr:08X}  来源：{src}")

    session = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx", options={"resume_on_disconnect": True})
    if session is None:
        print("[!] 没有探针。检查 ST-Link 是否插好、驱动是否装了。")
        sys.exit(1)

    with session:
        t = session.target
        print(f"[+] 已连接：{t.part_number}")
        for i in range(args.times):
            t.resume()
            time.sleep(0.4)
            t.halt()
            v = [t.read32(addr + 4 * k) for k in range(14)]
            v = [x - (1 << 32) if x >= (1 << 31) else x for x in v]
            d = dict(zip(NAMES, v))
            print("M2006 tgt%7.2f m%7.2f cur%6d st%d | 四轮 r1%7.2f r2%7.2f r3%7.2f r4%7.2f cst%d ctgt%7.2f | vbat%6.2f sL%3d sR%3d col%5.2f"
                  % (d["tgt"] / 100, d["rpm"] / 100, d["cur"], d["stop"],
                     d["r1"] / 100, d["r2"] / 100, d["r3"] / 100, d["r4"] / 100,
                     d["cstop"], d["ctgt"] / 100,
                     d["vbat"] / 100, d["sL"], d["sR"], d["col"] / 100))
        t.resume()
        print("[+] 已恢复运行")


if __name__ == "__main__":
    main()
