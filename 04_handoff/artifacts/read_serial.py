#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
读取 RoboMaster C 板的 USB 虚拟串口输出。

用法:
    python3 read_serial.py              # 一直读，Ctrl+C 退出
    python3 read_serial.py -t 5         # 只读 5 秒
    python3 read_serial.py -d /dev/cu.usbmodem123  # 手动指定设备

说明: USB 虚拟串口不依赖波特率，这里的 115200 只是占位。
"""
import argparse
import glob
import os
import select
import sys
import termios
import time


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def configure(fd):
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    cflag = (cflag & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB))
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    cflag &= ~getattr(termios, "CRTSCTS", 0)
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [0, 0, cflag, 0, ispeed, ospeed, cc])
    termios.tcflush(fd, termios.TCIOFLUSH)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--seconds", type=float, default=0,
                    help="读取时长(秒)，0 = 一直读")
    ap.add_argument("-d", "--device", default=None, help="串口设备路径")
    args = ap.parse_args()

    dev = args.device or find_port()
    if not dev:
        print("❌ 没找到 /dev/cu.usbmodem* 设备。", file=sys.stderr)
        print("   检查: 1) C 板 micro USB 是否插在电脑上", file=sys.stderr)
        print("         2) 是否被 Parallels 抢给了 Windows", file=sys.stderr)
        return 1

    print(f"📡 打开 {dev}   (Ctrl+C 退出)\n", file=sys.stderr)
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)

    end = time.time() + args.seconds if args.seconds > 0 else None
    buf = b""
    try:
        while True:
            if end and time.time() > end:
                break
            r, _, _ = select.select([fd], [], [], 0.2)
            if not r:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError:
                print("\n⚠️  设备断开（板子复位或被拔掉）", file=sys.stderr)
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.rstrip(b"\r").decode("utf-8", "replace"))
    except KeyboardInterrupt:
        print("\n(已停止)", file=sys.stderr)
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
