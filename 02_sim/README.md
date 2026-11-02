# 02_sim · B 线（算法）环境

## python/ ← B 线的主力

已经验证能跑（Python 3.12 + numpy 2.5 + scipy 1.18 + matplotlib 3.11）：

```powershell
cd 02_sim\python
python mecanum_pid_sim.py
```

输出在 `python\sim_out\`：3 张 PNG + 3 个 CSV。五个实验分别回答：
1. 麦轮逆解/正解是否闭合（符号对不对）
2. 24V 母线电流预算：23° 坡、20 cm 台阶到底够不够
3. M3508 速度环：为什么必须有 I 项、I 限幅要多小才会"卡死"
4. M2006 位置环：单环撞机械限位、串级+前馈为什么快一倍
5. 开环 vs 四轮闭环：2 m 直线偏多少

`README.md` 里有全部结论表，可以直接当分工会的汇报材料。

## matlab/ ← 只能当参照，跑不了

- 本机是 **MATLAB R2026a（装了）但 Simulink 没装**（`toolbox\simulink` 只有 5.4 MB 的壳，`load_system` 不存在）
- 所以官方那 4 个 `.slx`（simple_PID / single_PID_actual / double_PID_actual / d_filter_demo）**打不开**
- `check_env.m` 会把结论写进 `check_env_out.txt`（UTF-8），跑一次就知道当前状态：
  ```powershell
  cd 02_sim\matlab; matlab -batch check_env
  ```
- 想真跑 Simulink：得用 MathWorks 账号补装（几 GB，`license('test','Simulink')` 返回 1，说明许可在，只是没装文件）。**27 天内不值得**——python 仿真已经覆盖了同样的四个实验，而且结论更细。

### `pid_demo.m` ← 这个能跑（本机 R2026a 实测通过）

交接包带来的纯 MATLAB PID 教学脚本（**不需要 Simulink、不需要任何 Toolbox**），四组演示直接打表：

```powershell
cd 02_sim\matlab; matlab -batch pid_demo
```

实测结果（值得抄进笔记）：

| 演示 | 参数 → 结果 |
|---|---|
| A 速度环 | 纯 P(kp=2) 稳态只到 **66.7**（目标 100）→ I 才消掉；ki=800 时超调 **49.6%** |
| B 位置环 | 纯 P 稳态 **0.743**（目标 1）、超调 100%；加 D(kd=20) 才 0.60 s 到位 |
| C 积分饱和 | 不限积分 → 峰值爆到 **234**（目标 1）且 8 s 回不来；I_max=0.15 → 峰值 1.15 |
| D 限幅的代价 | 有扰动 d=0.3 时，I_max=0.05/0.15/0.40 → 稳态误差 **8.3%/5.0%/0.3%** |

> ⚠️ 演示 C 的结论文字写"峰值冲到约 6"，但表格算出来是 234。**看表**，文字那句是早期版本的残留。

## 结论先记这四条

| 现象 | 真实原因 |
|---|---|
| 车压到地毯上速度就掉 | 纯 P 没有 I，静差是物理必然 |
| 加了 Ki 但静差还在 | `i_limit` 设太小，I 项被限幅"卡住"（要 ≥ 稳态指令，约 6000/16384） |
| 夹爪到位又甩回去、甚至撞限位 | 位置环用了单环纯 P+D；要串级（外环位置→内环速度）+ 重力前馈 |
| 直走 2 m 偏几十厘米 | 轮子之间 15% 增益差异；四轮闭环只能压到 48 mm，还要 IMU 航向兜底 |

> ⚠️ **仿真参数不能直接抄进固件**：框架的积分项是 `errSum += err`，**没有乘 dt**（`Motor.cpp:22`），
> 1 kHz 下积分速度是常规写法的 1000 倍 → 固件里的 `ki` 量级应该是 **0.1 ~ 1.0**（不是 10）。
> 而且 `ki` 和 `componentKiMax` **必须一起改**：限幅公式是 `INRANGE(errSum, ±componentKiMax/ki)`，
> 只改 `ki` 不改 `componentKiMax`（默认 0）→ 积分被压成 0，等于没加 I。
