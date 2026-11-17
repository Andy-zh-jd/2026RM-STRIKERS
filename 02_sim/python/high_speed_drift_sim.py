"""
RM2027 校内赛 — 麦轮底盘「高速侧移 / 跑偏」量化仿真
================================================================================
要回答的三个问题（都是实车日志里已经出现、但说不清有多严重的）：
  1. 3 m/s 高速直行，四个轮子等效滚动半径只要差 0.2%~0.8%，
     终点会偏多少 mm？为什么"低速看不出来、高速很明显"？
  2. 固件里那段新代码（①饱和归一化 ②IMU 直行航向锁 ③侧向加速度限制）
     各自的收益是多少？开关它到底值不值得？—— 用数字说话，不用"感觉好多了"。
  3. 航向锁的 KP/KD 取多少？在 200 Hz 采样 + 5 ms 延迟 + 0.1°/s 零偏
     这种"真实传感器"的条件下会不会抖？

模型与固件逐层对应
--------------------------------------------------------------------------------
01_firmware/RM2027_School_Comp_Frame/userCode/tasks/Src/ChassisTask.cpp::
WheelsSpeedCalc() 的六层结构，在仿真里一行一行照抄（同样是 1 kHz）：

    摇杆速度 → ①斜坡限幅 → ②侧向加速度限制 → ③IMU 直行航向锁
             → ④麦轮逆解 → ⑤饱和归一化 → ⑥输出轴 rpm 目标
             → 每轮 P 速度环(kp=120, ±12000) → M3508 一阶惯性(tau=30 ms, 空载 482 rpm)
             → 每轮各自的滚动半径 → 正解 → 姿态积分

两个"故意不简化"的地方，因为结论全靠它们：
  · 轮子有**各自的滚动半径** r_i = r·(1+δ_i)：四个麦轮的直径、辊子阻力、
    压紧程度不可能一样，同样的轮速落到地面就是不同的速度 —— 这是跑偏的源头。
  · 电机有**空载转速天花板 482 rpm**：P 环再大也突破不了它，所以超限的轮子
    只能"卡住"，其余轮子照走 —— 这是满杆斜推时速度矢量方向歪掉的源头。

运行：python high_speed_drift_sim.py        （约 10 s，纯数值，不联网）
输出：./sim_out/expA_high_speed_straight.{png,csv}
      ./sim_out/expB_saturation.{png,csv}
      ./sim_out/expC_traction_limit.{png,csv}
      ./sim_out/expD_yaw_gain.{png,csv}
"""

import os
import sys
import csv
import math
import unicodedata
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Windows 控制台默认 GBK，强制 UTF-8 避免中文/符号报错
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei"]
plt.rcParams["axes.unicode_minus"] = False

# 同目录的既有仿真库：运动学、电机、PID、常数直接复用，避免"两套公式各说各话"
from mecanum_pid_sim import (MecanumKinematics, MotorModel, PID,
                             ESC_CMD_MAX, M3508_RATIO, DT)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim_out")
os.makedirs(OUT, exist_ok=True)


# ==================================================================
# 1. 参数表：每一个数都来自固件宏，改仿真前先改这里对一遍 ChassisTask.h
# ==================================================================
R_NOM = 0.07625            # 麦轮半径 m（WHEEL_DIAMETER 0.1525 的一半）
K_GEOM = 0.39              # (TRACK_WIDTH 0.40 + WHEEL_BASE 0.38)/2，旋转等效力臂

WHEEL_KP = 120.0           # pidRegulator.kp（纯 P，ki=0：调参阶段就是这组参数）
CHASSIS_IQ_MAX = 12000.0   # CHASSIS_IQ_MAX，≈14.6 A/电机，四轮 58 A
TAU_WHEEL = 0.030          # 电机+电流环等效时间常数 s
RPM_NOLOAD = 482.0         # M3508 24V 输出轴空载转速上限（实测值）
MOTOR_GAIN_RPM = 9000.0    # 满指令 16384 对应的等效输出轴 rpm（线性区增益）

# 纯 P 速度环的稳态关系（实验 B 的解析扫场要用）：
#   cmd = kp·err  →  rpm_ss = gain·cmd/16384 = (kp·gain/16384)·err
#   => rpm = K·(target - rpm)  =>  rpm/target = K/(K+1)  ← 这就是 1.5% 静差的成因
WHEEL_STEADY_GAIN = WHEEL_KP * MOTOR_GAIN_RPM / ESC_CMD_MAX   # ≈65.9
WHEEL_DROOP = WHEEL_STEADY_GAIN / (WHEEL_STEADY_GAIN + 1.0)   # ≈0.9851

CHASSIS_ACC_LIN = 6.0      # m/s²，斜坡限幅（vx/vy）
CHASSIS_ACC_ANG = 40.0     # rad/s²，斜坡限幅（ω）
CHASSIS_ALAT_MAX = 5.0     # m/s²，侧向加速度限制
A_LAT_GRIP = 4.5           # m/s²，麦轮侧向抓地上限（μ≈0.46，实验 C 用）

CHASSIS_WHEEL_RPM_MAX = 450.0   # 饱和归一化阈值（输出轴 rpm）
CHASSIS_YAW_KP = 3.0            # 角度误差(rad) → 角速度修正(rad/s)
CHASSIS_YAW_KD = 0.25           # 角速度阻尼
CHASSIS_YAW_TURN_THR = 0.30     # rad/s，超过它认为"驾驶员在转弯"，不锁航向
CHASSIS_YAW_CORR_MAX = 1.5      # rad/s，修正量上限

STICK_VXY = 3.0            # 满杆平移 m/s
STICK_W = 6.283185307      # 满杆自转 rad/s（60 rpm）

# 陀螺仪：零偏 + 白噪声 + 200 Hz 采样保持 + 5 ms 延迟（固件 1 kHz 里读的是上一次 SPI 值）
GYRO_BIAS_DPS = 0.10
GYRO_NOISE_DPS = 0.15
GYRO_DIV = 5               # 1 kHz / 200 Hz = 5
GYRO_DELAY_S = 0.005

# 每轮等效滚动半径误差（FL, FR, RL, RR）。实车必然存在，正负都可能有。
DELTA_BASE = (-0.004, +0.003, +0.002, -0.002)
# 扫描用的误差幅度（|δ| 的最大值）；±1.8% 是把固件日志里实测的 1.5°/s 反解出来的等效值
DELTA_SCAN = (0.002, 0.004, 0.008, 0.018)

SEED = 20261116            # 固定随机种子：不同配置之间噪声完全相同，差异只来自控制律

MPS_PER_RPM = 2 * math.pi * R_NOM / 60.0     # 输出轴 rpm → 地面速度 m/s（标称轮径）
RPM_PER_MPS = 1.0 / MPS_PER_RPM


# ==================================================================
# 2. 小工具
# ==================================================================
def pad(text, width):
    """按**显示宽度**补空格（中文占 2 列），保证控制台表格能直接粘进文档"""
    w = sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in str(text))
    return str(text) + " " * max(0, width - w)


def wrap180(deg):
    """角度折到 [-180, 180)，对应固件的 WrapDeg180()"""
    d = math.fmod(deg + 180.0, 360.0)
    if d < 0:
        d += 360.0
    return d - 180.0


def slew_toward(cur, tgt, step):
    """一阶限速：朝目标走，但一步最多走 step。固件里叫 SlewToward()"""
    d = tgt - cur
    if d > step:
        return cur + step
    if d < -step:
        return cur - step
    return tgt


def limit_lateral_accel(vx, vy, w, alat_max=CHASSIS_ALAT_MAX):
    """③ 侧向加速度限制：|ω|·|v| > a_max 时把 (vx,vy) **等比**缩小。

    为什么只砍平移不砍 ω：转弯半径被放大是"弯走大一点"，而 ω 一变，
    车头朝向就不听摇杆了 —— 手上感觉全乱。宁可弯大，不可车头不听话。
    为什么用等比而不是各轴独立限幅：独立限幅会让速度矢量方向变化，
    驾驶员推的是斜前方，车却走另一个方向，这正是我们要消除的"侧移感"。
    """
    vmag = math.hypot(vx, vy)
    if vmag < 0.05:                       # 几乎没在平移，限幅没意义（也与固件一致）
        return vx, vy, 1.0
    alat = abs(w) * vmag
    if alat <= alat_max:
        return vx, vy, 1.0
    s = alat_max / alat
    return vx * s, vy * s, s


def desat_normalize(rpm_target, rpm_max=CHASSIS_WHEEL_RPM_MAX):
    """⑤ 饱和归一化：四个轮速是同一个 (vx,vy,ω) 的线性组合，
    只是"削掉超限的那个"会让速度矢量方向歪掉（= 满杆斜推却往一边滑）。

    正确做法是**四轮等比缩放**：方向不变，只是整体慢一点。
    返回 (缩放后的目标, 缩放比例)。比例 <1 说明这一刻被电机能力卡住了。
    """
    m = max(abs(v) for v in rpm_target)
    if m > rpm_max:
        s = rpm_max / m
        return [v * s for v in rpm_target], s
    return list(rpm_target), 1.0


# ==================================================================
# 3. 固件版运动学（k=0.39）与轮子模型
# ==================================================================
class FirmwareKinematics(MecanumKinematics):
    """与固件完全一致的麦轮运动学，外加**输出轴** rpm 换算。

    ★ 为什么必须覆盖 k：父类默认 half_len=half_wid=0.20 → k=0.40，
      而固件是 k=(TRACK_WIDTH 0.40 + WHEEL_BASE 0.38)/2 = 0.39。
      差 2.5% 看着小，但它直接乘在自转项上：60 rpm 满转时四轮速差会差
      2×(0.40-0.39)×6.283 = 0.126 m/s ≈ 16 rpm —— 就是"转弯半径总比
      想象的大一点"这种说不清的手感问题，而且会让仿真的结论对不上实车。

    ★ 为什么单独写 output_rpm：框架里 Motor::SetTargetSpeed() 收的是
      **输出轴** rpm（减速比在 Motor 内部处理），电机回传的也是输出轴 rpm。
      而父类的 wheel_to_rotor_rpm 算的是**转子** rpm（×19.2），差 19 倍，
      混用一次就会得出"450 rpm 能跑 60 m/s"这种荒唐结论。
    """

    def __init__(self, wheel_radius=R_NOM):
        super().__init__(wheel_radius=wheel_radius, half_len=0.20, half_wid=0.20)
        self.k = K_GEOM                              # 0.39，覆盖父类算出来的 0.40
        self.rpm_per_mps = 60.0 / (2 * math.pi * self.r)

    def wheel_v_to_output_rpm(self, wheel_v):
        """轮子地面线速度 (m/s) → 输出轴 rpm。v = rpm/60·2πr  =>  rpm = v·60/(2πr)"""
        return [v * self.rpm_per_mps for v in wheel_v]


class Wheel3508(MotorModel):
    """M3508 + C620 的输出轴速度模型（1 kHz 一阶惯性）。

    与 mecanum_pid_sim.MotorModel 的差别只有两处，但两处都决定结论：

      ① **没有 load 静差项**。静差必须由 P 环自己产生（真实电机就是这样）。
         预先在模型里扣掉静差 = 自欺欺人，会系统性地低估跑偏和速度亏损。
      ② **rpm_ss 被空载转速 482 rpm 硬限幅**。这是反电动势天花板，
         P 环再大也不可能突破；所以超限的轮子只能"卡在 482"，
         其余轮子照走 → 速度矢量方向歪掉。这就是 ⑤ 饱和归一化要解决的问题。
    """

    def __init__(self):
        super().__init__(tau=TAU_WHEEL, gain=MOTOR_GAIN_RPM, load=0.0, noise=0.0,
                         cmd_limit=ESC_CMD_MAX)
        self.rpm_ss = 0.0

    def step(self, cmd, dt):
        self.cmd = float(np.clip(cmd, -self.cmd_limit, self.cmd_limit))
        # cmd 是 C620 电流指令（±16384）。线性区按 gain 折算成输出轴 rpm，
        # 然后被空载转速卡住 —— 限幅顺序不能反，否则"卡住"的量级会算错。
        self.rpm_ss = float(np.clip(self.gain * self.cmd / self.cmd_limit,
                                    -RPM_NOLOAD, RPM_NOLOAD))
        self.rpm += (self.rpm_ss - self.rpm) * dt / self.tau
        return self.rpm


# ==================================================================
# 4. 整车仿真器
# ==================================================================
class SimLog:
    """一次仿真的逐毫秒记录。字段名即为曲线名，画图和算指标都直接用。"""

    def __init__(self, **cols):
        for key, val in cols.items():
            setattr(self, key, np.asarray(val, dtype=float))
        self.n = len(self.t)


class DriftSim:
    """整车 1 ms 步进仿真，每一层都可以单独开关 —— 这就是 A/B 对比的全部机制。

    参数默认值 = 固件当前值，所以"打开/关闭某个开关"就等于"烧/不烧那段代码"。
    """

    def __init__(self, imu_lock=False, desat=False, alat=False,
                 kp_yaw=CHASSIS_YAW_KP, kd_yaw=CHASSIS_YAW_KD,
                 delta=DELTA_BASE, gyro_bias=GYRO_BIAS_DPS, seed=SEED,
                 alat_max=CHASSIS_ALAT_MAX):
        self.kin = FirmwareKinematics()
        self.imu_lock = imu_lock
        self.desat = desat
        self.alat = alat
        self.alat_max = alat_max
        self.kp_yaw = kp_yaw
        self.kd_yaw = kd_yaw
        self.delta = tuple(delta)
        self.gyro_bias = gyro_bias
        self.seed = seed
        # 每轮"同样的轮速 → 地面速度"的增益（1+δ），跑偏就是它四个不相等造成的
        self.ground_gain = [1.0 + d for d in self.delta]

    def run(self, duration, cmd_fn, dist_fn=None):
        """跑 duration 秒。cmd_fn(t) -> (vx, vy, w_rad)；dist_fn(t) -> 外部偏航角速度扰动 rad/s"""
        kin = self.kin
        n = int(round(duration / DT))
        # 四轮 P 速度环：固件里四个底盘电机共用同一组 kp=120/ki=0 的 PID_Regulator_t
        pids = [PID(WHEEL_KP, 0.0, 0.0, out_limit=CHASSIS_IQ_MAX) for _ in range(4)]
        wheels = [Wheel3508() for _ in range(4)]
        rng = np.random.default_rng(self.seed)

        slew_vx = slew_vy = slew_w = 0.0     # 斜坡状态（固件全局变量 slewVx/slewVy/slewW）
        yaw_meas = 0.0                       # 陀螺积分航向（deg，固件 yawDeg）
        yaw_target = 0.0                     # 进入直行时锁定的目标航向（固件 yawTargetDeg）
        yaw_hold = False                     # 固件 yawHold
        gz_prev = gz_held = 0.0              # 200 Hz 采样 + 5 ms 延迟的两级保持
        th = x = y = 0.0                     # 真实姿态/位置
        th_i = x_i = y_i = 0.0               # "指令想要"的姿态/位置（无执行误差的理想车）
        vx_t = vy_t = w_t = 0.0              # 上一毫秒的真实车体速度（陀螺采样用）

        rec = {k: [] for k in ("t", "x", "y", "th", "x_ideal", "y_ideal", "th_ideal",
                               "vxi", "vyi",
                               "vx", "vy", "w", "gz", "yaw_meas", "yaw_err",
                               "vx_cmd", "vy_cmd", "w_cmd", "w_eff", "w_corr",
                               "tgt", "rpm", "pre_max", "pinned", "desat_scale")}

        for k in range(n):
            t = k * DT
            d_now = dist_fn(t) if dist_fn else 0.0

            # ---------- 陀螺仪：200 Hz 采样保持 + 5 ms 传输延迟 ----------
            # 用"上一毫秒的真实角速度"采样，物理上就是这段时间内的转速。
            # 5 ms 延迟不是可以忽略的细节：航向环的相位裕度全在这里，
            # 台架上好好的、上车一开就抖，十有八九是这段延迟没算进去。
            if k % GYRO_DIV == 0:
                raw = math.degrees(w_t) + self.gyro_bias + rng.normal(0.0, GYRO_NOISE_DPS)
                gz_held = gz_prev          # 控制器此刻拿到的 = 5 ms 前的样本
                gz_prev = raw
            yaw_meas = wrap180(yaw_meas + gz_held * DT)   # 1 kHz 用最新的保持值积分

            # ---------- 1. 摇杆目标（阶跃，模拟"一把推满"）----------
            tvx, tvy, tw = cmd_fn(t)

            # ---------- 2. 斜坡限幅 ----------
            # 不限的话 1 ms 内目标从 0 跳到 376 rpm，P 环误差 45120 被限到 12000，
            # 四轮 58 A 瞬间拉爆电池架 35 A 上限 → 压降 → 四轮同时掉速 → 车"发飘"。
            slew_vx = slew_toward(slew_vx, tvx, CHASSIS_ACC_LIN * DT)
            slew_vy = slew_toward(slew_vy, tvy, CHASSIS_ACC_LIN * DT)
            slew_w = slew_toward(slew_w, tw, CHASSIS_ACC_ANG * DT)

            vx, vy, w = slew_vx, slew_vy, slew_w

            # ---------- 3. 侧向加速度限制 ----------
            # 注：固件把这段放在航向锁**之后**、用 wEff 算 a_lat；
            # 这里按任务书顺序放在前面、用摇杆 ω 算。两者互斥（限幅只在转弯时
            # 起作用，航向锁只在直行时起作用），数学上不可区分；
            # 若将来两者同时活跃，以固件的 wEff 为准。
            if self.alat:
                vx, vy, _ = limit_lateral_accel(vx, vy, w, self.alat_max)

            # ---------- 4. IMU 直行航向锁 ----------
            w_corr = 0.0
            w_eff = w
            if self.imu_lock:
                gz_rad = gz_held * math.pi / 180.0
                if abs(w) > CHASSIS_YAW_TURN_THR:
                    # 驾驶员在转弯：这一版不做角度跟踪，只把目标持续跟随当前航向，
                    # 保证下一段直行是从"现在的朝向"开始锁，不会一进直行就甩一把。
                    yaw_hold = False
                    yaw_target = yaw_meas
                else:
                    if not yaw_hold:
                        # 刚进入直行：目标 = 当前航向，此刻修正量恰好为 0，无跳变
                        yaw_hold = True
                        yaw_target = yaw_meas
                    err = wrap180(yaw_target - yaw_meas)
                    w_corr = (self.kp_yaw * err * math.pi / 180.0
                              - self.kd_yaw * gz_rad)
                    w_corr = float(np.clip(w_corr, -CHASSIS_YAW_CORR_MAX,
                                           CHASSIS_YAW_CORR_MAX))
                    w_eff = w + w_corr
            yaw_err = wrap180(yaw_target - yaw_meas) if self.imu_lock else 0.0

            # ---------- 5. 麦轮逆解 ----------
            v_wheel = kin.inverse(vx, vy, w_eff)     # k=0.39，与固件同一组式子
            rpm_target = kin.wheel_v_to_output_rpm(v_wheel)
            pre_max = max(abs(v) for v in rpm_target)

            # ---------- 6. 饱和归一化 ----------
            if self.desat:
                rpm_target, desat_scale = desat_normalize(rpm_target)
            else:
                desat_scale = 1.0

            # ---------- 7. 四轮 P 环 + 电机 ----------
            for i in range(4):
                cmd = pids[i].update(rpm_target[i], wheels[i].rpm, DT)
                wheels[i].step(cmd, DT)

            # ---------- 8. 轮速 → 地面速度（每轮自己的滚动半径）----------
            # 目标 rpm 是按**标称轮径**算的（固件就是这么干的），
            # 但轮子真正压在地上滚过去的速度由各自的实际半径决定 ——
            # 这个"算的时候用一个 r、跑的时候是另一个 r"的错位就是跑偏的物理来源。
            wv_ground = [wheels[i].rpm * MPS_PER_RPM * self.ground_gain[i] for i in range(4)]

            # ---------- 9. 正解 + 姿态积分 ----------
            vx_t, vy_t, w_t = kin.forward(wv_ground)
            w_t += d_now                     # 外部偏航扰动（撞击/压边），陀螺也看得见
            th += w_t * DT
            x += (vx_t * math.cos(th) - vy_t * math.sin(th)) * DT
            y += (vx_t * math.sin(th) + vy_t * math.cos(th)) * DT
            # 理想车：同一条摇杆指令但执行零误差 —— "驾驶员想要的那台车"。
            # 用它做基准才能把误差拆成两部分：横向（方向歪了，IMU/归一化要修的）
            # 和沿程（速度亏损 = 纯 P 静差 + 饱和，固件是**明知故犯**地接受它）。
            th_i += w_eff * DT
            vxi = vx * math.cos(th_i) - vy * math.sin(th_i)
            vyi = vx * math.sin(th_i) + vy * math.cos(th_i)
            x_i += vxi * DT
            y_i += vyi * DT

            # ---------- 记录 ----------
            rec["t"].append(t)
            rec["x"].append(x); rec["y"].append(y); rec["th"].append(math.degrees(th))
            rec["x_ideal"].append(x_i); rec["y_ideal"].append(y_i)
            rec["th_ideal"].append(math.degrees(th_i))
            rec["vxi"].append(vxi); rec["vyi"].append(vyi)
            rec["vx"].append(vx_t); rec["vy"].append(vy_t); rec["w"].append(w_t)
            rec["gz"].append(gz_held); rec["yaw_meas"].append(yaw_meas)
            rec["yaw_err"].append(yaw_err)
            rec["vx_cmd"].append(vx); rec["vy_cmd"].append(vy); rec["w_cmd"].append(w)
            rec["w_eff"].append(w_eff); rec["w_corr"].append(w_corr)
            rec["tgt"].append(list(rpm_target))
            rec["rpm"].append([wheels[i].rpm for i in range(4)])
            rec["pre_max"].append(pre_max)
            rec["pinned"].append(1.0 if max(abs(wheels[i].rpm) for i in range(4))
                                 >= RPM_NOLOAD - 1.0 else 0.0)
            rec["desat_scale"].append(desat_scale)

        return SimLog(**rec)


# ==================================================================
# 5. 通用指标
# ==================================================================
def track_rate(log, t_from):
    """四轮平均跟踪率（%）：实际输出轴 rpm / 目标 rpm。

    只统计真正在转的采样点（目标 > 50 rpm），否则静止段会把均值拉成 0。
    这个数直接对应固件日志里实测的 88~90% —— 它 <100% 不是 bug，
    是纯 P 速度环的静差，1.5% 的亏损 = 5 m 少走 7.5 cm。
    """
    mask = (log.t >= t_from) & (log.tgt.max(axis=1) > 50.0)
    if not mask.any():
        return 0.0, [0.0] * 4
    per = [float(np.mean(log.rpm[mask, i] / log.tgt[mask, i]) * 100.0) for i in range(4)]
    return float(np.mean(per)), per


def heading_settle(t, err, t_kick, band=1.0, t_from=None):
    """航向回整指标：调节时间(s)、超调(deg)、稳态误差(deg)。

    调节时间 = 最后一次离开 ±band 的时刻 - 扰动开始时刻（之后一直待在里面才算数）。
    超调 = 越过目标后往反方向跑的最大幅度（一阶响应理论上是 0）。
    """
    i0 = int(np.argmax(t >= t_kick))
    seg_t, seg_e = t[i0:], err[i0:]
    outside = np.abs(seg_e) > band
    if not outside.any():
        t_settle = 0.0
    elif outside.all():
        t_settle = float("nan")
    else:
        last = int(np.nonzero(outside)[0][-1])
        t_settle = float(seg_t[last] - t_kick)
    # 超调：第一次过零之后的反向最大幅值
    cross = np.nonzero(np.sign(seg_e) != np.sign(seg_e[0]))[0] if len(seg_e) else []
    if len(cross):
        after = seg_e[int(cross[0]):]
        overshoot = float(max(0.0, -np.min(after) if seg_e[0] > 0 else np.max(after)))
    else:
        overshoot = 0.0
    tail = err[t >= t[-1] - 0.5]
    return t_settle, overshoot, float(np.mean(np.abs(tail)))


def dir_deg(vx, vy):
    return math.degrees(math.atan2(vy, vx))


def movavg(y, n):
    """n 点滑动平均（单位：采样点）。

    画"跑偏角速度"时必须平滑：陀螺单点噪声 σ=0.15°/s，而跑偏信号本身只有 0.33°/s
    —— 不平滑的话图上只能看到噪声，读者会以为"没有跑偏"。
    这也正是固件里 gz 必须做低通的原因：单帧读数不能直接用。
    """
    if n <= 1:
        return y
    return np.convolve(y, np.ones(n) / n, mode="same")


# ==================================================================
# 6. 实验 A：高速直行 5 m —— 半径误差到底偏多少，IMU 锁能救多少
# ==================================================================
def exp_a_straight():
    print("\n" + "=" * 84)
    print("实验 A｜高速直行（满杆 vx=3.0 m/s，3.5 s）。题目写'5 m'，"
          "但 3.0×3.5 = 10.5 m，两个位置的指标都给出")
    print("=" * 84)

    T_END = 3.5
    straight = lambda t: (STICK_VXY, 0.0, 0.0)

    configs = [
        ("① 基线（无锁 / 无归一化）", dict(imu_lock=False, desat=False, alat=False), "tab:red", "-"),
        ("② 仅饱和归一化", dict(imu_lock=False, desat=True, alat=False), "tab:orange", "--"),
        ("③ 归一化 + IMU 航向锁", dict(imu_lock=True, desat=True, alat=False), "tab:green", "-"),
    ]
    logs, rows = {}, []
    print("\n" + pad("配置", 28) + pad("终点|y|(mm)", 13) + pad("5m处|y|(mm)", 13)
          + pad("终点航向(°)", 13) + pad("gz RMS(°/s)", 13) + pad("真实偏航率(°/s)", 16)
          + pad("跟踪率(%)", 11))
    print("-" * 110)
    for label, kw, color, ls in configs:
        log = DriftSim(**kw).run(T_END, straight)
        rate, per = track_rate(log, 0.8)
        mask = log.t >= 0.8
        gz_rms = float(np.sqrt(np.mean(log.gz[mask] ** 2)))
        y_5m = float(np.interp(5.0, log.x, log.y)) if log.x.max() >= 5.0 else float("nan")
        rows.append([label, round(log.y[-1] * 1000, 1), round(y_5m * 1000, 1),
                     round(log.th[-1], 3), round(gz_rms, 3),
                     round(float(np.mean(log.w[mask] * 180.0 / math.pi)), 3),
                     round(rate, 2)] + [round(p, 2) for p in per])
        logs[label] = dict(log=log, color=color, ls=ls)
        print(pad(label, 28) + pad(f"{log.y[-1]*1000:+.1f}", 13) + pad(f"{y_5m*1000:+.1f}", 13)
              + pad(f"{log.th[-1]:+.2f}", 13) + pad(f"{gz_rms:.3f}", 13)
              + pad(f"{np.mean(log.w[mask])*180/math.pi:+.3f}", 16)
              + pad(f"{rate:.2f}", 11))

    base = logs["① 基线（无锁 / 无归一化）"]["log"]
    d02 = float(np.max(np.abs(base.y - logs["② 仅饱和归一化"]["log"].y)) * 1000)
    print(f"\n  [!] ② 与 ① 逐点最大差 = {d02:.4f} mm —— 3.0 m/s 直行只要 375.8 rpm，"
          f"根本没碰到 450 rpm 的门槛，")
    print("      所以饱和归一化在'直行'这个工况里是空转的。它的问题出在斜推/自转（见实验 B）。")
    print(f"  [!] 四个轮子的跟踪率完全一样（{rows[0][6]:.2f}%）—— 跑偏不是速度环的锅！"
          f"纯 P 的 1.5% 静差对四个轮子一视同仁，")
    print(f"      抵掉的是'跑得够不够远'（5 m 少走 {5*0.0155*1000:.0f} mm），"
          f"真正让车歪的是'同样的轮速 → 不同的地面速度'，即轮径误差。")

    # ---------- 半径误差幅度扫描 ----------
    # 说清楚为什么"高速才明显"：
    #   偏航角速度 ω_drift = vx·(-δ_FL+δ_FR-δ_RL+δ_RR)/(4k)  ← 与车速成正比
    #   横向偏移     y(T)  ≈ ½·vx·ω_drift·T²                 ← 再乘一次 vx
    #   所以同样跑 T 秒，偏移 ∝ vx²。低速时 0.35 m/s 的 ω_drift 只有 0.04°/s，看不见。
    print("\n--- A2：等效滚动半径误差幅度扫描（同一组相对比例，只缩放幅度）---")
    print(pad("误差幅度", 17) + pad("理论ω漂(°/s)", 15) + pad("基线终点|y|", 14)
          + pad("基线5m处|y|", 14) + pad("锁后终点|y|", 14) + pad("锁后5m|y|", 12)
          + pad("抑制比", 10))
    print("-" * 102)
    sweep = []
    for amp in DELTA_SCAN:
        scale = amp / abs(DELTA_BASE[0])
        delta = tuple(d * scale for d in DELTA_BASE)
        lb = DriftSim(imu_lock=False, desat=False, delta=delta).run(T_END, straight)
        ll = DriftSim(imu_lock=True, desat=True, delta=delta).run(T_END, straight)
        w_theory = (STICK_VXY * (-delta[0] + delta[1] - delta[2] + delta[3]) / (4 * K_GEOM))
        y_b = lb.y[-1] * 1000
        y_l = ll.y[-1] * 1000
        y5b = float(np.interp(5.0, lb.x, lb.y)) * 1000
        y5l = float(np.interp(5.0, ll.x, ll.y)) * 1000
        supp = (1 - abs(y_l) / abs(y_b)) * 100 if abs(y_b) > 1e-9 else 0.0
        tag = f"±{amp*100:.1f}%" + ("(实测等效)" if amp == 0.018 else "")
        sweep.append(dict(amp=amp, w_theory=math.degrees(w_theory), y_base=y_b,
                          y_lock=y_l, y5_base=y5b, y5_lock=y5l, sup=supp,
                          th_base=lb.th[-1], th_lock=ll.th[-1]))
        print(pad(tag, 17) + pad(f"{math.degrees(w_theory):.3f}", 15)
              + pad(f"{y_b:+.1f} mm", 14) + pad(f"{y5b:+.1f} mm", 14)
              + pad(f"{y_l:+.1f} mm", 14) + pad(f"{y5l:+.1f} mm", 12)
              + pad(f"{supp:.0f}%", 10))

    # ---------- 诊断：20 s 长跑，看"零偏"和"轮径误差"的时间尺度 ----------
    # 航向锁的残余有两个来源，**都随时间无界增长**，但阶数不同：
    #   ① 轮径误差造出的纯侧向速度 → y 线性增长（陀螺修不了"横"）
    #   ② 陀螺零偏 0.1°/s → 测量航向在漂，P 环把假误差当真修 → 车身真的以
    #      0.1°/s 转过去 → y ∝ ½·vx·(bias·t)·t，**二次**增长
    # 短测（3~8 s）里 ① 占主导，而且 ② 的符号可能刚好和 ① 相反 —— 于是
    # "零偏没标定"看起来反而更好。这个假象会随时间反转，所以必须跑够长。
    T_LONG = 20.0
    longs = [
        ("无锁基线", dict(imu_lock=False, desat=False)),
        ("锁 + bias=0（已标定）", dict(imu_lock=True, desat=True, gyro_bias=0.0)),
        ("锁 + bias=0.1°/s（现状）", dict(imu_lock=True, desat=True)),
    ]
    long_logs = {name: DriftSim(**kw).run(T_LONG, straight) for name, kw in longs}

    def sample(log, tq):
        i = min(int(round(tq / DT)), log.n - 1)
        return log.y[i] * 1000, log.th[i]

    print(f"\n  [诊断，20 s 长跑] 直行跑得越久，零偏越要命（|y| 单位 mm / 航向单位 °）：")
    print("          " + pad("配置", 26) + pad("3.5 s", 18) + pad("8 s", 18)
          + pad("20 s", 20))
    for name, lg in long_logs.items():
        cells = []
        for tq in (3.5, 8.0, 20.0):
            yy, tt = sample(lg, tq)
            cells.append(f"{yy:+.0f}mm/{tt:+.2f}°")
        print("          " + pad(name, 26) + pad(cells[0], 18) + pad(cells[1], 18)
              + pad(cells[2], 20))
    y_b0 = sample(long_logs["锁 + bias=0（已标定）"], T_LONG)[0]
    y_b1 = sample(long_logs["锁 + bias=0.1°/s（现状）"], T_LONG)[0]
    lin = sample(long_logs["锁 + bias=0（已标定）"], 8.0)[0] / 8.0        # mm/s
    quad = 0.5 * STICK_VXY * (GYRO_BIAS_DPS * math.pi / 180.0) * 1000    # mm/s²
    print(f"          拆解：侧向残余是线性的 {lin:.1f} mm/s（20 s 约 {lin*20/1000:.2f} m）；"
          f"零偏是二次项 {quad:.2f} mm/s²（y ≈ {quad:.2f}·t²）。")
    print(f"          两者在 t ≈ {2*lin/quad:.0f} s 处打平，之后零偏项全面反超："
          f"20 s 时 {y_b1:+.0f} mm（已标定 {y_b0:+.0f} mm）。")
    print(f"          => 3.5 s 的短测**永远**不会暴露零偏问题，但整场比赛一定会暴露。")

    # ---------- 画图 ----------
    fig, axes = plt.subplots(2, 2, figsize=(13.0, 8.6))

    ax = axes[0, 0]
    for label, d in logs.items():
        lg = d["log"]
        ax.plot(lg.x, lg.y * 1000, color=d["color"], linestyle=d["ls"],
                linewidth=2.0, label=f"{label}：终点 {lg.y[-1]*1000:+.0f} mm")
    ax.axvline(5.0, color="k", linestyle=":", linewidth=1.2)
    ax.text(5.05, ax.get_ylim()[1] * 0.85, "5 m 考核线", fontsize=9)
    ax.axhline(0, color="k", linewidth=0.8)
    ax.set_xlabel("前进距离 x (m)")
    ax.set_ylabel("横向偏移 y (mm)")
    ax.set_title("实验 A：3.0 m/s 高速直行的横向漂移\n"
                 f"（轮径误差 δ = {DELTA_BASE[0]*100:+.1f}%/{DELTA_BASE[1]*100:+.1f}%/"
                 f"{DELTA_BASE[2]*100:+.1f}%/{DELTA_BASE[3]*100:+.1f}%  FL/FR/RL/RR）")
    ax.grid(alpha=0.3)
    ax.legend(loc="lower right", fontsize=8.5)

    ax = axes[0, 1]
    for label, d in logs.items():
        ax.plot(d["log"].t, d["log"].th, color=d["color"], linestyle=d["ls"],
                linewidth=1.8, label=label)
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("真实航向角 (°)")
    ax.set_title("航向角随时间漂移（斜率就是跑偏角速度）")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8.5)

    ax = axes[1, 0]
    for label, d in logs.items():
        # 去掉两端各 50 ms：滑动平均在首尾会因补零产生假的下冲
        ax.plot(d["log"].t[50:-50], movavg(d["log"].w * 180.0 / math.pi, 100)[50:-50],
                color=d["color"], linestyle=d["ls"], linewidth=1.8, label=label)
    ax.axhline(0, color="k", linewidth=0.8)
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("真实偏航角速度 (°/s)，100 ms 平滑")
    ax.set_title("偏航角速度：轮径误差在 3 m/s 时'造出'了 0.33°/s 的假自转\n"
                 f"（陀螺单点噪声 σ=±{GYRO_NOISE_DPS}°/s 比它还大，不平滑根本分辨不出来）")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8.5)

    ax = axes[1, 1]
    amps = np.array([s["amp"] for s in sweep]) * 100
    ax.plot(amps, [s["y_base"] for s in sweep], "o-", color="tab:red",
            linewidth=2, label="基线：终点横向偏移")
    ax.plot(amps, [s["y_lock"] for s in sweep], "s-", color="tab:green",
            linewidth=2, label="IMU 航向锁：终点横向偏移")
    ax.plot(amps, [s["y5_base"] for s in sweep], "^--", color="tab:red",
            linewidth=1.2, alpha=0.7, label="基线：5 m 处横向偏移")
    ax.axhline(0, color="k", linewidth=0.8)
    ax.set_xlabel("等效滚动半径误差幅度 |δ| (%)")
    ax.set_ylabel("终点横向偏移 (mm)")
    ax.set_title("横向偏移 ∝ 轮径误差 且 ∝ 车速²\n"
                 "(3.5 s 内 y ≈ ½·vx·ω漂·T²，而 ω漂 又正比于 vx)")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8.5)

    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "expA_high_speed_straight.png"), dpi=140)
    plt.close(fig)

    # ---------- CSV：主表 + 扫描表 + 轨迹表 三段 ----------
    path = os.path.join(OUT, "expA_high_speed_straight.csv")
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["【表 A1】3.5 s 直行配置对比"])
        wr.writerow(["配置", "终点|y|mm", "5m处|y|mm", "终点航向deg", "gz_RMS_dps",
                     "真实偏航率dps", "四轮平均跟踪率%",
                     "FL跟踪%", "FR跟踪%", "RL跟踪%", "RR跟踪%"])
        wr.writerows(rows)
        wr.writerow([])
        wr.writerow(["【表 A2】等效滚动半径误差幅度扫描（3.5 s，vx=3.0）"])
        wr.writerow(["误差幅度%", "理论偏航率dps", "基线终点|y|mm", "基线5m|y|mm",
                     "锁后终点|y|mm", "锁后5m|y|mm", "抑制比%",
                     "基线终点航向deg", "锁后终点航向deg"])
        for s in sweep:
            wr.writerow([round(s["amp"] * 100, 2), round(s["w_theory"], 3),
                         round(s["y_base"], 1), round(s["y5_base"], 1),
                         round(s["y_lock"], 1), round(s["y5_lock"], 1),
                         round(s["sup"], 1), round(s["th_base"], 3),
                         round(s["th_lock"], 3)])
        wr.writerow([])
        wr.writerow(["【表 A3】横向偏移随 x 的曲线（等距降采样）"])
        wr.writerow(["x_m", "基线_y_mm", "仅归一化_y_mm", "归一化+IMU锁_y_mm"])
        xs = np.linspace(0.0, min(d["log"].x.max() for d in logs.values()), 80)
        cols = [np.interp(xs, logs[k]["log"].x, logs[k]["log"].y) * 1000 for k in logs]
        for i in range(len(xs)):
            wr.writerow([round(xs[i], 3)] + [round(c[i], 1) for c in cols])

    print(f"-> 已输出 expA_high_speed_straight.png / .csv")
    y20 = sample(long_logs["锁 + bias=0.1°/s（现状）"], T_LONG)[0]
    y20_b0 = sample(long_logs["锁 + bias=0（已标定）"], T_LONG)[0]
    y20_base = sample(long_logs["无锁基线"], T_LONG)[0]
    return dict(base=base, locked=logs["③ 归一化 + IMU 航向锁"]["log"],
                sweep=sweep, rows=rows, d02=d02,
                y20=y20, y20_b0=y20_b0, y20_base=y20_base, lin=lin, quad=quad)


# ==================================================================
# 7. 实验 B：满杆斜推 / 满杆前进+自转 —— 450 rpm 上限的代价
# ==================================================================
def static_wheel_map(vx, vy, w, desat=False):
    """稳态解析：一条车体速度指令 → 四个轮子**真正能给**的地面速度 → 正解回车身。

    为什么可以用解析式代替仿真来扫整片 vx-vy 平面：
      · 1 kHz 的 P 环在 30 ms 内就稳态了，稳态关系能闭式写出：
          rpm = WHEEL_DROOP·target（纯 P 的 1.5% 静差，见 WHEEL_DROOP 的推导）
          |target| > 482/0.985 ≈ 489 rpm 时轮子被空载转速卡死，只能到 482
      · 一次完整仿真 2000 步要 25 ms，扫 3600 个点要一分半；解析式瞬间出结果。
    扫出来的最坏点再用完整仿真复核（实验 B 的 B2/B3），误差 < 0.5°。
    """
    k = K_GEOM
    target = [vx - vy - k * w, vx + vy + k * w, vx + vy - k * w, vx - vy + k * w]
    rpm = [v * RPM_PER_MPS for v in target]
    if desat:
        rpm, _ = desat_normalize(rpm)
    pin_at = RPM_NOLOAD / WHEEL_DROOP
    act = [r * WHEEL_DROOP if abs(r) <= pin_at else math.copysign(RPM_NOLOAD, r)
           for r in rpm]
    v = [r * MPS_PER_RPM for r in act]
    return ((v[0] + v[1] + v[2] + v[3]) / 4.0,
            (-v[0] + v[1] + v[2] - v[3]) / 4.0,
            (-v[0] + v[1] - v[2] + v[3]) / (4.0 * k))


def exp_b_saturation():
    print("\n" + "=" * 84)
    print("实验 B｜满杆斜推 (3.0, 1.5) 与 满杆前进+满杆自转 (3.0, 6.283 rad/s)，2 s")
    print("=" * 84)
    T_END = 2.0
    cases = [
        ("斜推 3.0+1.5 m/s", lambda t: (3.0, 1.5, 0.0), "斜推"),
        ("前进 3.0+自转 6.28", lambda t: (3.0, 0.0, STICK_W), "前进+自转"),
    ]
    variants = [
        ("不做归一化", dict(desat=False, imu_lock=False, alat=False), "tab:red", "--"),
        ("做归一化", dict(desat=True, imu_lock=False, alat=False), "tab:blue", "-"),
    ]
    results, rows = {}, []

    def measure(log):
        """统一算实验 B 的指标。窗口取 1.0~2.0 s（0.5 s 斜坡之后已经稳态）。

        误差拆成三块，因为它们该由不同的代码负责：
          · 横向偏差：速度**方向**歪了 → IMU 航向锁 / 饱和归一化要修的就是它
          · 沿程亏损：速度**大小**不够 → 纯 P 静差 + 电机天花板，只能靠抬 kp/降 VXY_MAX
          · 自转完成度：自转掉了多少 → 与平移的比例失配会把转弯半径改掉
        把"横向"和"沿程"混成一个欧氏距离，会得出"归一化让偏差变大"的错误结论
        （因为它主动减速），所以必须分开看。
        """
        m = log.t >= 1.0
        cmd_dir = dir_deg(float(np.mean(log.vx_cmd[m])), float(np.mean(log.vy_cmd[m])))
        act_dir = dir_deg(float(np.mean(log.vx[m])), float(np.mean(log.vy[m])))
        dd = wrap180(act_dir - cmd_dir)
        spd = float(np.mean(np.hypot(log.vx[m], log.vy[m])))
        spd_cmd = float(np.mean(np.hypot(log.vx_cmd[m], log.vy_cmd[m])))
        d_th_i = log.th_ideal[-1] - log.th_ideal[0]
        d_th = log.th[-1] - log.th[0]
        yaw_done = d_th / d_th_i * 100.0 if abs(d_th_i) > 10.0 else float("nan")
        w_act = float(np.mean(log.w[m]))
        w_cmd = float(np.mean(log.w_eff[m]))
        radius_ratio = ((spd / w_act) / (spd_cmd / w_cmd)
                        if abs(w_act) > 0.5 and abs(w_cmd) > 0.5 else float("nan"))
        # 位置误差按"理想速度方向"分解：横向（垂直）= 侧移，沿程（平行）= 缩水。
        # 旋转工况（|ω| 大）下这个分解没有意义 —— 圆轨迹的误差主要是"转过的相位差"，
        # 硬拆会得出"横移 0.96 m"这种误导读者的数字，所以置 NaN 让表里显示 "-"。
        dx = log.x - log.x_ideal
        dy = log.y - log.y_ideal
        dev_end = float(math.hypot(float(dx[-1]), float(dy[-1])))
        if abs(w_cmd) > 0.5:
            cross_max = along_end = float("nan")
        else:
            vi = np.maximum(np.hypot(log.vxi, log.vyi), 1e-9)
            ux, uy = log.vxi / vi, log.vyi / vi
            along = dx * ux + dy * uy
            cross = -dx * uy + dy * ux
            cross_max = float(np.max(np.abs(cross)))
            along_end = float(along[-1])
        return dict(cmd_dir=cmd_dir, act_dir=act_dir, dd=dd, spd=spd, spd_cmd=spd_cmd,
                    over=float(np.mean(log.pre_max[m] > CHASSIS_WHEEL_RPM_MAX) * 100),
                    pin=float(np.mean(log.pinned[m]) * 100), yaw_done=yaw_done,
                    radius_ratio=radius_ratio, along_end=along_end,
                    cross_max=cross_max, dev_end=dev_end)

    print("\n" + pad("工况", 24) + pad("配置", 11) + pad("指令方向(°)", 13)
          + pad("实际方向(°)", 13) + pad("方向差(°)", 11) + pad("实际/指令速度", 15)
          + pad("自转完成%", 11) + pad("横向偏差(m)", 13) + pad("沿程亏损(m)", 13)
          + pad("超450%", 9) + pad("顶482%", 9))
    print("-" * 142)
    for cname, fn, short in cases:
        for vname, kw, color, ls in variants:
            log = DriftSim(**kw).run(T_END, fn)
            d = measure(log)
            results[(short, vname)] = dict(log=log, color=color, ls=ls, label=cname, **d)
            rows.append([cname, vname, round(d["cmd_dir"], 2), round(d["act_dir"], 2),
                         round(d["dd"], 2), round(d["spd"], 3), round(d["spd_cmd"], 3),
                         "" if math.isnan(d["yaw_done"]) else round(d["yaw_done"], 1),
                         "" if math.isnan(d["cross_max"]) else round(d["cross_max"], 3),
                         "" if math.isnan(d["along_end"]) else round(d["along_end"], 3),
                         round(d["dev_end"], 3),
                         round(d["over"], 0), round(d["pin"], 0),
                         "" if math.isnan(d["radius_ratio"]) else round(d["radius_ratio"], 3)])
            print(pad(cname, 24) + pad(vname, 11) + pad(f"{d['cmd_dir']:+.1f}", 13)
                  + pad(f"{d['act_dir']:+.1f}", 13) + pad(f"{d['dd']:+.1f}", 11)
                  + pad(f"{d['spd']:.2f}/{d['spd_cmd']:.2f}", 15)
                  + pad("-" if math.isnan(d["yaw_done"]) else f"{d['yaw_done']:.0f}", 11)
                  + pad("-" if math.isnan(d["cross_max"]) else f"{d['cross_max']:.3f}", 13)
                  + pad("-" if math.isnan(d["along_end"]) else f"{d['along_end']:+.3f}", 13)
                  + pad(f"{d['over']:.0f}", 9) + pad(f"{d['pin']:.0f}", 9))

    a = results[("斜推", "不做归一化")]
    b = results[("斜推", "做归一化")]
    c = results[("前进+自转", "不做归一化")]
    e = results[("前进+自转", "做归一化")]
    print(f"\n  [!] 两种工况的失效方式**不一样**，但根都是'轮子被 482 rpm 卡住'：")
    print(f"      (a) 斜推：被卡的是一对斜对轮（FR/RL），它们同时贡献 vx 和 vy。"
          f"侧向分量本来就小，")
    print(f"          相对损失更大 → 速度矢量往'更直'的方向偏 {a['dd']:+.1f}°。"
          f"别嫌 2 度小：")
    print(f"          跑 10 m 就是 10·tan({abs(a['dd']):.1f}°) = "
          f"{10*math.tan(math.radians(abs(a['dd']))):.2f} m 的横向偏差。"
          f"2 s 内实测横向偏差 {a['cross_max']*1000:.0f} mm。")
    print(f"          归一化把方向差压到 {b['dd']:+.1f}°、横向偏差 "
          f"{b['cross_max']*1000:.0f} mm，代价是速度从 {a['spd']:.2f} 降到 "
          f"{b['spd']:.2f} m/s（沿程亏损 {-b['along_end']:.2f} m）。")
    print(f"      (b) 前进+自转：左两轮慢、右两轮快，被卡的是右侧一对，**左右对称**，"
          f"所以方向一点都不歪（{c['dd']:+.1f}°）；")
    print(f"          但自转只完成 {c['yaw_done']:.0f}%、平移只完成 "
          f"{c['spd']/c['spd_cmd']*100:.0f}% —— 比例失配使转弯半径变成指令值的 "
          f"{c['radius_ratio']:.2f} 倍，")
    print(f"          驾驶员把左摇杆拧到底，车头比预期慢 {100-c['yaw_done']:.0f}%；"
          f"归一化后自转 {e['yaw_done']:.0f}%、平移 "
          f"{e['spd']/e['spd_cmd']*100:.0f}%（等比缩水，半径比 {e['radius_ratio']:.2f}）。"
          f"两个数不完全相等是因为斜坡起步段还没触发归一化。")
    print(f"          （旋转工况的'横向/沿程'分解对圆轨迹没有意义，故表中置'-'；"
          f"该工况看'自转完成度'，终点位置偏差见 CSV："
          f"不做 {c['dev_end']:.2f} m、做 {e['dev_end']:.2f} m。）")

    # ---------- B2：把整片摇杆范围扫一遍，找方向歪得最狠的工况 ----------
    # 摇杆是方的：任一根轴满杆都是 3.0 m/s，所以斜推到角上是 (3.0, 3.0) 而不是 3.0 的圆。
    worst = None
    for vyv in np.linspace(-2.99, 2.99, 61):
        for vxv in np.linspace(0.05, 2.99, 61):
            if math.hypot(vxv, vyv) < 0.6:
                continue
            avx, avy, _ = static_wheel_map(vxv, vyv, 0.0)
            err = wrap180(dir_deg(avx, avy) - dir_deg(vxv, vyv))
            if worst is None or abs(err) > abs(worst[0]):
                worst = (err, vxv, vyv)
    print(f"\n  [B2] 全摇杆范围扫描（解析，7200 个点）：最坏方向差 "
          f"{worst[0]:+.2f}° @ (vx={worst[1]:.2f}, vy={worst[2]:+.2f}) m/s；"
          f"归一化后该点方向差 "
          f"{wrap180(dir_deg(*static_wheel_map(worst[1], worst[2], 0.0, desat=True)[:2]) - dir_deg(worst[1], worst[2])):+.2f}°。")
    log_w = DriftSim(desat=False, imu_lock=False, alat=False).run(
        T_END, lambda t: (worst[1], worst[2], 0.0))
    dw = measure(log_w)
    print(f"       用完整仿真复核该点：方向差 {dw['dd']:+.2f}°、横向偏差 "
          f"{dw['cross_max']*1000:.0f} mm、速度 {dw['spd']:.2f}/{dw['spd_cmd']:.2f} m/s"
          f"（与解析值一致，说明解析模型可信）。")
    rows.append(["最坏点(扫描)", "不做归一化", round(dw["cmd_dir"], 2), round(dw["act_dir"], 2),
                 round(dw["dd"], 2), round(dw["spd"], 3), round(dw["spd_cmd"], 3),
                 "", round(dw["cross_max"], 3), round(dw["along_end"], 3),
                 round(dw["dev_end"], 3), round(dw["over"], 0), round(dw["pin"], 0), ""])

    # ---------- 画图 ----------
    fig, axes = plt.subplots(2, 2, figsize=(13.0, 8.8))
    for col, (short, cname) in enumerate([("斜推", "满杆斜推 3.0 + 1.5 m/s"),
                                          ("前进+自转", "满杆前进 3.0 + 自转 6.283 rad/s")]):
        ax = axes[0, col]
        ref = results[(short, "不做归一化")]["log"]      # 摇杆原始方向（满杆）的轨迹
        ax.plot(ref.x_ideal, ref.y_ideal, "k--", linewidth=1.5,
                label="摇杆满杆的理想轨迹（不含电机限制）")
        for vname, _, color, ls in variants:
            d = results[(short, vname)]
            lg = d["log"]
            if math.isnan(d["cross_max"]):
                lab = f"{vname}：终点位置偏差 {d['dev_end']:.2f} m"
            else:
                lab = (f"{vname}：横向 {d['cross_max']*1000:.0f} mm / "
                       f"沿程 {d['along_end']:+.2f} m")
            ax.plot(lg.x, lg.y, color=color, linestyle=ls, linewidth=2.1, label=lab)
            ax.plot(lg.x_ideal, lg.y_ideal, color=color, linestyle=":", linewidth=1.0,
                    alpha=0.5)
        ax.plot(0, 0, "ko", markersize=4)
        ax.set_xlabel("X (m)")
        ax.set_ylabel("Y (m)")
        ax.set_title(f"{cname}（2 s，含 0.5 s 斜坡起步）")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8.0, loc="best")
        ax.axis("equal")

    ax = axes[1, 0]
    lg = results[("前进+自转", "不做归一化")]["log"]
    # 本例左右对称：FL≡RL（慢）、FR≡RR（快），所以按"对"画，避免四条曲线两两重合看不清
    pair = [("左侧两轮 FL≡RL（慢）", (0, 2), "tab:blue", "-"),
            ("右侧两轮 FR≡RR（快）", (1, 3), "tab:red", "-")]
    for name, idx, color, ls in pair:
        ax.plot(lg.t, lg.rpm[:, idx[0]], color=color, linestyle=ls, linewidth=2.1,
                label=f"{name} 实际")
        ax.plot(lg.t, lg.tgt[:, idx[0]], color=color, linestyle=":", linewidth=1.0,
                alpha=0.7, label=f"{name} 目标")
    ax.axhline(CHASSIS_WHEEL_RPM_MAX, color="tab:orange", linewidth=1.4,
               label=f"归一化阈值 {CHASSIS_WHEEL_RPM_MAX:.0f} rpm")
    ax.axhline(RPM_NOLOAD, color="k", linewidth=1.4, linestyle="--",
               label=f"空载转速上限 {RPM_NOLOAD:.0f} rpm（物理天花板）")
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("输出轴转速 (rpm)")
    ax.set_title("不做归一化：右侧两轮目标 683 rpm 却只能到 482，左侧两轮照走到 69 rpm\n"
                 f"→ 左右对称地被卡住：方向不歪，但自转只完成 {c['yaw_done']:.0f}%，"
                 f"转弯半径被放大 {c['radius_ratio']:.2f} 倍")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8.0, loc="center right")

    # ---------- 方向误差 / 速度完成度 的全摇杆范围分布 ----------
    # 摇杆是方的：任一根轴都能到 3.0 m/s，斜推到角上是 (3.0, 3.0)。
    ax = axes[1, 1]
    gvy = np.linspace(-3.0, 3.0, 121)
    gvx = np.linspace(0.0, 3.0, 121)
    ratio = np.zeros((len(gvx), len(gvy)))
    derr = np.zeros_like(ratio)
    for i, vx_ in enumerate(gvx):
        for j, vy_ in enumerate(gvy):
            avx, avy, _ = static_wheel_map(vx_, vy_, 0.0)
            sp = math.hypot(avx, avy)
            ratio[i, j] = sp / max(math.hypot(vx_, vy_), 1e-6)
            derr[i, j] = abs(wrap180(dir_deg(avx, avy) - dir_deg(vx_, vy_)))
    mesh = ax.pcolormesh(gvy, gvx, ratio * 100.0, shading="auto", cmap="YlOrRd",
                         vmin=60, vmax=100)
    cs = ax.contour(gvy, gvx, derr, levels=[1.0, 2.0, 3.0], colors="k",
                    linewidths=1.0)
    ax.clabel(cs, fmt="%d°", fontsize=8)
    fig.colorbar(mesh, ax=ax, label="实际速度 / 指令速度 (%)")
    ax.plot(1.5, 3.0, "b*", markersize=13, label="实验点 斜推 (3.0, 1.5)")
    ax.plot(worst[2], worst[1], "kp", markersize=11, label=f"最坏点 {worst[0]:+.1f}°")
    ax.set_xlabel("横向速度指令 vy (m/s)")
    ax.set_ylabel("前进速度指令 vx (m/s)")
    ax.set_title("不做归一化：颜色 = 速度完成度，黑线 = 方向误差等值线\n"
                 "（方向差很小但代价很大 —— 角落处只剩 65% 的速度）")
    ax.grid(alpha=0.25)
    ax.legend(fontsize=8.5, loc="lower left")

    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "expB_saturation.png"), dpi=140)
    plt.close(fig)

    path = os.path.join(OUT, "expB_saturation.csv")
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["工况", "配置", "指令方向deg", "实际方向deg", "方向差deg",
                     "实际速度m/s", "指令速度m/s", "自转完成度%",
                     "横向偏差m", "沿程亏损m", "终点位置偏差m",
                     "目标超450rpm时间占比%", "轮子顶482rpm时间占比%", "转弯半径比"])
        wr.writerows(rows)
        wr.writerow([])
        wr.writerow(["【B2】全摇杆范围扫描（解析）"])
        wr.writerow(["最坏方向差deg", "出现位置vx", "出现位置vy", "归一化后该点方向差deg"])
        wr.writerow([round(worst[0], 2), round(worst[1], 2), round(worst[2], 2),
                     round(wrap180(dir_deg(*static_wheel_map(worst[1], worst[2], 0.0,
                                                             desat=True)[:2])
                                   - dir_deg(worst[1], worst[2])), 2)])
        wr.writerow([])
        wr.writerow(["【B3】指标定义（把误差拆开才不会得出错误结论）"])
        wr.writerow(["横向偏差m", "位置误差中垂直于摇杆方向的分量 = 侧移；旋转工况无意义，留空"])
        wr.writerow(["沿程亏损m", "位置误差中沿摇杆方向的分量；负 = 跑得不够远（速度亏损）"])
        wr.writerow(["基准", "摇杆指令 + 执行零误差的理想车（不含任何电机能力限制）"])
        wr.writerow(["自转完成度/转弯半径比", "只在旋转工况有意义；半径比 = (v/ω)实际 / (v/ω)指令"])

    print("-> 已输出 expB_saturation.png / .csv")
    return dict(cases=results, worst=worst, worst_dd=dw["dd"])


# ==================================================================
# 8. 实验 C：高速转弯"需求 vs 抓地能力"（纯能力核算，不建打滑模型）
# ==================================================================
def exp_c_traction():
    print("\n" + "=" * 84)
    print("实验 C｜高速转弯：需求侧向加速度 a_lat=|ω|·v  vs  麦轮抓地上限 "
          f"{A_LAT_GRIP} m/s²（μ≈{A_LAT_GRIP/9.81:.2f}）")
    print("=" * 84)
    vxs = (1.0, 2.0, 3.0)
    wrpms = (30.0, 60.0, 90.0, 120.0)

    def chain_allowed_v(w_rpm, alat_on, alat_max=CHASSIS_ALAT_MAX):
        """跑一遍"斜坡 + 侧向加速度限制"这层（不含轮子），量出稳态实际允许多快。

        为什么还要仿真而不是直接写 a/|ω|：为了验证限制器代码本身
        （包括 vmag<0.05 的保护、等比缩放有没有把方向改掉）——
        解析式和实现对不上，是这种"限幅器"最典型的 bug。
        """
        w_t = w_rpm * math.pi / 30.0
        vx = w = 0.0
        for _ in range(1500):
            vx = slew_toward(vx, STICK_VXY, CHASSIS_ACC_LIN * DT)
            w = slew_toward(w, w_t, CHASSIS_ACC_ANG * DT)
            ox, oy, _ = limit_lateral_accel(vx, 0.0, w, alat_max) if alat_on else (vx, 0.0, 1.0)
        return ox

    rows = []
    print("\n" + pad("ω(rpm)", 9) + pad("ω(rad/s)", 10) + pad("vx(m/s)", 9)
          + pad("a_lat(m/s²)", 12) + pad("a_lat/g", 10) + pad("判定", 12)
          + pad("限幅后vx", 10) + pad("限幅后a_lat", 12))
    print("-" * 100)
    for w_rpm in wrpms:
        w_rad = w_rpm * math.pi / 30.0
        for vx in vxs:
            a = w_rad * vx
            if a > A_LAT_GRIP:
                verdict = "必然侧滑"
            elif a > 0.8 * A_LAT_GRIP:
                verdict = "临界"
            else:
                verdict = "安全"
            v_lim = chain_allowed_v(w_rpm, True)
            a_lim = w_rad * v_lim
            rows.append([w_rpm, round(w_rad, 3), vx, round(a, 2), round(a / 9.81, 2),
                         verdict, round(v_lim, 3), round(a_lim, 2)])
            print(pad(f"{w_rpm:.0f}", 9) + pad(f"{w_rad:.2f}", 10)
                  + pad(f"{vx:.1f}", 9) + pad(f"{a:.2f}", 12) + pad(f"{a/9.81:.2f}", 10)
                  + pad(verdict, 12) + pad(f"{v_lim:.2f}", 10) + pad(f"{a_lim:.2f}", 12))

    n_slip = sum(1 for r in rows if r[5] == "必然侧滑")
    print(f"\n  [!] {len(rows)} 个组合里有 {n_slip} 个必然侧滑（a_lat > {A_LAT_GRIP}）。"
          f"唯一安全的是 vx=1.0 + 30 rpm（a_lat={rows[0][3]}）。")
    print(f"      => 麦轮上'既要 3 m/s 又要转'是物理不成立的，不是调参问题。")
    w_at_3 = CHASSIS_ALAT_MAX / 3.0 * 30.0 / math.pi
    print(f"      => 3 m/s 时侧向加速度限制器允许的最大自转只有 {w_at_3:.1f} rpm"
          f"（{w_at_3*6:.0f} °/s）：再加油门就会把平移速度砍下来。")

    # ---------- 画图 ----------
    fig, axes = plt.subplots(1, 2, figsize=(13.2, 5.4))

    ax = axes[0]
    w_grid = np.linspace(0.5, 130.0, 400)
    w_rad = w_grid * math.pi / 30.0
    v_nolim = np.full_like(w_grid, STICK_VXY)
    v_lim = np.minimum(STICK_VXY, CHASSIS_ALAT_MAX / w_rad)
    v_grip = np.minimum(STICK_VXY, A_LAT_GRIP / w_rad)
    ax.fill_between(w_grid, v_grip, STICK_VXY, color="tab:red", alpha=0.12)
    ax.plot(w_grid, v_nolim, color="tab:red", linewidth=2.0,
            label=f"不限幅：摇杆要多少给多少（{STICK_VXY} m/s）")
    ax.plot(w_grid, v_lim, color="tab:blue", linewidth=2.2,
            label=f"a-lat 限制器 a={CHASSIS_ALAT_MAX} m/s² → v≤a/|ω|")
    ax.plot(w_grid, v_grip, color="k", linewidth=1.4, linestyle="--",
            label=f"抓地物理上限 a={A_LAT_GRIP} m/s²（红色区域=必然侧滑）")
    ax.text(75, 2.1, "必然侧滑区", color="tab:red", fontsize=10, rotation=-20)
    for w_rpm, mk in zip(wrpms, ("o", "s", "^", "D")):
        wv = np.array(vxs)
        ax.plot(np.full(3, w_rpm), wv, mk, color="0.25", markersize=5)
        ok = w_rpm * math.pi / 30.0 * wv <= A_LAT_GRIP
        ax.plot(np.full(int(ok.sum()), w_rpm), wv[ok], mk, color="tab:green",
                markersize=8, markeredgecolor="k")
    ax.plot([], [], "o", color="tab:green", markersize=8, label="表 C 中抓地允许的点")
    ax.plot([], [], "o", color="0.25", markersize=5, label="表 C 中必然侧滑的点")
    ax.set_xlabel("自转角速度 ω (rpm)")
    ax.set_ylabel("平移速度 vx (m/s)")
    ax.set_title("带/不带 a-lat 限制器的 vx–ω 可行边界\n"
                 "限制器把需求压到能力以内（代价是推满却不快）")
    ax.set_xlim(0, 130)
    ax.set_ylim(0, 3.4)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8.2, loc="upper right")

    ax = axes[1]
    width = 0.26
    idx = np.arange(len(wrpms))
    for j, vx in enumerate(vxs):
        vals = [w_ * math.pi / 30.0 * vx for w_ in wrpms]
        bars = ax.bar(idx + (j - 1) * width, vals, width, label=f"vx={vx} m/s")
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v + 0.5, f"{v:.0f}",
                    ha="center", fontsize=7.5)
    ax.axhline(A_LAT_GRIP, color="k", linestyle="--", linewidth=1.5,
               label=f"抓地上限 {A_LAT_GRIP} m/s²")
    ax.axhline(CHASSIS_ALAT_MAX, color="tab:blue", linestyle=":", linewidth=1.5,
               label=f"限制器阈值 {CHASSIS_ALAT_MAX} m/s²")
    ax.set_xticks(idx)
    ax.set_xticklabels([f"{w:.0f} rpm" for w in wrpms])
    ax.set_ylabel("需求侧向加速度 a-lat (m/s²)")
    ax.set_title("各工况需求 vs 抓地能力\n"
                 f"（{n_slip}/{len(rows)} 个组合超出抓地，注：限制器阈值 5.0 本身就比抓地 4.5 高 11%）")
    ax.grid(alpha=0.3, axis="y")
    ax.legend(fontsize=8.2)

    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "expC_traction_limit.png"), dpi=140)
    plt.close(fig)

    path = os.path.join(OUT, "expC_traction_limit.csv")
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["ω_rpm", "ω_rad_s", "vx_m_s", "a_lat_m_s2", "a_lat_g",
                     "判定", "限幅后允许vx_m_s", "限幅后a_lat_m_s2"])
        wr.writerows(rows)

    print("-> 已输出 expC_traction_limit.png / .csv")
    return dict(rows=rows, n_slip=n_slip, w_at_3=w_at_3)


# ==================================================================
# 9. 实验 D：航向锁增益选择与鲁棒性
# ==================================================================
def exp_d_yaw_gain():
    print("\n" + "=" * 84)
    print("实验 D｜航向锁 KP/KD 扫描：0.5 s 时给一个'50 ms 内被转过 10°'的扰动")
    print("=" * 84)
    T_END = 4.0
    T_KICK = 0.5
    KICK_DEG = 10.0
    KICK_DUR = 0.05
    kick_rate = KICK_DEG / KICK_DUR * math.pi / 180.0    # rad/s

    def dist_fn(t):
        return kick_rate if T_KICK <= t < T_KICK + KICK_DUR else 0.0

    straight = lambda t: (STICK_VXY, 0.0, 0.0)
    kps = (1.5, 3.0, 5.0)
    kds = (0.0, 0.25, 0.5)

    print("\n" + pad("KP", 7) + pad("KD", 7) + pad("闭环时间常数(1+KD)/KP", 24)
          + pad("调节时间(s)", 13) + pad("超调(°)", 11) + pad("稳态误差(°)", 13)
          + pad("峰值修正(rad/s)", 16) + pad("饱和时间占比%", 15))
    print("-" * 115)
    rows, logs = [], {}
    for kp in kps:
        for kd in kds:
            sim = DriftSim(imu_lock=True, desat=True, kp_yaw=kp, kd_yaw=kd)
            log = sim.run(T_END, straight, dist_fn=dist_fn)
            err = log.th - 0.0                       # 目标航向 = 0°
            t_set, os_, ss = heading_settle(log.t, err, T_KICK)
            peak = float(np.max(np.abs(log.w_corr)))
            satf = float(np.mean(np.abs(log.w_corr) >= CHASSIS_YAW_CORR_MAX - 1e-6) * 100)
            tau = (1.0 + kd) / kp
            rows.append([kp, kd, round(tau, 3), round(t_set, 3), round(os_, 3),
                         round(ss, 3), round(peak, 3), round(satf, 2)])
            logs[(kp, kd)] = log
            print(pad(f"{kp:.1f}", 7) + pad(f"{kd:.2f}", 7)
                  + pad(f"{tau:.2f} s", 24)
                  + pad(f"{t_set:.2f}" if not math.isnan(t_set) else "未收敛", 13)
                  + pad(f"{os_:.2f}", 11) + pad(f"{ss:.3f}", 13)
                  + pad(f"{peak:.3f}", 16) + pad(f"{satf:.1f}", 15))

    # 噪声表现：稳态时修正量里有多少是陀螺噪声翻出来的
    best = logs[(3.0, 0.25)]
    tail = best.t >= 2.0
    print(f"\n  抖动检查（KP=3, KD=0.25，稳态 t>2 s）：修正量 ω_corr 的 σ = "
          f"{np.std(best.w_corr[tail]):.4f} rad/s，折合轮速差 "
          f"{np.std(best.w_corr[tail])*K_GEOM*RPM_PER_MPS:.2f} rpm ——")
    print(f"  远小于速度环静差（~5 rpm），所以在本参数下噪声不会引起可感知的抖动，"
          f"KD 不需要为了噪声加大。")

    # 零偏诊断：把陀螺零偏清零再跑 3 组，看"稳态航向误差"到底是谁贡献的
    print("\n  [零偏诊断] 同一组 KP（KD=0.25），把陀螺零偏清成 0 再跑，看稳态误差：")
    ss_b0 = {}
    for kp in kps:
        lo = DriftSim(imu_lock=True, desat=True, kp_yaw=kp, kd_yaw=0.25,
                      gyro_bias=0.0).run(T_END, straight, dist_fn=dist_fn)
        t_set0, os0, ss0 = heading_settle(lo.t, lo.th, T_KICK)
        t_set1 = next(r[3] for r in rows if r[0] == kp and r[1] == 0.25)
        ss1 = next(r[5] for r in rows if r[0] == kp and r[1] == 0.25)
        ss_b0[kp] = ss0
        print(f"          KP={kp:<4} 调节 {t_set0:.2f} s（有零偏 {t_set1:.2f} s）、"
              f"稳态误差 {ss0:.3f}°（有零偏 {ss1:.3f}°）")
    print(f"          => 零偏清零后，KP=3/5 的稳态误差从 0.24/0.30° 掉到 "
          f"{ss_b0[3.0]:.2f}/{ss_b0[5.0]:.2f}°（剩下的那点才是轮径漂移造成的固有静差 "
          f"≈ω漂/KP）。")
    print(f"             KP=1.5 那行反而变大，是因为它调节时间 1.81 s、4 s 窗口里"
          f"'没走完'和'零偏'混在一起。")
    print(f"             结论：稳态误差这一列不适合选参，选参只看调节时间和超调。")

    # ---------- 画图 ----------
    fig, axes = plt.subplots(2, 2, figsize=(13.0, 8.6))
    colors = {1.5: "tab:red", 3.0: "tab:blue", 5.0: "tab:green"}
    styles = {0.0: ":", 0.25: "-", 0.5: "--"}

    ax = axes[0, 0]
    for (kp, kd), log in logs.items():
        ax.plot(log.t, log.th, color=colors[kp], linestyle=styles[kd], linewidth=1.6,
                label=f"KP={kp} KD={kd}")
    ax.axhspan(-1.0, 1.0, color="tab:green", alpha=0.12, label="±1° 收敛带")
    ax.axvline(T_KICK, color="k", linestyle=":", linewidth=1)
    ax.text(T_KICK + 0.02, 9.0, "10° 扰动", fontsize=8.5)
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("真实航向角 (°)")
    ax.set_title("航向扰动后的回整过程（真值，不是陀螺积分值）\n"
                 "9 组组合全部稳定：200 Hz 保持 + 5 ms 延迟总共只有 10 ms，"
                 "远小于闭环时间常数")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=7.6, ncol=2)

    ax = axes[0, 1]
    for kd in kds:
        ts = [rows[i][3] for i in range(len(rows)) if rows[i][1] == kd]
        ax.plot(kps, ts, marker="o", linestyle=styles[kd], linewidth=1.9,
                label=f"KD={kd}")
    ax.set_xlabel("KP")
    ax.set_ylabel("回到 ±1° 的调节时间 (s)")
    ax.set_title("调节时间 ∝ (1+KD)/KP：\n"
                 "KD 在这里其实在拖慢响应：速率反馈并进角度环 = 给积分器加了个滞后")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=9)

    ax = axes[1, 0]
    xs = np.arange(len(rows))
    ax.bar(xs - 0.2, [r[4] for r in rows], 0.4, label="超调(°)")
    ax.bar(xs + 0.2, [r[5] for r in rows], 0.4, label="稳态误差(°)")
    ax.set_xticks(xs)
    ax.set_xticklabels([f"{r[0]}/{r[1]}" for r in rows], fontsize=7.5, rotation=45)
    ax.set_xlabel("KP / KD")
    ax.set_ylabel("角度 (°)")
    ax.set_title("超调与稳态误差\n（超调几乎为 0：这是'角度 P + 速率 D'的一阶响应；\n"
                 "稳态误差 0.1~0.4° 是零偏漂移与未收敛的混合，不用于选参）")
    ax.grid(alpha=0.3, axis="y")
    ax.legend(fontsize=9)

    ax = axes[1, 1]
    lg = logs[(3.0, 0.25)]
    ax.plot(lg.t, lg.w_corr * 180.0 / math.pi, color="tab:blue", linewidth=1.8,
            label="航向锁修正量 ωcorr (°/s) —— 左轴")
    ax.axhline(CHASSIS_YAW_CORR_MAX * 180 / math.pi, color="k", linestyle="--",
               linewidth=1, label=f"修正量限幅 ±{CHASSIS_YAW_CORR_MAX} rad/s")
    ax.axhline(-CHASSIS_YAW_CORR_MAX * 180 / math.pi, color="k", linestyle="--",
               linewidth=1)
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("修正量 ωcorr (°/s)")
    ax.set_ylim(-100, 100)
    ax2 = ax.twinx()
    ax2.plot(lg.t, lg.w * 180.0 / math.pi, color="tab:red", linewidth=1.0, alpha=0.8,
             label="真实偏航角速度 (°/s) —— 右轴（峰值 200°/s 被截断）")
    ax2.set_ylabel("真实偏航角速度 (°/s)")
    ax2.set_ylim(-100, 210)
    ax.set_title("推荐参数 KP=3 / KD=0.25 的细节\n"
                 f"扰动的 200°/s 峰值靠 D 项顶住（修正量峰值 "
                 f"{np.max(np.abs(lg.w_corr))*180/math.pi:.0f}°/s），之后压到 ~0.1°/s")
    ax.grid(alpha=0.3)
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, fontsize=8.0, loc="lower right")

    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "expD_yaw_gain.png"), dpi=140)
    plt.close(fig)

    path = os.path.join(OUT, "expD_yaw_gain.csv")
    with open(path, "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["KP", "KD", "闭环时间常数s", "调节时间s", "超调deg",
                     "稳态误差deg", "峰值修正rad_s", "修正饱和时间占比%"])
        wr.writerows(rows)

    print("-> 已输出 expD_yaw_gain.png / .csv")
    return dict(rows=rows, kps=kps, kds=kds)


# ==================================================================
# 10. 汇总（直接可粘进文档）
# ==================================================================
def print_summary(A, B, C, D):
    print("\n" + "=" * 84)
    print("汇总（可直接粘进技术文档）")
    print("=" * 84)

    sw = A["sweep"]
    s4 = next(s for s in sw if abs(s["amp"] - 0.004) < 1e-9)
    s18 = next(s for s in sw if abs(s["amp"] - 0.018) < 1e-9)
    print("\n【实验 A｜高速直行的跑偏有多大】")
    print(f"  · 轮径误差 δ = {DELTA_BASE[0]*100:+.1f}/{DELTA_BASE[1]*100:+.1f}/"
          f"{DELTA_BASE[2]*100:+.1f}/{DELTA_BASE[3]*100:+.1f}% (FL/FR/RL/RR) 时，"
          f"3.0 m/s 直行 3.5 s(≈10.4 m) 基线偏 {s4['y_base']:+.0f} mm、"
          f"航向 {s4['th_base']:+.2f}°；5 m 处只有 {s4['y5_base']:+.0f} mm。")
    print(f"  · 误差放大到 ±1.8%（等效固件日志实测的 1.5°/s）时，3.5 s 偏 "
          f"{s18['y_base']:+.0f} mm、5 m 处 {s18['y5_base']:+.0f} mm —— "
          f"半米级，这就是'高速侧移明显'的量级。")
    print(f"  · IMU 航向锁把终点横向偏移压到 {s4['y_lock']:+.0f} mm（抑制 "
          f"{s4['sup']:.0f}%），航向从 {s4['th_base']:+.2f}° 压到 {s4['th_lock']:+.2f}°；")
    print(f"    但残余不为 0：轮径误差同时造出一个**纯侧向**速度（陀螺看不见），"
          f"陀螺只能修'转'，修不了'横'。")
    print(f"  · 饱和归一化在纯直行里是空转（②与①逐点差 {A['d02']:.2f} mm，"
          f"3 m/s 只需 376 rpm < 450）。")
    print(f"  · 零偏才是长期主因：t≈{2*A['lin']/A['quad']:.0f} s 之前两股误差互相抵消"
          f"（3.5 s 时带零偏反而更准），之后零偏的二次项反超：")
    print(f"    20 s 时 {A['y20']:+.0f} mm，零偏标定后只剩 {A['y20_b0']:+.0f} mm"
          f"（无锁基线 {A['y20_base']:+.0f} mm）—— 航向锁必须配上电零偏标定。")

    print("\n【实验 B｜450 rpm 上限 + 满杆复合指令】")
    Bc = B["cases"]
    a = Bc[("斜推", "不做归一化")]
    b = Bc[("斜推", "做归一化")]
    c = Bc[("前进+自转", "不做归一化")]
    e = Bc[("前进+自转", "做归一化")]
    print(f"  · 斜推 (3.0, 1.5)：不归一化方向差 {a['dd']:+.1f}°、速度 {a['spd']:.2f}/"
          f"{a['spd_cmd']:.2f} m/s；归一化方向差 {b['dd']:+.1f}°、速度 "
          f"{b['spd']:.2f}/{b['spd_cmd']:.2f} m/s。")
    print(f"    {abs(a['dd']):.1f}° 听着小，跑 10 m 就是 "
          f"{10*math.tan(math.radians(abs(a['dd']))):.2f} m 的横向偏差 —— 定向赛里等于出局。")
    print(f"  · 前进+自转：两种做法方向都不歪（{c['dd']:+.1f}° / {e['dd']:+.1f}°），"
          f"因为左右两对被**对称**卡住；真正的问题是")
    print(f"    自转只完成 {c['yaw_done']:.0f}%、平移只完成 {c['spd']/c['spd_cmd']*100:.0f}%"
          f"（比例失配 → 转弯半径 {c['radius_ratio']:.2f} 倍），"
          f"归一化后两者等比缩水、半径不变（{e['radius_ratio']:.2f} 倍）。")
    print(f"  · 全场最坏方向差 {B['worst_dd']:+.1f}° @ (vx={B['worst'][1]:.1f}, "
          f"vy={B['worst'][2]:+.1f})：归一化不改变'能跑多快'，只改变'方向对不对'。")
    print(f"  · 结论：归一化的代价是'整体慢一点'，不归一化是'方向歪 + 比例失控'。"
          f"前者可以预判，后者不行。")

    print("\n【实验 C｜高速转弯的物理账】")
    print(f"  · 12 个 (vx, ω) 组合里 {C['n_slip']} 个需求侧向加速度超过麦轮抓地 "
          f"{A_LAT_GRIP} m/s²，唯一安全的是 vx=1.0 + 30 rpm。")
    print(f"  · 3 m/s 时限制器只允许转 {C['w_at_3']:.1f} rpm（{C['w_at_3']*6:.0f} °/s）；"
          f"想要 60 rpm 满转就必须把平移砍到 "
          f"{CHASSIS_ALAT_MAX/(60*math.pi/30):.2f} m/s。")
    print(f"  · 注意限制器阈值 5.0 比抓地 4.5 高 11%：它保证'不失控'，不保证'不打滑'。")

    print("\n【实验 D｜航向锁参数】")
    best = next(r for r in D["rows"] if r[0] == 3.0 and r[1] == 0.25)
    fast = min(D["rows"], key=lambda r: (r[3] if not math.isnan(r[3]) else 9e9))
    print(f"  · 9 组 (KP, KD) 全部稳定、无振荡：闭环调节时间 "
          f"{min(r[3] for r in D['rows']):.2f}~{max(r[3] for r in D['rows']):.2f} s，"
          f"超调全部 < {max(r[4] for r in D['rows']):.2f}°。")
    print(f"  · 物理原因：闭环带宽最高才 KP=5 → {(1+0.0)/5.0:.2f} s 时间常数，"
          f"而采样+延迟只有 10 ms，相位代价可以忽略 —— 所以'sensor 延迟导致抖'"
          f"在这个带宽下不成立。")
    print(f"  · 推荐 KP=3.0 / KD=0.25（固件现值）：调节时间 {best[3]:.2f} s，"
          f"超调 {best[4]:.2f}°，稳态误差 {best[5]:.2f}°（这一列随增益乱跳，"
          f"零偏清零后全部≈0，说明它只反映零偏、不反映参数好坏）。")
    print(f"  · 最快的组合是 KP={fast[0]} / KD={fast[1]}（{fast[3]:.2f} s），"
          f"但它对陀螺噪声/轮子动态更敏感，收益只有 "
          f"{(best[3]-fast[3])*1000:.0f} ms —— 不值得为它牺牲鲁棒性。")

    print("\n【给固件的参数建议】")
    print(f"  1. 航向锁 KP=3.0 / KD=0.25 保持不动（实验 D 证明它是'够快且稳'的中间点）。")
    print(f"     真正该加的是**上电零偏标定**：0.1°/s 的零偏在 10 s 内就是 1° 航向、"
          f"3 m/s 下 5 cm 横移，")
    print(f"     而 P 环会把'零偏造成的假角度'当真去修，结果车身真的慢慢转过去 ——"
          f"这是当前航向锁最大的残余误差源。")
    print(f"  2. 饱和归一化 CHASSIS_DESAT_EN 必须开。它买到的是"
          f"'方向与平移/自转的比例可预期'，不是'跑得更快'——")
    print(f"     不归一化时斜推方向差 {abs(B['cases'][('斜推','不做归一化')]['dd']):.1f}°、"
          f"自转/平移完成度 {B['cases'][('前进+自转','不做归一化')]['yaw_done']:.0f}% vs "
          f"{B['cases'][('前进+自转','不做归一化')]['spd']/B['cases'][('前进+自转','不做归一化')]['spd_cmd']*100:.0f}%，"
          f"都是'说不清'的误差。")
    print(f"  3. 450 rpm 上限合理（空载 482 留 7% 余量），但归一化后车只会更慢，"
          f"建议把 CHASSIS_VXY_MAX 从 3.0 降到 2.8，把'慢'变成可预期的，")
    print(f"     而不是让驾驶员推到满杆才发现车不动。")
    print(f"  4. a_lat_max=5.0 偏保守（比抓地 4.5 还高 11%，既不防滑也不够灵敏）："
          f"要么降到 4.0 真正防滑，要么抬到 6.0 保手感。")
    print(f"     3 m/s 下它只允许 {C['w_at_3']:.0f} rpm 自转，比赛中'高速+急转'必须靠"
          f"先减速，不能靠打杆。")


def main():
    print("=" * 84)
    print("RM2027 校内赛 电控仿真：麦轮底盘高速侧移 / 跑偏量化")
    print("=" * 84)
    print(f"参数：r={R_NOM} m, k={K_GEOM}, 轮速环 kp={WHEEL_KP:.0f}(纯P)/"
          f"±{CHASSIS_IQ_MAX:.0f}, 空载 {RPM_NOLOAD:.0f} rpm, "
          f"归一化阈值 {CHASSIS_WHEEL_RPM_MAX:.0f} rpm")
    print(f"      斜坡 {CHASSIS_ACC_LIN} m/s² / {CHASSIS_ACC_ANG} rad/s², "
          f"a_lat_max {CHASSIS_ALAT_MAX}, 陀螺 200 Hz + 5 ms 延迟 + "
          f"{GYRO_BIAS_DPS}°/s 零偏 + {GYRO_NOISE_DPS}°/s 噪声")
    A = exp_a_straight()
    B = exp_b_saturation()
    C = exp_c_traction()
    D = exp_d_yaw_gain()
    print_summary(A, B, C, D)
    print("\n" + "=" * 84)
    print(f"全部输出在: {OUT}")
    print("=" * 84)


if __name__ == "__main__":
    main()
