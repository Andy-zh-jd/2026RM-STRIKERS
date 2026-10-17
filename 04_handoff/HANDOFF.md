# RM2027 校内赛 · 电控进度交接文档

> 生成时间：2026-10-17
> 项目：上海交通大学 RoboMaster 校内赛（2027 届）电控
> 交接对象：接手本项目的另一个智能体 / 队友
> 配套文件：`session/transcript.md`（完整对话，7800+ 行）、`code/`（当前源码）、`artifacts/`（产物）

---

## 0. 30 秒速览

| 项 | 状态 |
|---|---|
| **学生水平** | 完全新手，第一次接触嵌入式 / 单片机 / 虚拟机 |
| **开发机** | Apple M5 Pro / 24 GB / macOS 27 / Parallels Desktop 27 |
| **已完成** | 环境搭建 → ST-Link ARM64 驱动 → Keil → **串口调试链路** → **IMU 驱动 + 滤波 + 标定** → **PID 理论** |
| **当前卡点** | **没有电池、没有夹具** → 电机（CAN）环节一步未动 |
| **下一步第一件事** | ① 填完 `WheelsSpeedCalc()`（无需硬件）② 借 24V 可调电源读电机反馈 |
| **时间压力** | Parallels Pro 试用 **2026-10-27 到期**，学生版 339 元/年待购 |

---

## 1. 与学生的沟通方式（重要）

- 全程**中文**，学生指令很短（"读一下"= 让我去读串口、"现在呢"= 重试、"好了"= 已完成）
- 是**完全新手**：需要**点击级**的操作步骤，要说明"会看到什么"和"不对怎么办"
- 学生**直觉不错**：曾自己发现"实验2曲线幅度挺小啊"，纠正了我一个错误的仿真预期
- **"读一下"是高频请求** → 用 `artifacts/read_serial.py` 直接读串口，这是最有效的验证手段

---

## 2. 硬件清单

| 硬件 | 状态 | 说明 |
|---|---|---|
| RoboMaster 开发板 C 型 | ✅ 有 | STM32F407IGHx |
| ST-Link 烧写器 | ✅ 有 | **克隆版**，`VID_0483&PID_3748`，产品串 `STM32 STLink`，**无 VCP** |
| micro USB 数据线 | ✅ 有 | 曾经没有，已买到 |
| M3508 电机 + C620 电调 | ✅ 有 | 减速比 19:1，额定 3 N·m，20A |
| M2006 电机 + C610 电调 | ✅ 有 | 减速比 36:1，额定 1 N·m，10A |
| 遥控器 + DBUS 接收机 | ❓ **未确认** | 讲义和手册里有，但没确认学生手上有没有 |
| **电池（6S/24V）** | ❌ **没有** | 学生前后说法不一致（先说"有"，后说没有），需再确认 |
| **夹具 / 台钳** | ❌ **没有** | 电机无法可靠固定 → 禁止做"通电转动"测试 |

---

## 3. 软件环境（含关键路径）

| 项 | 细节 |
|---|---|
| Mac | macOS 27，Parallels Desktop **27.0.3**，授权 **Pro 14 天试用（2026-10-27 到期）** |
| 虚拟机 | `~/Parallels/Windows 11.pvm`，**Windows 11 ARM64**（build 26300），Parallels Tools 已装 |
| VM 配置 | 当前 **4 vCPU / 6 GB**（建议手动改 8 GB；Standard 版上限即 8GB/4vCPU） |
| Keil | **MDK 5.38a**（材料包自带 `MDK538a（Keil安装包）.EXE`），授权用 **MDK-Community 免费版**（非 keygen） |
| 器件包 | `Keil.STM32F4xx_DFP.3.0.0` |
| ST-Link 驱动 | **必须用 ARM64 版**：`stsw-link009_v3_arm64_unsigned`（见 §7.1） |
| 串口 | USB CDC，设备名 `STM32 Virtual ComPort`，PID 0x5740，序列号 207637805948 |
| 串口路径 | `/dev/cu.usbmodem2076378059481`（**每次重插/复位可能变化**，脚本会自动找） |
| MATLAB | R2025b 已装，**Simulink 无授权**（`license('test','Simulink')` 待确认） |

---

## 4. 文件位置（**最容易搞错的地方**）

| 内容 | 路径 | 备注 |
|---|---|---|
| **正在使用的工程** | `~/Desktop/Parallels/RM2027_School_Comp_Frame/` | ⚠️ 在**共享桌面**上编译（Windows 看到的就是它）。曾有"代码改了不生效"的隐患，目前正常 |
| 原始资料包（**只读，别改**） | `~/Desktop/27校内赛电控培训资料/` | 27 届 + 26 届两套，内容高度重复 |
| 原始工程备份（**别改**） | `~/Desktop/27校内赛电控培训资料/27校内赛电控培训资料/RM2027_School_Comp_Frame/` | 与 `26校内赛/` 下的工程**完全相同**（diff 只差学生自己的改动） |
| 本次会话产物 | `~/Documents/deepseek-harness/default-workspace/` | `pid_demo.m`、`tools/read_serial.py` |

> ⚠️ **同名学生有 3 份工程副本**（26届、27届、桌面 Parallels），只有 `~/Desktop/Parallels/` 那份是活的。改代码前务必确认在改这一份。

---

## 5. 已完成的工作（附验证证据）

### 5.1 环境链路（全通）
- Parallels + Win11 ARM64 + Keil 5.38a + Community 授权 + STM32F4 DFP
- 烧写、调试、串口收发全部可用

### 5.2 串口调试链路（已验证）
- `UsbPrintf()`（vsnprintf + CDC_Transmit_FS）正常工作
- 实测：6 秒收到 806 字节 / 31 行，`tick` 从 25259→31259（正好 6000 ms），间隔 200 ms 与代码一致
- 抓包验证过：`key_raw_state` 从 1 变 0（按键中断 → 变量 → USB → 屏幕，全链路）

### 5.3 看门狗（已验证）
- 启用 `MX_IWDG_Init()` 后，`tick` 连续跑到 143 秒无复位
- 踩过"标定期间不喂狗导致 1 秒重启循环"的坑（§7.7）

### 5.4 BMI088 IMU（已完成，质量良好）
| 指标 | 结果 |
|---|---|
| Chip ID | accel `0x1E`、gyro `0x0F`（稳定 12 行）|
| 加速度平方和 | 0.990 ~ 1.000 g，均值 **0.9951**（误差 0.5%）|
| 陀螺仪零偏（标定后）| 均值 −0.14 / −0.35 / −0.14 °/s |
| 陀螺仪噪声（1kHz 采样 + 一阶低通 α=0.9）| σ 从 **4.4 → 0.8~1.1 °/s**（预测 0.78，误差 1%）|
| 采样/打印 | 1 kHz 采样 / 10 Hz 打印（已解耦）|

### 5.5 PID 理论（已学完）
`artifacts/pid_demo.m` 四组演示（纯 MATLAB，不需 Simulink）：
- **A 速度环**：P 留下 33% 稳态误差 → I 消除；I 太大 49.6% 超调
- **B 位置环**：无 D 持续振荡（周期 0.63s ↔ kp=100 可反推）→ D 是刹车
- **C 积分饱和**：不限积分 → 超调 497% 且不收敛；I_max=0.15 → 15%
- **D 限幅的代价**（加扰动 d=0.3）：I_max=0.05/0.15/0.40 → 稳态误差 8.3%/5.0%/0.6%

---

## 6. 当前代码状态（`code/UserTask.cpp`，行号已核对）

```
第   4 行  #include "UserTask.h" / <stdio.h> / <stdarg.h>
第   9 行  UsbPrintf(const char *fmt, ...)           串口打印（vsnprintf + CDC_Transmit_FS）
第  26 行  CS_ACCEL_L/H, CS_GYRO_L/H 宏
第  34 行  IMU_ReadReg(isAccel, reg)                读寄存器（acc 多一个 dummy byte）
第  56 行  IMU_WriteReg(isAccel, reg, data)         写寄存器
第  74 行  IMU_ReadBytes(isAccel, reg, buf, n)
第  96 行  #define ACC_SEN 10920.0f   (±3g)
第  99 行  float g_gyroBias[3] / g_gyroDps[3] / g_accelG[3]
第 103 行  static void IMU_Update(void);            ← 前向声明（必须，否则 105 行报错）
第 105 行  void IMU_Calibrate(void)                 500 次采样求平均（循环内已喂狗）
第 122 行  #define GYRO_LPF 0.90f                  一阶低通系数（1kHz → 约 17Hz）
第 126 行  static void IMU_Update(void)             读六轴 → 去零偏 → 低通 → 换算
第 155 行  PID_Regulator_t userPidRegulator         kp=60, ki=0, kd=0, outputMax=10000
第 165 行  MOTOR_INIT_t userMotorInit               _motorID=MOTOR_ID_1, ratio=36
第 174 行  Motor UserMotor(MOTOR_ID_5, &userMotorInit)   ← 注意：实例用 ID 5
第 187 行  UserInit()   假读切SPI → 唤醒acc(0x7D/0x7C) → 0x40/0x41 → 读ID → IMU_Calibrate()
第 216 行  static char g_line[128]                  拼行缓冲区
第 218 行  AppendF2(name, v)                        追加"名字+两位小数"到 g_line
第 230 行  UserHandle() IMU_Update() 每1ms；每100ms 拼整行一次性打印 gx/gy/gz/ax/ay/az
第 247 行  UserMotor.Handle()   ← 但从未 SetTargetSpeed → stopFlag=1 → 电流输出恒 0
```

**`code/ChassisTask.cpp` 第 81~84 行**：`CMFLSpeed/CMFRSpeed/CMBLSpeed/CMBRSpeed = 0` —— `WheelsSpeedCalc()` 未实现。

**关键：`UserMotor` 已实例化但从未设目标速度，电机处于"停"状态（`stopFlag=1` → 电流指令 0）。**
`userMotorInit._motorID` 写的是 `MOTOR_ID_1` 而实例用 `MOTOR_ID_5` —— 这是框架原有写法（`_motorID` 字段对电机实例不生效，实际 ID 来自构造参数），接手时注意别被误导。

---

## 7. 已踩过并解决的坑（**接手时最有价值的部分**）

### 7.1 ST-Link 在 Win11 ARM64 上装不上（错误代码 52）
- **根因**：材料包里的 `stsw-link009.zip` **只有 x86/amd64 和 x64/x86 的 .cat**，**没有 arm64 目录、没有 arm64 签名**。解压验证过。
- **解法**：用 ST 官方的 **ARM64 unsigned** 包 `stsw-link009_v3_arm64_unsigned.zip`（内含 `arm64/` 目录，三个 INF 都有 `[Standard.NTarm64]` 段）。
  1. `shutdown /r /o` → 疑难解答 → 高级选项 → 启动设置 → 重启 → 按 **7**（禁用驱动签名强制，**单次生效**）
  2. 对 `stlink_dbg_winusb.inf` / `stlink_VCP.inf` / `stlink_bridge_winusb.inf` 逐个右键"安装"（**不要点 .bat**，它调的是 dpinst_amd64.exe）
  3. 设备出现在 **`通用串行总线设备`** 类别（不是"端口"，很多人在这找错）
- **为什么只需关一次**：INF 只是把设备绑到微软签名的 `winusb.sys`（`Include = winusb.inf / Needs = WINUSB.NT`），"未签名"只影响安装那一刻。
- ⚠️ **不要用 `bcdedit /set testsigning on`**（永久水印，还要手动关）。

### 7.2 Parallels 抢占 USB 设备
- **现象**：Mac 上 `ls /dev/cu.*` 看不到串口
- **诊断**：`ioreg -c IOUSBHostDevice -r -n "STM32 Virtual ComPort" | grep UsbExclusiveOwner` → `pid 1158, prl_vm_app`
- **解法**：Parallels 菜单 **设备 → USB 与蓝牙** 取消勾选；并在设置里关掉该设备的"连接时自动连接"
- **工作流**：**ST-Link 给 Windows（烧写），C 板 USB CDC 给 Mac（读串口）**，两者互不干扰

### 7.3 C 板的 LED 不是 GPIO，是 TIM5 PWM（点灯"渐变闪烁"之谜）
- **现象**：学生用 `HAL_GPIO_WritePin/TogglePin` 点灯，看到的是"渐变闪烁"，代码毫无作用
- **根因**：`Src/tim.c` 第 408~419 行把 LED 三脚配成 `GPIO_MODE_AF_PP` + `GPIO_AF2_TIM5`：
  `PH12→TIM5_CH3(红)`、`PH11→TIM5_CH2(绿)`、`PH10→TIM5_CH1(蓝)`
  且 `Device.cpp:191` 每 1ms 调 `aRGB_led_change(period)` 做跑马灯 → 这才是渐变的来源
- **解法**：用工程自带 `aRGB_led_show(0xAARRGGBB)`（Device.cpp:9），并注释掉 `aRGB_led_change(period)`
- ⚠️ `aRGB_led_show` / `bsp_BuzzerOn` / `bsp_BuzzerOff` / `key_raw_state` / `vccBat` **都没有在头文件里声明**，跨文件调用要自己写 `extern`

### 7.4 BMI088 加速度计数据全 0（Chip ID 却正常）
- **根因**：加速度计**上电默认 suspend 模式**，数据寄存器恒 0；但 Chip ID 在 suspend 下也能读
- **解法**：`IMU_WriteReg(1, 0x7D, 0x04)` 打开 → `IMU_WriteReg(1, 0x7C, 0x00)` 切 active → 再设 `0x40=0xA8`、`0x41=0x00`
- **验证方式**：把 `0x7C`/`0x7D` 读回来打印，应显示 `CONF=0x00 CTRL=0x04`

### 7.5 看门狗 1.02 秒打断 IMU 标定 → 板子 1 秒重启循环
- **现象**：串口设备反复出现/消失，周期约 1 秒
- **诊断**：监测 `/dev/cu.usbmodem*` 存在性，实测**周期 ≈ 1.0 s**，与 IWDG 超时完全吻合：
  `T = (8 × 4096) / 32000 = 1.024 s`（Prescaler=8, Reload=0xFFF, LSI≈32kHz）
- **根因**：`MX_IWDG_Init()` 在 `Device.cpp:274`，`UserInit()` 在 `:289`，喂狗代码在 `:203` 但被 `init_Flag==1` 挡住 → **标定期间（约 1.1 s）没人喂狗**
- **解法**：在 `IMU_Calibrate()` 的循环里加 `HAL_IWDG_Refresh(&hiwdg);`
- **教学点**：看门狗不管你在干什么，1 秒没人喂就复位。框架原本把 `MX_IWDG_Init()` 注释掉就是这个原因

### 7.6 串口打印乱序 + 丢换行
- **现象**：`gx=1.79 2.04 4.66 0.00 az=1.00 gx=5.16 ...`，标签大量消失、无 `\r\n`
- **根因**：一行数据被拆成 **7 次** `UsbPrintf`（6 个 `PrintF2` + 1 个换行），而 `CDC_Transmit_FS` 在 `TxState != 0` 时直接返回 `USBD_BUSY` 丢弃数据；USB FS 一个包要 ~1ms
- **解法**：**先拼一整行到 `g_line[128]`，再一次性发送**（`AppendF2()` + `UsbPrintf("%s\r\n", g_line)`）

### 7.7 `PrintF2` 负数丢符号
- 原实现 `ip = s/100; fp = s%100;` 在 `v = -0.5` 时得到 `ip=0, fp=-50→50` → 打印 `0.50`（负号丢失）
- **解法**：先判符号，`if (s<0) { 输出"-"; s=-s; }`

### 7.8 积分限幅把积分掐死（调参必踩）
- 框架 `Motor.cpp:22-24`：
  ```c
  PIDInfo.errSum += PIDInfo.err[3];                       // 注意：没有乘 dt
  INRANGE(PIDInfo.errSum, -componentKiMax/ki, +componentKiMax/ki);
  PIDInfo.componentKi = PIDInfo.errSum * PIDInfo.ki;
  ```
- **坑**：`ChassisTask.cpp` 里 `componentKiMax = 0`。**只把 `ki` 改大、不改 `componentKiMax`** → `errSum` 被压成 0 → **积分永远为 0**
- **另一坑**：`errSum += err` **没有乘 dt**，积分速度是常规写法的 1000 倍（1kHz）→ 代码里的 `ki` 量级应为 **0.1 ~ 1.0**（对应常规 100~1000）

### 7.9 仿真"模型陷阱"（学生自己发现的）
- 演示 C 用理想双积分器 `θ''=u`，**静止不需要任何输出就能保持位置** → 积分限幅给小了也看不出问题
- 加恒定扰动 `d=0.3` 后，I_max=0.05 才暴露出 **8.3% 稳态误差**
- **教训**：仿真里没写进去的物理效应，就不会在结果里出现

---

## 8. 未完成的工作与下一步

### 8.1 【可立即做，无需硬件】`WheelsSpeedCalc()` 底盘解算 ⭐
文件：`code/ChassisTask.cpp`（当前四个轮速全为 0）

已知参数（`ChassisTask.h`）：
```c
#define WHEEL_DIAMETER  0.1525f   // 麦轮直径 m
#define TRACK_WIDTH     0.40f     // 轮距 m
#define WHEEL_BASE      0.38f     // 轴距 m
// 关键常数 k = (WHEEL_BASE + TRACK_WIDTH)/2 = 0.39
```

待填的四行（麦轮逆运动学，单位 m/s，ω 单位 rad/s）：
```c
float k = (WHEEL_BASE + TRACK_WIDTH) / 2.0f;   // 0.39
CMFLSpeed = fbVelocity - lrVelocity - k * rtVelocity;
CMFRSpeed = fbVelocity + lrVelocity + k * rtVelocity;
CMBLSpeed = fbVelocity + lrVelocity - k * rtVelocity;
CMBRSpeed = fbVelocity - lrVelocity + k * rtVelocity;
```
后面的 `÷(WHEEL_DIAMETER/2)` 和 `RADpS2RPM()` 框架已写好，不要动。

**三个纸笔自检**（写进代码前先手算核对）：
| 输入 | 期望四轮 |
|---|---|
| `fb=1, lr=0, ω=0` | 全 `+1.0`（直行）|
| `fb=0, lr=1, ω=0` | `-1, +1, +1, -1`（横移）|
| `fb=0, lr=0, ω=1` | `-0.39, +0.39, -0.39, +0.39`（原地转）|

**符号需实车校准**（`fb`/`lr`/`ω` 的正方向因队伍约定而异）。

### 8.2 【需要 24V 电源】读电机 CAN 反馈
- 没有电池 → **用实验室可调直流电源：24 V / 限流 2 A**（限流比电池安全得多）
- **不需要夹具**：拔掉三相动力线，只接 `7-pin 编码器线 + CAN + 电源`，电机物理上不可能转
- 电调设 **ID = 5**（对应 `UserMotor` = `MOTOR_ID_5`，CAN1 / 0x1FF）
- 期望：手转电机轴时 `feedback.angle`（0~8191）和 `feedback.speed` 变化
- 打印代码见 `session/transcript.md` 对应轮次

### 8.3 【需要夹具】证明电机能转
- 优先把 M2006（1 N·m，更安全）装到车架上——**车架本身就是最好的夹具**
- 或借台钳 / 买 G 字夹（20~30 元）
- 自检：`SetTargetSpeed(5)`，若**电机本体跟着扭** → 固定不合格，立即断电

### 8.4 【需要遥控器】DBUS 解析
- 讲义 `UART 遥控器.pdf`，对应 `RemoteControl.cpp`
- **未确认学生是否有遥控器 + 接收机**

### 8.5 学习路线剩余部分
- `CAN.pptx`（电机通信原理）
- `UART 遥控器.pdf`
- `RTOS.pptx`（讲义自己说"混个脸熟即可"）
- PID 已学完（§5.5）

---

## 9. 待确认 / 风险

| 项 | 状态 | 影响 |
|---|---|---|
| 到底有没有电池 | 学生前后说法矛盾 | 决定用电池还是可调电源 |
| 有没有遥控器 + DBUS 接收机 | 未知 | 决定能否做遥控器任务 |
| Parallels 授权 | 试用 2026-10-27 到期 | 到期后虚拟机不能用 |
| 工程放在共享桌面编译 | 目前正常 | 将来若出现"改了代码不生效"，第一嫌疑 |
| M2006 的 `userMotorInit._motorID = MOTOR_ID_1` 但实例用 `MOTOR_ID_5` | 代码里不一致 | `SetTargetSpeed` 前需确认 CAN ID 配置是否正确（ID 5 → 0x1FF）|
| 车架实际轮距/轴距 | 用的是默认值 0.40/0.38 | 需向结构队友确认 |

---

## 10. 关键数值速查

### C 板引脚（来自用户手册 + `main.h`）
| 功能 | 引脚 |
|---|---|
| LED | LED_R=**PH12**、LED_G=**PH11**、LED_B=**PH10**（TIM5_CH3/2/1，**PWM 驱动**）|
| 用户按键 KEY | **PA0**（按下=低电平，10K 上拉，EXTI 上升+下降沿）|
| 蜂鸣器 | **PD14 = TIM4_CH3**，无源，额定 4000 Hz |
| SPI1（BMI088）| CLK=PB3、MISO=PB4、MOSI=PA7、CS_ACCEL=PA4、CS_GYRO=PB0 |
| 电压检测 | ADC_BAT = PF10 |
| SWD | J8：A=SWDIO、B=SWCLK、C=GND、D=3.3V |

### BMI088 寄存器
| 项 | 地址 / 值 |
|---|---|
| acc Chip ID | 0x00 → **0x1E** |
| gyro Chip ID | 0x00 → **0x0F** |
| acc 数据 | **0x12~0x17**（小端，低字节在前）|
| gyro 数据 | **0x02~0x07** |
| acc 唤醒 | 0x7D=0x04，0x7C=0x00 |
| acc 配置 | 0x40=0xA8（ODR 800Hz）、0x41=0x00（±3g）|
| 量程系数 | ACC_SEN=**10920** LSB/g（±3g）、GYRO_SEN=**16.384** LSB/(°/s)（±2000dps）|
| SPI 模式 | Mode 3（CPOL=1, CPHA=2edge），8bit，MSB first，软 NSS，预分频 256（~328 kHz）|
| 读时序 | **加速度计多 1 个 dummy byte**（数据在 `rx[2+]`）；陀螺仪无 dummy（`rx[1+]`）|
| 模式切换 | 加速度计上电是 **I2C**，需先做一次"虚假读取"把 CS 拉出上升沿才切到 SPI |

### CAN 映射（`Motor.h` / `Motor.cpp`）
| 代码 ID | CAN 总线 | 发送帧 | 对应电调 ID |
|---|---|---|---|
| `MOTOR_ID_1~4` | CAN1 | **0x200** | 1~4（底盘）|
| `MOTOR_ID_5~8` | CAN1 | **0x1FF** | 5~8（机构）|
| `MOTOR_ID_9~12` | CAN2 | 0x200 | 1~4 |
| `MOTOR_ID_13~16` | CAN2 | 0x1FF | 5~8 |

反馈帧 0x201~0x208，代码用 `StdId - 0x201` 定位。
`state.speed = feedback.speed / reductionRatio`（M3508=19，M2006=36）。
`stopFlag` 默认 1 → `motor_intensity = 0`（安全默认）。

### PID 参数现状
```c
// ChassisTask.cpp：4 个 M3508
.kp=60, .ki=0, .kd=0, .componentKiMax=0, .outputMax=16384
// UserTask.cpp：1 个 M2006
.kp=60, .ki=0, .kd=0, .componentKiMax=0, .outputMax=10000
```
**注意**：`ki` 与 `componentKiMax` 必须一起改（§7.8）。

---

## 11. 如果要复现整条链路的验证

```bash
# 1. 看串口设备在不在（Mac 侧）
ls /dev/cu.usbmodem*

# 2. 读串口（自动找设备，Ctrl+C 退出）
python3 artifacts/read_serial.py
python3 artifacts/read_serial.py -t 5     # 只读 5 秒

# 3. 看设备是否被 Parallels 抢占
ioreg -c IOUSBHostDevice -r -n "STM32 Virtual ComPort" | grep UsbExclusiveOwner
#    → 若出现 "prl_vm_app"，去 Parallels 菜单 设备→USB与蓝牙 取消勾选

# 4. Windows 侧编译烧写：Keil 打开工程 → F7 编译 → F8 下载
```

⚠️ 读串口时若报 `Resource busy`：通常是板子刚复位正在重新枚举，等 1~2 秒重试。
