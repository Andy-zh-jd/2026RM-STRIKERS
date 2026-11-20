# -*- coding: utf-8 -*-
"""备份 / 核对车上固件（整片 STM32F407 flash 读回）。

为什么（2026-11-18）：车上固件可能和仓库不一致（本地改过、没提交/推送），
**烧录前先把它整片读回来存档**，以后任何时候都能恢复；读回时还会自动扫描
固件里的 "RM2027_C ..." 版本串，确认板上跑的到底是哪一版。

用法（板子接 ST-Link，板子可以不接 24V，ST-Link 供电即可）：
    python tools\\backup_firmware.py                    # 读回并保存 .hex/.bin，同时和 build 对比
    python tools\\backup_firmware.py --no-compare       # 只备份，不对比
    python tools\\backup_firmware.py --out backups/xxx.hex

恢复（把备份刷回板上）：
    pyocd flash -t stm32f407ighx backups\\RM_Frame_C_oncar_<时间>.hex

输出：
    backups/RM_Frame_C_oncar_<时间>.hex   Intel HEX（可直接 pyocd flash 回来）
    backups/RM_Frame_C_oncar_<时间>.bin   原始二进制（0x08000000 起）
    sha256 打印到控制台（记到 实测记录 里，以后核对用）
"""
import argparse
import datetime
import hashlib
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
BUILD = os.path.join(ROOT, "01_firmware", "RM2027_School_Comp_Frame", "build")
ELF = os.path.join(BUILD, "RM_Frame_C.elf")
BIN = os.path.join(BUILD, "RM_Frame_C.bin")
FLASH_BASE = 0x08000000
FLASH_SIZE_APP = 0x20000     # 默认备份前 128 KB（App 实测 58 KB 多，足够；恢复也快）
FLASH_SIZE_FULL = 0x100000   # --full：整片 1 MB（STM32F407IG）
CHUNK = 4096
DBGMCU_APB1_FZ = 0xE0042008  # bit12 = DBG_IWDG_STOP：halt 时冻结独立看门狗，防止长 halt 被复位


def objcopy_bin():
    """用 ELF 生成 build/RM_Frame_C.bin（如果比 ELF 旧才重新生成）。"""
    try:
        if (not os.path.exists(BIN)) or os.path.getmtime(BIN) < os.path.getmtime(ELF):
            subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", ELF, BIN], check=True)
    except Exception as e:
        print("[!] objcopy 失败（跳过对比）:", e)
        return None
    return BIN if os.path.exists(BIN) else None


def write_intel_hex(path, base, data):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for off in range(0, len(data), 16):
            chunk = data[off:off + 16]
            addr = base + off
            rec = bytes([len(chunk), (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + chunk
            chk = (-sum(rec)) & 0xFF
            f.write(":" + rec.hex().upper() + "%02X" % chk + "\n")
        # EOF
        f.write(":00000001FF\n")


def find_version_strings(data):
    """在固件二进制里找可读字符串（如 'RM2027_C 2026-11-18 ...'）。"""
    out = []
    for m in re.finditer(rb"[ -~]{12,}", data):
        s = m.group().decode("ascii", "replace")
        if "RM2027" in s or "FW " in s or "2026-" in s:
            out.append((base_addr(m.start()), s))
    return out


def base_addr(off):
    return FLASH_BASE + off


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="", help="输出 .hex 路径（默认 backups/RM_Frame_C_oncar_<时间>.hex）")
    ap.add_argument("--no-compare", action="store_true", help="不和 build/RM_Frame_C.bin 对比")
    ap.add_argument("--full", action="store_true", help="整片读 1 MB（默认只读前 128 KB App 区）")
    ap.add_argument("--size", default="", help="自定义读取大小，如 0x30000（十六进制）")

    size = FLASH_SIZE_FULL if args.full else FLASH_SIZE_APP
    if args.size:
        size = int(args.size, 0)
    size = (size + CHUNK - 1) // CHUNK * CHUNK
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.out or os.path.join(ROOT, "backups", "RM_Frame_C_oncar_%s.hex" % stamp)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    binout = os.path.splitext(out)[0] + ".bin"

    session = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f407ighx", options={"resume_on_disconnect": True})
    if session is None:
        print("[!] 没有探针")
        sys.exit(1)

    print("[+] 读取板上 flash: 0x%08X ~ 0x%08X (%d KB)"
          % (FLASH_BASE, FLASH_BASE + size - 1, size // 1024))
    print("    （读之前先停稳车；工具会冻结 IWDG 后 halt，读完自动恢复运行）")
    chunks = []
    with session:
        target = session.target
        target.resume()
        old_fz = None
        try:
            # 冻结 IWDG：halt 期间看门狗不跑，长读也不会把板子复位
            try:
                import struct as _st
                old_fz = _st.unpack("<I", bytes(target.read_memory_block8(DBGMCU_APB1_FZ, 4)))[0]
                target.write_memory_block8(DBGMCU_APB1_FZ, _st.pack("<I", old_fz | (1 << 12)))
            except Exception as e:
                print("[!] 冻结 IWDG 失败（继续，但 halt 太久的风险自担）:", e)
            target.halt()
            for off in range(0, size, CHUNK):
                chunks.append(bytes(target.read_memory_block8(FLASH_BASE + off, CHUNK)))
                if off % (CHUNK * 64) == 0:
                    print("    已读 %d KB" % (off // 1024), flush=True)
        finally:
            try:
                target.resume()
            except Exception:
                pass
            if old_fz is not None:
                try:
                    import struct as _st
                    target.write_memory_block8(DBGMCU_APB1_FZ, _st.pack("<I", old_fz))
                except Exception:
                    pass
    data = b"".join(chunks)

    write_intel_hex(out, FLASH_BASE, data)
    with open(binout, "wb") as f:
        f.write(data)
    sha = hashlib.sha256(data).hexdigest()
    print("\n[+] 已保存:")
    print("    HEX :", out)
    print("    BIN :", binout)
    print("    SHA256:", sha)
    print("    恢复: pyocd flash -t stm32f407ighx \"%s\"" % out)

    vers = find_version_strings(data)
    if vers:
        print("\n[+] 固件里找到的版本/标识串:")
        for addr, s in vers[:6]:
            print("    0x%08X  %s" % (addr, s))
    else:
        print("\n[!] 没有扫到版本串（说明板上固件是加版本号宏之前的旧版本，或是别人编译的）")

    if not args.no_compare:
        b = objcopy_bin()
        if b:
            with open(b, "rb") as f:
                local = f.read()
            n = min(len(local), len(data))
            diff = sum(1 for i in range(n) if local[i] != data[i])
            total = len(data)
            # flash 未用区一般是 0xFF；只统计 local 有内容的范围
            print("\n[+] 与当前仓库构建 build/RM_Frame_C.bin 对比:")
            print("    本地 bin 大小 %d B，diff 字节 %d" % (len(local), diff))
            if diff == 0 and len(local) <= total:
                print("    ✅ 板上固件 == 当前仓库构建（没被别人改过）")
            else:
                print("    ⚠️ 不一致：板上固件和当前仓库构建不同 —— 可能在别人电脑上编译烧过，")
                print("       或者你还没烧最新构建。**烧录前先确认车上那版源码在谁手上。**")
                # 打印第一个差异地址
                for i in range(n):
                    if local[i] != data[i]:
                        print("    首个差异地址: 0x%08X（板上 0x%02X / 本地 0x%02X）"
                              % (FLASH_BASE + i, data[i], local[i]))
                        break


if __name__ == "__main__":
    main()
