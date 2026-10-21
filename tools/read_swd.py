# -*- coding: utf-8 -*-
"""SWD 快照读取：完全绕开 USB CDC，用 ST-Link 直接读固件里的 g_dbg[8]。

用法： python tools\read_swd.py [次数]
固件侧约定（UserTask.cpp）：g_dbg[0..7] = tgt, rpm, cur, stop, vbat*100, sL, sR, col
地址随固件变化，改代码后请用：
    arm-none-eabi-nm -C build\RM_Frame_C.elf | findstr " g_dbg$"
"""
import sys, time
from pyocd.core.helpers import ConnectHelper

ADDR = 0x20002700          # g_dbg 的地址（改代码后要重新查）
NAMES = ["tgt", "rpm", "cur", "stop", "vbat", "sL", "sR", "col"]

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    s = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx", options={"resume_on_disconnect": True})
    with s:
        t = s.target
        for _ in range(n):
            t.resume(); time.sleep(0.4)
            t.halt()
            v = [t.read32(ADDR + 4 * k) for k in range(8)]
            v = [x - (1 << 32) if x >= (1 << 31) else x for x in v]
            d = dict(zip(NAMES, v))
            print("%6.2f rpm=%7.2f cur=%6d stop=%d vbat=%6.2f sL=%3d sR=%3d col=%5.2f" % (
                d["tgt"] / 100, d["rpm"] / 100, d["cur"], d["stop"],
                d["vbat"] / 100, d["sL"], d["sR"], d["col"] / 100))
        t.resume()

if __name__ == "__main__":
    main()