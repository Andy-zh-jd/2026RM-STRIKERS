# -*- coding: utf-8 -*-
"""读取机械臂 6 个舵机的当前"逻辑角"与限位（SWD，绕开 USB 串口）。

⚠️ 重要：PCA9685 是**开环 PWM**，舵机没有角度回读。本工具读的是**固件里的命令角**
   `armAngle[]`（以及已下发角 `armSent[]`、限位 `armLimitMin/Max`）。
   - 如果舵机没有 5V/24V 供电（`vbat` 只有 3~4 V），PCA9685 不工作、armSent=-999（卸力），
     读到的只是固件里的默认/上次目标值，**不代表机械臂实际姿态**（手可以随意掰动它）。
   - 只有接上 24 V、PCA 初始化成功、舵机有力之后，`armAngle` 才等于机械臂实际保持的目标角。

用法：
    python tools\\read_arm.py                 # 读一次
    python tools\\read_arm.py --watch 30      # 连续读 30 s（每 0.5 s 一行，调限位时用）
    python tools\\read_arm.py --watch 60 --interval 0.2

通道：ch0 底座 / ch1 大臂 / ch2 小臂 / ch3 腕俯仰 / ch4 腕旋转 / ch5 夹爪
提示：`ARM_DIR[ch] = -1` 的通道（目前只有 ch0）**物理角 = 180 - 逻辑角**；其余逻辑角=物理角。
"""
import argparse
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

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "01_firmware", "RM2027_School_Comp_Frame", "build", "RM_Frame_C.elf")
CH_NAMES = ["ch0 底座", "ch1 大臂", "ch2 小臂", "ch3 腕俯仰", "ch4 腕旋转", "ch5 夹爪"]
SERVO_US_MIN, SERVO_US_MAX = 500, 2500


def sym(name):
    out = subprocess.run(["arm-none-eabi-nm", "-C", ELF],
                         capture_output=True, text=True, timeout=30).stdout
    m = re.search(r"^([0-9a-fA-F]{8})\s+\S\s+%s$" % re.escape(name), out, re.M)
    return int(m.group(1), 16) if m else None


def rdf(t, addr, n):
    return list(struct.unpack("<%df" % n, bytes(t.read_memory_block8(addr, 4 * n))))


def rdb(t, addr, n=1):
    return list(bytes(t.read_memory_block8(addr, n)))


def pulse_us(angle):
    a = max(0.0, min(180.0, angle))
    return SERVO_US_MIN + (SERVO_US_MAX - SERVO_US_MIN) * a / 180.0


def read_state(t, A):
    ang = rdf(t, A["armAngle"], 6)
    sent = rdf(t, A["armSent"], 6)
    lo = rdf(t, A["armLimitMin"], 6)
    hi = rdf(t, A["armLimitMax"], 6)
    dr = rdf(t, A["ARM_DIR"], 6)
    m1 = rdb(t, A["g_pcaMode1"])[0]
    rec = rdb(t, A["g_pcaRecover"])[0]
    dbg = struct.unpack("<24i", bytes(t.read_memory_block8(A["g_dbg"], 96)))
    return dict(ang=ang, sent=sent, lo=lo, hi=hi, dir=dr, m1=m1, rec=rec, dbg=dbg)


def print_table(s):
    print("通道         逻辑角   物理角   PWM(us)   下限    上限   已下发")
    for i in range(6):
        phys = 180.0 - s["ang"][i] if s["dir"][i] < 0 else s["ang"][i]
        us = pulse_us(phys)
        sent = s["sent"][i]
        sent_s = "  -999(未下发/卸力)" if sent < -900 else "%6.1f" % sent
        print("%-10s %7.1f  %6.1f   %6.0f   %5.0f  %5.0f  %s"
              % (CH_NAMES[i], s["ang"][i], phys, us, s["lo"][i], s["hi"][i], sent_s))
    print("ARM_DIR = %s   (ch0=-1 → 物理角 = 180 - 逻辑角)"
          % str([int(x) for x in s["dir"]]))
    print("PCA MODE1=0x%02X (SLEEP bit4=%d)  pcaRecover=%d  vbat=%.2f V"
          % (s["m1"], (s["m1"] >> 4) & 1, s["rec"], s["dbg"][4] / 100.0))
    if s["dbg"][4] / 100.0 < 10.0:
        print("⚠️ vbat<10 V：只有 USB/ST-Link 供电，PCA9685/舵机没有 5 V/24 V → "
              "armAngle 只是命令值，不代表实际姿态；调的限位要等上电后才能验证。")
    if s["m1"] == 0x00:
        print("⚠️ g_pcaMode1=0：PCA9685 没有初始化成功（常见原因：没接 24 V，5 V 降压没电）。")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--watch", type=float, default=0.0,
                    help="连续读取秒数（默认 0=只读一次）；调限位时用")
    ap.add_argument("--interval", type=float, default=0.5, help="连续模式的间隔 s")
    args = ap.parse_args()

    A = {n: sym(n) for n in ("armAngle", "armSent", "armLimitMin", "armLimitMax",
                             "ARM_DIR", "g_pcaMode1", "g_pcaRecover", "g_dbg")}
    if any(v is None for v in A.values()):
        print("[!] ELF 里找不到符号，先 build 一次:", A)
        sys.exit(1)

    session = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx", options={"resume_on_disconnect": True})
    if session is None:
        print("[!] 没有探针")
        sys.exit(1)

    with session:
        t = session.target
        if args.watch <= 0:
            t.halt()
            s = read_state(t, A)
            t.resume()
            print_table(s)
            return
        t0 = time.monotonic()
        print("[+] 连续读取 %.0f s（每 %.1f s），Ctrl+C 可提前结束" % (args.watch, args.interval))
        print("    时间   ch0    ch1    ch2    ch3    ch4    ch5      (逻辑角，限位见结尾)")
        try:
            while time.monotonic() - t0 < args.watch:
                t.resume()
                time.sleep(args.interval)
                t.halt()
                s = read_state(t, A)
                print("%6.1fs  %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f"
                      % (time.monotonic() - t0, *s["ang"]))
                t.resume()
        except KeyboardInterrupt:
            print("\n[!] 手动结束")
        finally:
            t.resume()
        print()
        t.halt()
        s = read_state(t, A)
        t.resume()
        print_table(s)


if __name__ == "__main__":
    main()
