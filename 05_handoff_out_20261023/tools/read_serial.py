#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读 C 板 USB-CDC 串口（Windows 版，自动找设备）

用法：
    python tools\\read_serial.py           # 自动找 STM32 虚拟串口，一直读到 Ctrl+C
    python tools\\read_serial.py -t 10     # 只读 10 秒
    python tools\\read_serial.py -p COM7   # 手动指定端口

为什么要有这个脚本：板子每次复位/重插，COM 口号都可能变；
这个脚本按 VID 0x0483（STMicroelectronics）+ 描述里的 STM32 自动定位，
省掉每次去设备管理器里翻的时间。
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports


def find_port() -> str:
    cands = []
    for p in list_ports.comports():
        vid = getattr(p, "vid", None)
        desc = (p.description or "")
        if vid == 0x0483 or "STM32" in desc or "STMicro" in desc:
            cands.append(p)
    if len(cands) == 1:
        return cands[0].device
    if not cands:
        print("[!] 没找到 STM32 虚拟串口。检查：")
        print("    1) 板子是不是用『数据线』连的（很多线只充电）")
        print("    2) 烧进去的固件是不是把 USB CDC 初始化了（MX_USB_DEVICE_Init）")
        print("    3) 设备管理器里有没有『STM32 Virtual ComPort』")
        print("\n当前所有串口：")
        for p in list_ports.comports():
            print(f"    {p.device:8s} {p.description}")
        sys.exit(1)
    print("[!] 找到多个候选，用 -p 指定：")
    for p in cands:
        print(f"    {p.device:8s} {p.description}")
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--seconds", type=float, default=0, help="读多少秒，0=一直读")
    ap.add_argument("-p", "--port", default="", help="手动指定串口，如 COM7")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"[+] 打开 {port} @ {args.baud}，Ctrl+C 退出", flush=True)

    try:
        ser = serial.Serial(port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        print(f"[!] 打不开：{e}")
        print("    常见原因：板子刚复位正在重新枚举 → 等 1~2 秒重试；或别的程序占着这个口")
        sys.exit(1)

    t0 = time.time()
    nbytes = nlines = 0
    try:
        while True:
            line = ser.readline()
            if line:
                nbytes += len(line)
                nlines += 1
                sys.stdout.write(line.decode("utf-8", "replace"))
                sys.stdout.flush()
            if args.seconds and (time.time() - t0) >= args.seconds:
                break
            if not line and args.seconds == 0:
                time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        dt = max(time.time() - t0, 1e-6)
        print(f"\n[+] 结束：{dt:.1f}s，{nlines} 行 / {nbytes} 字节（{nbytes/dt:.0f} B/s）")


if __name__ == "__main__":
    main()
