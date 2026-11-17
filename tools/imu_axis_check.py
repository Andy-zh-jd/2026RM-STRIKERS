# -*- coding: utf-8 -*-
"""IMU yaw 轴/正方向台架确认工具（2026-11-17）

为什么需要：BMI088 装在 C 板上，哪个轴是**车体 yaw（绕竖直轴）**、正方向是不是
"俯视逆时针为正"，本仓库没有记录过。`CHASSIS_IMU_YAW_AXIS` / `SIGN` 填错时，
航向锁会变成正反馈（车越偏越转），所以开 `CHASSIS_IMU_YAW_EN` 之前必须确认。

用法（板子接 ST-Link、只 USB/ST-Link 供电即可）：
    python tools\\imu_axis_check.py --seconds 120 --rate 10

看到脚本开始采样后，按下面动作做（不用卡秒，脚本按"转动事件"自动分段）：
    1) 先把车放平稳、手离开 → 2~3 s
    2) 俯视把**车体逆时针转约 90°**（1~2 s 内转完，像拧方向盘），停住
    3) 保持不动 2~3 s
    4) 俯视把车体**顺时针转回 90°**（回到原方向），停住
    5) 手离开，等采样结束

⚠️ 只绕竖直轴转车体：**不要抬起来、不要前后/左右压倾斜**（工具会用加速度计检查）。
   转的时候尽量"一步到位再停"，别来回抖。
   ⚠️ **给 ST-Link 的 USB 线留出余量**：转车体时扯到线会掉线（2026-11-17 实测踩过）。
      本脚本带断线重连（最多 3 次），但掉线期间的数据会缺。

输出：`logs/imu_axis_<时间>.csv`（逐样本）+ 控制台分析（每个转动事件的三轴积分角度、
加速度是否稳定、推荐的 AXIS/SIGN）。

判断原理：绕竖直轴转 90° 时，**只有真正的 yaw 轴**会积出 ~±90°，
另外两轴积分接近 0；逆时针转 → yaw 轴积分应为正。若相反，则 SIGN 取 -1。
"""
import argparse
import datetime
import math
import os
import re
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
GYRO_SEN = 16.384          # LSB/(°/s)，与固件一致（±2000 dps 量程）
AXIS_NAMES = ["gx", "gy", "gz"]


def sym(name):
    out = subprocess.run(["arm-none-eabi-nm", "-C", ELF],
                         capture_output=True, text=True, timeout=30).stdout
    m = re.search(r"^([0-9a-fA-F]{8})\s+\S\s+%s$" % re.escape(name), out, re.M)
    return int(m.group(1), 16) if m else None


def read_f32(target, addr, n):
    """读 n 个 float32（按字对齐后切片，避免 unaligned read32）。"""
    import struct
    words = target.read_memory_block32(addr & ~3, ((addr & 3) + 4 * n + 3) // 4)
    buf = b"".join(struct.pack("<I", w) for w in words)
    return list(struct.unpack("<%df" % n, buf[addr & 3:(addr & 3) + 4 * n]))


def read_i16_3(target, addr):
    """读 3 个 int16（g_gyro）。"""
    import struct
    words = target.read_memory_block32(addr & ~3, 2)
    return list(struct.unpack("<3h", struct.pack("<II", *words)[:6]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=120.0, help="采样时长 s（默认 120）")
    ap.add_argument("--rate", type=float, default=10.0, help="采样率 Hz（默认 10；越高越吃 USB）")
    ap.add_argument("--out", default="", help="CSV 路径（默认 logs/imu_axis_<时间>.csv）")
    args = ap.parse_args()

    addrs = {n: sym(n) for n in ("g_gyroLpf", "g_gyro", "g_accelG", "g_gyroBias")}
    if any(v is None for v in addrs.values()):
        print("[!] ELF 里找不到符号，先 build 一次:", addrs)
        sys.exit(1)

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.out or os.path.join(ROOT, "logs", "imu_axis_%s.csv" % stamp)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    dt = 1.0 / max(1.0, args.rate)

    print("[+] CSV:", out)
    print("[+] 开始采样 %.0f s @ %.0f Hz —— 现在按提示转动车体" %
          (args.seconds, args.rate))
    print("    动作：静止 2s → 俯视逆时针转 90° → 停 3s → 顺时针转回 90° → 停")
    t0 = time.monotonic()
    retries = 0
    rows = []
    f = open(out, "w", encoding="utf-8-sig", newline="")
    f.write("t_s,raw_gx,raw_gy,raw_gz,ax,ay,az\n")
    try:
        while True:
            t = time.monotonic() - t0
            if t >= args.seconds or retries >= 4:
                break
            session = None
            try:
                session = ConnectHelper.session_with_chosen_probe(
                    target_override="stm32f407ighx",
                    options={"resume_on_disconnect": True})
                if session is None:
                    raise RuntimeError("没有探针")
                with session:
                    target = session.target
                    target.resume()
                    bias = read_f32(target, addrs["g_gyroBias"], 3)
                    acc = read_f32(target, addrs["g_accelG"], 3)
                    k = 0
                    while True:
                        t = time.monotonic() - t0
                        if t >= args.seconds:
                            break
                        target.resume()
                        time.sleep(dt)
                        target.halt()
                        # 每帧只读 gyro（3 int16 = 1~2 个 word），SWD 事务最少
                        raw = read_i16_3(target, addrs["g_gyro"])
                        # bias / accel 变化很慢，每 5 帧读一次即可
                        if k % 5 == 0:
                            bias = read_f32(target, addrs["g_gyroBias"], 3)
                            acc = read_f32(target, addrs["g_accelG"], 3)
                        k += 1
                        r = [((raw[i] - bias[i]) / GYRO_SEN) for i in range(3)]
                        rows.append((t, *r, *acc))
                        f.write("%.3f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f\n"
                                % (t, *r, *acc))
                        f.flush()
                        target.resume()
            except KeyboardInterrupt:
                print("\n[!] 手动停止采样，使用已采数据")
                break
            except Exception as e:
                retries += 1
                print("[!] 采样中断（%s），1 s 后尝试重连（第 %d 次）..." % (e, retries),
                      flush=True)
                time.sleep(1.0)
    finally:
        try:
            f.close()
        except Exception:
            pass
        try:
            if session is not None:
                session.target.resume()
        except Exception:
            pass

    analyze(rows)


def analyze(rows):
    """按角速度幅值把数据分成"转动事件"，对每个事件积分三轴角度。

    rows 元素：(t, raw_gx,raw_gy,raw_gz, ax, ay, az)
    """
    if len(rows) < 10:
        print("[!] 样本太少，无法分析（CSV 可能只有表头）。检查 ST-Link 连接后重试。")
        return
    print("\n[+] 共 %d 个样本，时长 %.1f s" % (len(rows), rows[-1][0] - rows[0][0]))

    # 1) 转动事件检测：三轴合成角速度 > 20 °/s
    mag = [math.sqrt(r[1] ** 2 + r[2] ** 2 + r[3] ** 2) for r in rows]
    thr = 20.0
    gap = 0.5
    events = []
    in_ev = False
    start = 0
    for i, m in enumerate(mag):
        if m > thr and not in_ev:
            in_ev = True
            start = i
        elif m <= thr and in_ev:
            if i - 1 > start:
                events.append([start, i - 1])
            in_ev = False
    if in_ev and len(rows) - 1 > start:
        events.append([start, len(rows) - 1])
    # 合并间隔 < gap 的事件（一次转动中间可能速度过零/短暂停顿）
    merged = []
    for s, e in events:
        if merged and rows[s][0] - rows[merged[-1][1]][0] < gap:
            merged[-1][1] = e
        else:
            merged.append([s, e])
    events = [tuple(x) for x in merged]

    # 2) 加速度稳定性（判断有没有抬车/倾斜）
    base_n = max(5, len(rows) // 20)
    base = [sum(rows[i][4 + k] for i in range(base_n)) / base_n for k in range(3)]
    max_dev = 0.0
    for r in rows:
        dev = math.sqrt(sum((r[4 + k] - base[k]) ** 2 for k in range(3)))
        max_dev = max(max_dev, dev)
    print("    加速度计 |a| 基线 = %.3f，最大偏差 = %.3f g（<0.15 说明基本没抬/倾斜）"
          % (math.sqrt(sum(x * x for x in base)), max_dev))

    if not events:
        print("\n[!] 没检测到明显转动（|ω| 没超过 20 °/s）。")
        print("    可能原因：①转车体时 ST-Link 掉线（CSV 里数据有断档）；②还没开始转。")
        print("    请确认 ST-Link 线不会被扯到，然后重跑本工具再转一次。")
        return

    # 3) 逐事件积分
    print("\n[+] 检测到 %d 个转动事件：" % len(events))
    total_abs = [0.0, 0.0, 0.0]
    first_big = None
    for k, (s, e) in enumerate(events):
        ang = [0.0, 0.0, 0.0]
        for i in range(s + 1, e + 1):
            dt = rows[i][0] - rows[i - 1][0]
            for a in range(3):
                ang[a] += 0.5 * (rows[i][1 + a] + rows[i - 1][1 + a]) * dt
        dur = rows[e][0] - rows[s][0]
        peak = max(mag[s:e + 1])
        print("    #%d  t=%.1f~%.1fs (%.2fs, 峰值 %.0f °/s)  积分角度: "
              "gx=%+7.1f°  gy=%+7.1f°  gz=%+7.1f°"
              % (k + 1, rows[s][0], rows[e][0], dur, peak, *ang))
        for a in range(3):
            total_abs[a] += abs(ang[a])
        if first_big is None and abs(max(ang, key=abs)) > 30:
            first_big = (k + 1, list(ang))

    # 4) 结论
    axis = max(range(3), key=lambda a: total_abs[a])
    print("\n[+] 判定：三轴累计转动量  gx=%.1f°  gy=%.1f°  gz=%.1f°"
          % (total_abs[0], total_abs[1], total_abs[2]))
    print("    → yaw 轴 = %s（index %d）" % (AXIS_NAMES[axis], axis))

    # 用"第一个大转动事件"判断符号；约定用户第一次是俯视逆时针
    if first_big:
        ev, ang = first_big
        sgn = 1 if ang[axis] > 0 else -1
        print("    事件 #%d 中该轴积分 = %+.1f°（第一次转动约定为俯视逆时针）" % (ev, ang[axis]))
        print("    → CHASSIS_IMU_YAW_AXIS = %d    CHASSIS_IMU_YAW_SIGN = %+d"
              % (axis, sgn))
        if sgn > 0:
            print("      结论：俯视逆时针 → 该轴为**正**，SIGN 用 +1")
        else:
            print("      结论：俯视逆时针 → 该轴为**负**，SIGN 用 -1")
    else:
        print("    [!] 转动角度太小（<30°），无法可靠判符号；请把车转满 90° 再试")
    print("\n[!] 提醒：确认后改 ChassisTask.h 的 CHASSIS_IMU_YAW_AXIS/SIGN；")
    print("    CHASSIS_IMU_YAW_EN 先保持 0，改完重烧并按文档 §3.3 验证修正方向。")


if __name__ == "__main__":
    main()
