# -*- coding: utf-8 -*-
"""SWD 快照读取：完全绕开 USB CDC，用 ST-Link 直接读固件里的 g_dbg[16]。

用法： python tools\read_swd.py [次数]
固件侧约定（UserTask.cpp 的 UserHandle）：
  [0] tgt   机构电机目标 rpm ×100      [1] rpm   机构实测 rpm ×100
  [2] cur   机构电流原值                [3] stop  机构 stopFlag
  [4] vbat  电压 ×100                  [5] sL    左三位开关
  [6] sR    右三位开关                 [7] col   右摇杆 ×100
  [8..11]  r1~r4 底盘四轮 rpm ×100     [12] 底盘 stopFlag   [13] 底盘目标 rpm ×100
地址随固件变化，改代码后重新查：
  arm-none-eabi-nm -C build\RM_Frame_C.elf | findstr " g_dbg$"
"""
import sys, time
from pyocd.core.helpers import ConnectHelper

ADDR = 0x20002700
NAMES = ["tgt", "rpm", "cur", "stop", "vbat", "sL", "sR", "col",
         "r1", "r2", "r3", "r4", "cstop", "ctgt"]

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    s = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx", options={"resume_on_disconnect": True})
    with s:
        t = s.target
        for _ in range(n):
            t.resume(); time.sleep(0.4)
            t.halt()
            v = [t.read32(ADDR + 4 * k) for k in range(14)]
            v = [x - (1 << 32) if x >= (1 << 31) else x for x in v]
            d = dict(zip(NAMES, v))
            print("M2006: tgt%6.2f m%7.2f cur%6d st%d | 四轮: r1 %7.2f r2 %7.2f r3 %7.2f r4 %7.2f cst%d ctgt%6.2f | vbat%6.2f sL%3d sR%3d col%5.2f" % (
                d["tgt"] / 100, d["rpm"] / 100, d["cur"], d["stop"],
                d["r1"] / 100, d["r2"] / 100, d["r3"] / 100, d["r4"] / 100,
                d["cstop"], d["ctgt"] / 100,
                d["vbat"] / 100, d["sL"], d["sR"], d["col"] / 100))
        t.resume()

if __name__ == "__main__":
    main()