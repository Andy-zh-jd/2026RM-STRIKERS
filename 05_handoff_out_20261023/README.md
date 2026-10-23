# RM2027 校内赛 · 电控交接包（出站）

> 打包时间：**2026-10-23 20:00** ｜ 来源机器：Windows 工作区 `F:\Program Files\交龙校内赛`
> 源码版本：git `b4f1f4e`（工作区干净，HEAD == 板上固件对应的源码）
> 用途：把当前进度、可烧固件、源码、踩坑记录交给**另一个智能体 / 队友**继续推进

## 30 秒速览

| 项 | 状态 |
|---|---|
| 水平 | **2.5 级 / 6 级**：M0 闭环 ✅、M1 遥控 ✅、M2 单电机 ✅；四轮落地走、抓取、上台阶都没做 |
| 固件 | GCC 链 **46 848 B**（-O2）；Keil 链 39 112 B 但卡在 Community 32 KB 上限 |
| 板上状态 | **`DBG_MINIMAL 1`**：不读 IMU（会拖死 USB），CAN/电机/遥控/打印全部正常 |
| 最大卡点 | IMU 的 SPI 读在 1 ms 中断里死等 → USB 打不开；SysTick 优先级已提到 0 仍未根治 |
| 下一步 | ①4 轮接上走起来（M3）②15 cm 冲坡实测 ③夹爪 ④检录预演 |

## 先读哪个

| 顺序 | 文件 | 说明 |
|---|---|---|
| **1** | **`STATUS.md`** | ⭐ 主文档：已验证 / 卡点 / 关键数字 / 风险 / 待办 |
| 2 | `firmware/烧录与校验.md` | 这个 hex 怎么烧、烧完怎么判断对 |
| 3 | `docs/实测记录.md` | 每次进实验室的真实读数（最有价值的原始证据）|
| 4 | `docs/总计划.md` | 目标分层 / M0–M6 闸门 / 砍功能红线（**§1 现状盘点已过时，以 STATUS.md 为准**）|
| 5 | `docs/代码架构详解.md` | 4 层架构、1 ms 中断、三个入口函数 |
| 6 | `code/` | 与板上固件一一对应的源码子集 |
| 7 | `history/git-log.txt` | 完整提交历史（每条 commit message 都是一次实验的结论）|

## 目录

```
05_handoff_out_20261023/
├── README.md              ← 你在看的这个
├── STATUS.md              ← 主文档（进度/卡点/数字/风险/待办）
├── MANIFEST.txt           ← 全部文件 SHA256
├── firmware/              ← 可直接烧的固件 + 烧录校验方法
│   ├── RM_Frame_C.hex / .bin / .elf / .map
│   └── 烧录与校验.md
├── code/                  ← 源码子集（userCode 全量 + Inc/Src/startup + CMake + Keil 工程）
├── docs/                  ← 13 篇中文文档 + img/ + 接线图示.html
├── sim/                   ← python 五实验 + matlab pid_demo/check_env
├── tools/                 ← read_serial.py（自动找口）/ read_swd.py（USB 挂了用 SWD 读）/ verify-env.ps1
└── history/git-log.txt
```

## ⚠️ 接手注意三件事

1. **完整可编译工程不在本包里**（Drivers/Middlewares 太大没放）。完整工程在
   `F:\Program Files\交龙校内赛\01_firmware\RM2027_School_Comp_Frame`（含 `build/`，GCC 一键编译）。
   Keil 那侧有个中文路径联接：`D:\RM2027_Frame` → 真实工程目录。
2. **别改 `Src/` 下的 CubeMX 生成文件**（can.c/tim.c/usart.c/usb*）。要改就改 `userCode/`。
3. **烧录前先关串口窗口**：`pyocd flash` 会复位板子 → USB 重新枚举 → 读串口的进程会报
   `ClearCommError / PermissionError 13`；烧完等 2 秒再开。
