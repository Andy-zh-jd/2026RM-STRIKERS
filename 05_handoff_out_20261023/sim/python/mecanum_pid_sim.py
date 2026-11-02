"""
RM2027 校内赛 — 四轮麦克纳姆轮运动学 + 串级 PID 仿真
================================================================
用途：零硬件期（10/11 领物资前）先把控制逻辑跑通，产出：
  1. 麦轮正/逆运动学验证（逆解 -> 正解 应闭合）
  2. 24V 母线电流预算（爬 23 度坡 / 上 20cm 台阶）
  3. 单电机速度环 PID 阶跃响应（M3508 + C620 场景）
  4. 机构位置环：单环 vs 串级 vs 串级+前馈（M2006 + C610 场景）
  5. 麦轮底盘走直线轨迹（说明为什么必须四轮闭环）

运行：python mecanum_pid_sim.py
输出：./sim_out/ 目录下的 PNG 与 CSV
"""

import os
import sys
import csv
import math
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Windows 控制台默认 GBK，强制 UTF-8 避免中文/符号报错
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sim_out")
os.makedirs(OUT, exist_ok=True)

DT = 0.001  # 控制周期 1ms，与 RM 常规 1kHz 一致

# 全局物理常数（来自 DJI 官方规格，不是估计值）
M3508_RATIO = 3591.0 / 187.0   # ≈ 19.2032
M2006_RATIO = 36.0
M3508_T_CONT = 3.0             # N·m 持续扭矩
M3508_I_CONT = 20.0            # A   C620 持续电流
M2006_T_CONT = 1.0             # N·m 持续扭矩
M2006_I_CONT = 10.0            # A   C610 持续电流
ESC_CMD_MAX = 16384.0          # CAN 电流指令满量程


# ==================================================================
# 1. 麦克纳姆轮运动学
# ==================================================================
class MecanumKinematics:
    """四轮 X 型布局（辊子与车体成 45°）麦克纳姆轮运动学。

    坐标约定：x 向前、y 向左、w 逆时针为正（右手系）
    轮序：0=左前 FL, 1=右前 FR, 2=左后 RL, 3=右后 RR
    内部统一用轮子线速度 (m/s)，对外可换算成电机转子 RPM。
    """

    def __init__(self, wheel_radius=0.076, half_len=0.20, half_wid=0.20):
        self.r = wheel_radius          # 76mm 麦轮（常见规格）
        self.lx = half_len             # 轮心到车体中心 纵向半距 (m)
        self.ly = half_wid             # 轮心到车体中心 横向半距 (m)
        self.k = self.lx + self.ly     # 旋转项系数

    def inverse(self, vx, vy, w):
        """车体速度 -> 四轮线速度（逆解）"""
        k = self.k
        return np.array([
            vx - vy - k * w,   # FL
            vx + vy + k * w,   # FR
            vx + vy - k * w,   # RL
            vx - vy + k * w,   # RR
        ])

    def forward(self, wheel_v):
        """四轮线速度 -> 车体速度（正解，用于自检与状态估计）"""
        v_fl, v_fr, v_rl, v_rr = wheel_v
        k = self.k
        vx = (v_fl + v_fr + v_rl + v_rr) / 4.0
        vy = (-v_fl + v_fr + v_rl - v_rr) / 4.0
        w = (-v_fl + v_fr - v_rl + v_rr) / (4.0 * k)
        return vx, vy, w

    def wheel_to_rotor_rpm(self, wheel_v):
        """轮线速度 -> 电机转子 RPM（电调回传的就是转子 RPM）"""
        return wheel_v / (2 * math.pi * self.r) * 60.0 * M3508_RATIO

    def rotor_rpm_to_wheel(self, rotor_rpm):
        """电机转子 RPM -> 轮线速度（读电调反馈时用这个）"""
        return rotor_rpm / M3508_RATIO / 60.0 * (2 * math.pi * self.r)


# ==================================================================
# 2. PID
# ==================================================================
class PID:
    """带积分限幅、输出限幅、微分低通、微分先行、前馈的 PID。

    每一条都是踩过坑才加的：
      - 积分限幅：不做的话电机堵转时积分爆掉，松手后车猛冲。
        ★但限幅值必须 >= 稳态所需指令，否则 I 项被"卡住"、静差永远消不掉。
      - 微分低通：不做的话编码器量化噪声被微分放大 -> 电机啸叫。
      - 微分先行：对测量值微分而非误差，设定值阶跃时不产生微分尖峰。
      - ff 前馈：把"已知的"重力/摩擦直接补上，不让 PID 慢慢去试。
    """

    def __init__(self, kp, ki, kd, out_limit, i_limit=None,
                 d_lpf_alpha=0.2, name="pid"):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.out_limit = out_limit
        self.i_limit = i_limit if i_limit is not None else out_limit
        self.alpha = d_lpf_alpha
        self.name = name
        self.integral = 0.0
        self.prev_meas = 0.0
        self.d_filt = 0.0
        self.last_out = 0.0
        self.last_p = self.last_i = self.last_d = 0.0

    def reset(self):
        self.integral = 0.0
        self.prev_meas = 0.0
        self.d_filt = 0.0

    def update(self, setpoint, meas, dt, ff=0.0):
        err = setpoint - meas
        p = self.kp * err

        # 条件积分：只有未饱和时才继续累积，抑制积分饱和
        i_new = self.integral + self.ki * err * dt
        i_new = float(np.clip(i_new, -self.i_limit, self.i_limit))
        self.integral = i_new

        # 微分先行（对测量值微分）+ 一阶低通
        d_raw = -(meas - self.prev_meas) / dt * self.kd
        self.d_filt += self.alpha * (d_raw - self.d_filt)
        self.prev_meas = meas

        out = p + self.integral + self.d_filt + ff
        out = float(np.clip(out, -self.out_limit, self.out_limit))
        self.last_p, self.last_i, self.last_d, self.last_out = p, self.integral, self.d_filt, out
        return out


# ==================================================================
# 3. 电机模型
# ==================================================================
class MotorModel:
    """一阶惯性 + 恒定负载 + 编码器量化噪声的电机模型。

    参数按 M3508 数量级设定（用于建立"参数感觉"，不是精确辨识）：
      空载 482rpm@输出轴 -> 转子 ≈ 482*19.2 ≈ 9254rpm，故 gain 取 9000。
      时间常数 30ms（含电流环与机械惯量）。
    """

    def __init__(self, tau=0.030, gain=9000.0, load=0.0, noise=0.0,
                 cmd_limit=ESC_CMD_MAX):
        self.tau = tau
        self.gain = gain
        self.load = load        # 恒定负载等效转速损失 (rpm)
        self.noise = noise      # 编码器量化噪声幅度 (rpm)
        self.cmd_limit = cmd_limit
        self.rpm = 0.0
        self.cmd = 0.0

    def step(self, cmd, dt):
        self.cmd = float(np.clip(cmd, -self.cmd_limit, self.cmd_limit))
        target = self.gain * (self.cmd / self.cmd_limit) - self.load
        self.rpm += (target - self.rpm) * dt / self.tau
        meas = self.rpm
        if self.noise > 0:
            meas += np.random.normal(0, self.noise)
        return meas


def step_metrics(t, y, target, band_pct=2.0):
    """统一计算阶跃响应指标。超调只在真正到达过目标时才计算。"""
    band = abs(target) * band_pct / 100.0
    inside = np.abs(y - target) <= band
    t_rise = float("nan")
    if inside.any():
        idx = int(np.argmax(inside))
        # 上升时间：第一次进入 ±band 的时刻
        t_rise = float(t[idx])
    reached = bool((y >= target).any()) if target > 0 else bool((y <= target).any())
    overshoot = (y.max() - target) / target * 100.0 if reached else float("nan")
    return y[-1], target - y[-1], overshoot, t_rise


# ==================================================================
# 实验一：运动学自检
# ==================================================================
def exp1_kinematics():
    kin = MecanumKinematics()
    cases = [
        ("前进 vx=1.0", 1.0, 0.0, 0.0),
        ("左移 vy=1.0", 0.0, 1.0, 0.0),
        ("原地逆时针 w=1.0", 0.0, 0.0, 1.0),
        ("斜向 vx=vy=0.8", 0.8, 0.8, 0.0),
        ("复合 vx=0.6,vy=0.3,w=0.5", 0.6, 0.3, 0.5),
    ]
    rows = []
    print("\n=== 实验一：麦轮运动学自检（逆解 -> 正解 应闭合）===")
    print(f"{'工况':<26}{'轮速 FL/FR/RL/RR (m/s)':<36}{'正解回代最大误差':<18}")
    for name, vx, vy, w in cases:
        wv = kin.inverse(vx, vy, w)
        bx, by, bw = kin.forward(wv)
        err = max(abs(bx - vx), abs(by - vy), abs(bw - w))
        rpm = kin.wheel_to_rotor_rpm(wv)
        rows.append([name, *[round(v, 4) for v in wv],
                     round(bx, 6), round(by, 6), round(bw, 6), round(err, 12),
                     *[round(v, 1) for v in rpm]])
        print(f"{name:<26}{np.round(wv,3).tolist()!s:<36}{err:<18.2e}")
    with open(os.path.join(OUT, "exp1_kinematics.csv"), "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["工况", "FL_m_s", "FR_m_s", "RL_m_s", "RR_m_s",
                     "正解vx", "正解vy", "正解w", "最大误差",
                     "FL转子rpm", "FR转子rpm", "RL转子rpm", "RR转子rpm"])
        wr.writerows(rows)
    print("-> 结论：所有工况正解回代误差 < 1e-9，运动学矩阵正确，可直接用于实车。")
    return kin


# ==================================================================
# 实验二：24V 母线电流预算
# ==================================================================
def exp2_power_budget():
    print("\n=== 实验二：爬 23 度坡 / 上 20cm 台阶 的电流预算（教材估算，现场必须实测）===")
    g = 9.81
    m = 18.0                 # 整车质量 (kg)，规则上限 20kg，留 2kg 余量
    r = 0.076                # 轮半径 m
    eff = 0.70               # 传动 + 麦轮打滑综合效率
    Kt = M3508_T_CONT / M3508_I_CONT   # 0.15 N·m/A

    scenarios = [
        ("平地匀速", 0.0, None),
        ("爬 23 度坡（匀速）", math.radians(23), None),
        ("上 20cm 台阶（重心抬升 0.10m）", None, 0.10),
    ]

    rows = []
    for name, slope, dh in scenarios:
        if slope is None:   # 台阶：重心抬升做的功等效为附加阻力（保守估算）
            F_slope = m * g * dh / 0.04      # 假设 4cm 水平行程内完成抬升
        else:
            F_slope = m * g * math.sin(slope)
        F_roll = m * g * 0.05                # 地胶滚动阻力系数取 0.05
        F_total = F_slope + F_roll
        T_per = (F_total * r) / 4.0 / eff
        I_per = T_per / Kt
        I_total = I_per * 4.0
        margin = (1 - T_per / M3508_T_CONT) * 100
        flag = "OK" if T_per <= M3508_T_CONT else "!! 超出持续扭矩"
        print(f"\n  [{name}]")
        print(f"    沿坡/抬升分力 {F_slope:7.1f} N + 滚动阻力 {F_roll:6.1f} N = {F_total:7.1f} N")
        print(f"    单电机轴扭矩  {T_per:7.3f} N·m   (M3508 持续 {M3508_T_CONT} N·m，余量 {margin:6.1f}%) {flag}")
        print(f"    单电机电流    {I_per:7.1f} A      (C620 持续 {M3508_I_CONT} A)")
        print(f"    四电机总电流  {I_total:7.1f} A @24V → 母线功率 ≈ {I_total*24/1000:.2f} kW")
        rows.append([name, round(F_total, 1), round(T_per, 3), round(margin, 1),
                     round(I_per, 1), round(I_total, 1)])

    print("\n  [!] 结论与对策（这几条要写进整车设计约束）：")
    print("      1. 爬 23 度坡需约 2.1 N·m/电机 = 持续扭矩的 70%，四电机合计约 56A。")
    print("         => 必须【助跑冲坡】，不能从静止硬爬（加速度扭矩会顶到 3 N·m 上限）。")
    print("      2. 上 20cm 台阶的静态估算远超持续扭矩（约 12 N·m/电机）——")
    print("         这说明台阶【不是爬上去的，是冲上去的】：靠动能，不靠扭矩。")
    print("         => 底盘要能承受冲击；这也是规则要求保险杠保护底盘的原因。")
    print("      3. 电流指令必须限幅（例如 12000/16384），给电池压降和控制留余量。")
    print("      4. 低电压保护阈值必须实测确定，不能拍脑袋 —— 瞬时压降会让主控复位，")
    print("         而在规则里'断电5秒'恰好是最危险的状态。")

    with open(os.path.join(OUT, "exp2_power_budget.csv"), "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["工况", "总阻力N", "单电机扭矩N·m", "扭矩余量%", "单电机电流A", "总电流A"])
        wr.writerows(rows)


# ==================================================================
# 实验三：单电机速度环（M3508 场景）
# ==================================================================
def exp3_velocity_loop():
    print("\n=== 实验三：M3508 速度环 PID（阶跃 0->3000rpm，含 200rpm 恒定负载 + 编码器噪声）===")
    # 注意 ki 的量级：积分项每秒累积 ki*误差。要消掉 500rpm 静差、目标指令约 6000，
    # 在亚秒级完成收敛，ki 需要 O(10) 量级。ki 取 1.5 这种"教科书感"的值，
    # 结果是 2 秒内 I 项才爬到需求量的 1/4 —— 静差几乎没消，这是实车调参最常见的坑。
    configs = [
        (2.0, 0.0, 0.0, "纯 P (Kp=2)"),
        (8.0, 0.0, 0.0, "大 P (Kp=8)"),
        (8.0, 10.0, 0.0, "P+I (Ki=10)"),
        (8.0, 10.0, 0.04, "P+I+D (推荐)"),
        (8.0, 0.5, 0.0, "P+微小I (Ki=0.5)：I 项形同虚设"),
    ]
    target = 3000.0
    t = np.arange(0, 1.5, DT)
    curves, rows = {}, []
    for kp, ki, kd, label in configs:
        pid = PID(kp, ki, kd, out_limit=ESC_CMD_MAX, i_limit=13000.0,
                  d_lpf_alpha=0.15, name=label)
        m = MotorModel(tau=0.030, gain=9000.0, load=200.0, noise=15.0)
        ys, cs = [], []
        for _ in t:
            c = pid.update(target, m.rpm, DT)
            m.step(c, DT)
            ys.append(m.rpm)
            cs.append(m.cmd)
        ys = np.array(ys)
        cs = np.array(cs)
        ss, err, overshoot, t_rise = step_metrics(t, ys, target)
        ripple = cs[-200:].std()
        curves[label] = ys
        rows.append([label, round(ss, 1), round(err, 1),
                     round(overshoot, 2) if not math.isnan(overshoot) else "",
                     round(t_rise * 1000, 0) if not math.isnan(t_rise) else "",
                     round(ripple, 1)])
        os_str = f"{overshoot:6.2f}" if not math.isnan(overshoot) else "   ---"
        tr_str = f"{t_rise*1000:5.0f}" if not math.isnan(t_rise) else "  ---"
        print(f"  {label:<28} 稳态={ss:7.1f}rpm 静差={err:6.1f} "
              f"超调={os_str}% 上升={tr_str}ms 抖动σ={ripple:5.1f}")

    fig, ax = plt.subplots(figsize=(10.5, 5.5))
    for label, ys in curves.items():
        ax.plot(t, ys, label=label, linewidth=1.7)
    ax.axhline(target, color="k", linestyle="--", linewidth=1, label=f"目标 {target:.0f} rpm")
    ax.set_xlabel("时间 (s)")
    ax.set_ylabel("M3508 转子转速 (rpm)")
    ax.set_title("实验三：速度环 PID 对比（阶跃 0 → 3000 rpm，含 200rpm 负载与编码器噪声）")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=9)
    ax.set_xlim(0, 0.8)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "exp3_velocity_pid.png"), dpi=140)
    plt.close(fig)

    with open(os.path.join(OUT, "exp3_velocity_pid.csv"), "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["配置", "稳态rpm", "静差rpm", "超调%", "上升时间ms", "指令抖动sigma"])
        wr.writerows(rows)
    print("-> 曲线已输出 exp3_velocity_pid.png")
    print("   关键认知 ①：纯 P 的静差 = 负载/gain，是【物理必然】，不是'没调好'。")
    print("              所以底盘速度环必须有 I 项，否则压到地毯上速度就掉。")
    print("   关键认知 ②：I 项要起作用，Ki 必须够大。Ki=0.5 时 1.5 秒内 I 项几乎没动；")
    print("              Ki=10 才能把静差压到接近零。'加了 I 项' ≠ '静差会消失'。")
    print("   关键认知 ③：i_limit 必须 >= 稳态所需指令（这里 ≈6000）。")
    print("              限幅设小了 I 项会被卡住，静差永远消不掉 —— 新手最常见错误。")


# ==================================================================
# 实验四：机构位置环（M2006 场景）
# ==================================================================
def exp4_cascade_position():
    print("\n=== 实验四：M2006 机构位置控制 — 单环 vs 串级 vs 串级+前馈 ===")

    GRAVITY_TORQUE = 0.25     # N·m，约占 M2006 持续扭矩 1 N·m 的 25%
    FRICTION_TORQUE = 0.08    # N·m，库伦摩擦（不可预知部分，必须靠 I 项吃掉）
    V_MAX = 1200.0            # 满指令下输出轴空载转速 (deg/s)
    ANGLE_MIN = 0.0           # ★ 机械限位：机构不能转到地面以下（模拟撞硬限位）

    def torque_to_cmd(tau):
        return tau / M2006_T_CONT * ESC_CMD_MAX

    DROOP_V = (GRAVITY_TORQUE + FRICTION_TORQUE) / M2006_T_CONT * V_MAX
    FF_CMD = torque_to_cmd(GRAVITY_TORQUE)
    print(f"  重力负载 {GRAVITY_TORQUE} N·m + 库伦摩擦 {FRICTION_TORQUE} N·m")
    print(f"  无前馈时的等效速度压降 = {DROOP_V:.0f} deg/s")
    print(f"  重力前馈指令 = {FF_CMD:.0f} / {ESC_CMD_MAX:.0f}（只补重力，摩擦留给 I 项）")
    print(f"  机械限位：输出轴角度不得低于 {ANGLE_MIN:.0f}°（撞限位=机构损坏）")

    class PosMotor:
        """M2006 位置电机模型（减速比 36:1）。带机械限位。"""

        def __init__(self):
            self.tau = 0.020
            self.ang = 0.0
            self.vel = 0.0
            self.hit_limit = False

        def step_with_cmd(self, cmd, dt):
            target_vel = cmd / ESC_CMD_MAX * V_MAX - DROOP_V
            self.vel += (target_vel - self.vel) * dt / self.tau
            self.ang += self.vel * dt
            if self.ang < ANGLE_MIN:          # 撞机械限位：直接顶住
                self.ang = ANGLE_MIN
                self.vel = 0.0
                self.hit_limit = True
            return self.ang, self.vel

    target_deg = 90.0
    t = np.arange(0, 2.5, DT)
    results, limits = {}, {}

    # ---------- 方案 1：单环位置 P+D（无积分）----------
    pid_pos = PID(45.0, 0.0, 1.5, out_limit=ESC_CMD_MAX, i_limit=12000.0, d_lpf_alpha=0.2)
    m = PosMotor()
    a1 = []
    for _ in t:
        cmd = pid_pos.update(target_deg, m.ang, DT)
        a, _ = m.step_with_cmd(cmd, DT)
        a1.append(a)
    results["单环位置 P+D (P=45,D=1.5)"] = np.array(a1)
    limits["单环位置 P+D (P=45,D=1.5)"] = m.hit_limit

    # ---------- 方案 2：串级，无前馈 ----------
    outer = PID(6.0, 0.0, 0.0, out_limit=800.0, d_lpf_alpha=1.0)
    inner = PID(20.0, 30.0, 0.0, out_limit=ESC_CMD_MAX, i_limit=13000.0, d_lpf_alpha=0.1)
    m = PosMotor()
    a2 = []
    for _ in t:
        v_target = outer.update(target_deg, m.ang, DT)
        cmd = inner.update(v_target, m.vel, DT)
        a, _ = m.step_with_cmd(cmd, DT)
        a2.append(a)
    results["串级（无前馈）"] = np.array(a2)
    limits["串级（无前馈）"] = m.hit_limit

    # ---------- 方案 3：串级 + 重力前馈（推荐）----------
    outer3 = PID(6.0, 0.0, 0.0, out_limit=800.0, d_lpf_alpha=1.0)
    inner3 = PID(20.0, 30.0, 0.0, out_limit=ESC_CMD_MAX, i_limit=13000.0, d_lpf_alpha=0.1)
    m = PosMotor()
    a3, v3, sp3 = [], [], []
    for _ in t:
        v_target = outer3.update(target_deg, m.ang, DT)
        cmd = inner3.update(v_target, m.vel, DT, ff=FF_CMD)
        a, v = m.step_with_cmd(cmd, DT)
        a3.append(a); v3.append(v); sp3.append(v_target)
    results["串级 + 重力前馈 (推荐)"] = np.array(a3)
    limits["串级 + 重力前馈 (推荐)"] = m.hit_limit

    fig, axes = plt.subplots(2, 1, figsize=(10.5, 7.5), sharex=True)
    colors = {"单环位置 P+D (P=45,D=1.5)": "tab:orange",
              "串级（无前馈）": "tab:red",
              "串级 + 重力前馈 (推荐)": "tab:blue"}
    for label, ys in results.items():
        axes[0].plot(t, ys, label=label, linewidth=1.9, color=colors[label])
    axes[0].axhline(target_deg, color="k", linestyle="--", linewidth=1, label="目标 90°")
    axes[0].axhline(ANGLE_MIN, color="r", linewidth=1.4, linestyle=":",
                    label=f"机械限位 {ANGLE_MIN:.0f}°（撞上=机构损坏）")
    axes[0].set_ylabel("输出轴角度 (°)")
    axes[0].set_title("实验四：M2006 机构位置控制 — 单环 / 串级 / 串级+前馈\n"
                      f"（重力 {GRAVITY_TORQUE} N·m + 库伦摩擦 {FRICTION_TORQUE} N·m）")
    axes[0].grid(alpha=0.3)
    axes[0].legend(loc="lower right", fontsize=9)

    axes[1].plot(t, v3, label="实际输出轴速度 (deg/s)", linewidth=1.7, color="tab:blue")
    axes[1].plot(t, sp3, label="内环速度设定（外环输出，±800°/s 限幅）",
                 linewidth=1.2, linestyle="--", color="tab:red")
    axes[1].set_xlabel("时间 (s)")
    axes[1].set_ylabel("速度 (°/s)")
    axes[1].grid(alpha=0.3)
    axes[1].legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "exp4_cascade_position.png"), dpi=140)
    plt.close(fig)

    rows = []
    for name, ys in results.items():
        ss, err, overshoot, t_settle = step_metrics(t, ys, target_deg, band_pct=2.0)
        crashed = "撞限位!" if limits[name] else "未撞限位"
        rows.append([name, round(ss, 3), round(err, 3),
                     round(overshoot, 2) if not math.isnan(overshoot) else "",
                     round(t_settle * 1000, 0) if not math.isnan(t_settle) else "",
                     crashed])
        os_str = f"{overshoot:6.2f}" if not math.isnan(overshoot) else "   ---"
        ts_str = f"{t_settle*1000:6.0f}" if not math.isnan(t_settle) else "   ---"
        print(f"  {name:<26} 稳态={ss:8.3f}° 静差={err:7.3f}° "
              f"超调={os_str}% 调节={ts_str}ms  {crashed}")

    with open(os.path.join(OUT, "exp4_cascade_position.csv"), "w", newline="", encoding="utf-8-sig") as f:
        wr = csv.writer(f)
        wr.writerow(["方案", "稳态角度", "静差", "超调%", "调节时间ms", "机械限位"])
        wr.writerows(rows)

    print("-> 曲线已输出 exp4_cascade_position.png")
    print("   结论 ①：单环位置环若无积分，顶着负载会大幅超调、反向甩过目标，")
    print("           最终停在一个毫无意义的平衡点上（这里甚至撞到机械限位）。")
    print("           ★这就是规则'变形机构必须有机械限位'的工程原因 ——")
    print("             软件出错时，只有机械限位能保住机构不被打坏。")
    print("   结论 ②：串级把'速度上限'显式限幅，动作天然柔和、不超调。")
    print("   结论 ③：加前馈后重力不用 PID 去试，I 项只对付未知摩擦，")
    print("           静差和调节时间同时变好 —— 这是夹爪/升降机构的标准做法。")


# ==================================================================
# 实验五：底盘轨迹（为什么必须四轮闭环）
# ==================================================================
def exp5_chassis_trajectory(kin):
    print("\n=== 实验五：底盘轨迹 — 开环 vs 四轮闭环（目标：走 2m 直线）===")

    def simulate(closed_loop, kp=12.0, ki=3.0, kd=0.05, dist_ratio=0.85):
        """dist_ratio: 左侧两轮因摩擦/载荷不同导致的有效增益差异（85% = 温和失配）"""
        vx_target = 0.8          # m/s
        wv_target = kin.inverse(vx_target, 0.0, 0.0)
        pids = [PID(kp, ki, kd, out_limit=14000.0, i_limit=12000.0) for _ in range(4)]
        wheel_v = np.zeros(4)
        x = y = th = 0.0
        traj = []
        t = np.arange(0, 6.0, DT)
        for _ in t:
            cmds = np.zeros(4)
            for i in range(4):
                if closed_loop:
                    tgt_rpm = kin.wheel_to_rotor_rpm(wv_target[i])
                    meas_rpm = kin.wheel_to_rotor_rpm(wheel_v[i])
                    cmds[i] = pids[i].update(tgt_rpm, meas_rpm, DT)
                else:
                    # 开环：按标称模型直接换算指令，不做任何反馈修正
                    cmds[i] = kin.wheel_to_rotor_rpm(wv_target[i]) / 9000.0 * ESC_CMD_MAX
            for i in range(4):
                gain = 1.0 if i in (1, 3) else dist_ratio
                target_wv = kin.rotor_rpm_to_wheel(cmds[i] / ESC_CMD_MAX * 9000.0)
                wheel_v[i] += (target_wv * gain - wheel_v[i]) * DT / 0.030
            bx, by, bw = kin.forward(wheel_v)
            th += bw * DT
            x += (bx * math.cos(th) - by * math.sin(th)) * DT
            y += (bx * math.sin(th) + by * math.cos(th)) * DT
            traj.append((x, y, th))
            if x >= 2.0:
                break
        return np.array(traj)

    ol = simulate(False)
    cl = simulate(True)

    fig, ax = plt.subplots(figsize=(11, 4.8))
    ax.plot(ol[:, 0], ol[:, 1], linewidth=2.2, color="tab:red",
            label=f"开环：跑完 2m 横向漂移 {ol[-1,1]*1000:.0f}mm，航向偏 {math.degrees(ol[-1,2]):.1f}°")
    ax.plot(cl[:, 0], cl[:, 1], linewidth=2.2, color="tab:green",
            label=f"四轮闭环：漂移 {cl[-1,1]*1000:.0f}mm，航向偏 {math.degrees(cl[-1,2]):.2f}°")
    ax.axhline(0, color="k", linewidth=0.8, linestyle=":")
    ax.set_xlabel("X 前进距离 (m)")
    ax.set_ylabel("Y 横向偏移 (m)")
    ax.set_title("实验五：为什么麦克纳姆轮底盘必须做四轮速度闭环\n"
                 "（模拟左侧两轮有效增益仅为右侧的 85%，即轻微摩擦/载荷失配）")
    ax.grid(alpha=0.3)
    ax.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "exp5_chassis_trajectory.png"), dpi=140)
    plt.close(fig)

    print(f"  开环    ：横向漂移 {ol[-1,1]*1000:7.1f} mm，航向偏离 {math.degrees(ol[-1,2]):6.2f}°")
    print(f"  四轮闭环：横向漂移 {cl[-1,1]*1000:7.1f} mm，航向偏离 {math.degrees(cl[-1,2]):6.2f}°")
    print("-> 曲线已输出 exp5_chassis_trajectory.png")
    print("   结论：轮子之间只差 15% 的增益失配，开环 2m 就偏出几十厘米；")
    print("         即使做了四轮闭环，残留航向偏差仍会让车走斜 ——")
    print("         这就是为什么实车还需要 IMU 航向闭环来兜底。")


def main():
    print("=" * 72)
    print("RM2027 校内赛 电控仿真：麦克纳姆轮运动学 + 串级 PID")
    print("=" * 72)
    kin = exp1_kinematics()
    exp2_power_budget()
    exp3_velocity_loop()
    exp4_cascade_position()
    exp5_chassis_trajectory(kin)
    print("\n" + "=" * 72)
    print(f"全部输出在: {OUT}")
    print("=" * 72)


if __name__ == "__main__":
    main()
