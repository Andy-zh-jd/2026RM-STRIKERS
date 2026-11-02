# RM2027 校内赛电控 · 交接包

> 打包时间：2026-10-17
> 用途：把这一轮工作的**进度、成果、踩过的坑**移交给另一个智能体 / 队友

## 先读哪个

| 顺序 | 文件 | 说明 |
|---|---|---|
| **1** | **`HANDOFF.md`** | ⭐ **主文档**。30 秒速览、环境、已完成、8 个已解决的坑、下一步、数值速查 |
| 2 | `code/` | 学生**当前真正的代码**（从 `~/Desktop/Parallels/` 抓的）|
| 3 | `artifacts/` | 本次产物：PID 教学脚本、串口读取脚本、ARM64 ST-Link 驱动 |
| 4 | `session/transcript.md` | 完整对话记录（7800+ 行，48 轮），可搜索定位细节 |
| 5 | `MANIFEST.txt` | 文件清单 + SHA256 |

## 一句话现状

环境全通（Keil + ST-Link + 串口），**IMU 驱动 + 滤波 + 标定已完成并验证**，PID 理论已学完；
**卡在电机环节**——学生没有电池、没有夹具，CAN/电机一步未动。

## 下一步（按优先级）

1. **`WheelsSpeedCalc()` 底盘解算** —— 不需要任何硬件，纸笔就能做完（公式在 `HANDOFF.md` §8.1）
2. **借 24V 可调电源（限流 2 A）读电机 CAN 反馈** —— 拔掉三相线，**不需要夹具**（§8.2）
3. 等有夹具 → 5 rpm 自检 → 30 rpm 正常转（§8.3）

## 目录结构

```
RM2027-handoff/
├── README.md               ← 你在看的这个
├── HANDOFF.md              ← 主交接文档
├── MANIFEST.txt            ← 文件清单 + SHA256
├── artifacts/
│   ├── pid_demo.m                          PID 教学仿真（4 组演示，纯 MATLAB）
│   ├── read_serial.py                      串口读取脚本（自动找设备）
│   └── stsw-link009_v3_arm64_unsigned.zip  Win11 ARM64 的 ST-Link 驱动（必备）
├── code/
│   ├── UserTask.cpp        ← 当前主要工作文件（IMU + 串口打印）
│   ├── ChassisTask.cpp     ← 待填 WheelsSpeedCalc()
│   ├── ChassisTask.h       ← 底盘参数（轮距/轴距/轮径）
│   ├── ControlTask.cpp     ← 遥控器挡位逻辑
│   ├── UserTask.h / Motor.h / Motor.cpp / Device.cpp / main.h
└── session/
    ├── transcript.md       完整对话（含工具调用摘要）
    └── raw/session.jsonl   原始会话日志（JSONL）
```

## ⚠️ 三个最容易踩回旧坑的提醒

1. **ST-Link 驱动必须用 ARM64 版** —— 材料包里的 `stsw-link009.zip` 只有 x86/x64，装了也报代码 52
2. **C 板 LED 是 TIM5 PWM，不是 GPIO** —— `HAL_GPIO_WritePin` 对它无效
3. **改 `ki` 必须同时改 `componentKiMax`** —— 否则积分被压成 0，等于没加 I
