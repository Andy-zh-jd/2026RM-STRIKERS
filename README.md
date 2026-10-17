# 交龙校内赛 · 电控工作区

人在一起，就按顺序串行推 —— 文件夹是按**内容**分的，不是按人分的。一个人推的时候，另一个盯着手册和现象，别抢同一块板子。

| 文件夹 | 是什么 | 谁 |
|---|---|---|
| `01_firmware\RM2027_School_Comp_Frame` | 官方框架的本地副本，已配成 **VS Code + GCC/CMake + pyOCD** 一键编译烧录（Ctrl+Shift+B 编译） | A 线 |
| `02_sim\python` | 麦轮运动学 + PID 五个仿真实验，跑 `python mecanum_pid_sim.py` 出图 | B 线 |
| `02_sim\matlab` | 官方 4 个 PID 模型（**本机没装 Simulink，打不开**，只作对照） | B 线 |
| `03_docs` | 本地构建说明、麦轮逆解粘贴版、上车对符号清单、**电池上电第一坑** | 三人 |
| `04_handoff` | **2026-10-17 另一台机器（Mac+Parallels）的交接包**：HANDOFF.md 主文档、当期源码、trandscript | 归档只读 |
| `tools` | `verify-env.ps1` 一键自检、`install-keil.ps1` 装 Keil（需管理员） | 三人 |

**桌面那份 `交龙校内赛` 是官方原件 + 培训资料，只读，不要在里面改代码。** 所有改动都在这里，进 Git。

## 三条命令

```powershell
# 1) 环境自检（任何时候怀疑环境有问题，先跑这个）
powershell -ExecutionPolicy Bypass -File tools\verify-env.ps1

# 2) 编译固件（也可以在 VS Code 里 Ctrl+Shift+B）
cd 01_firmware\RM2027_School_Comp_Frame; cmake -G Ninja -B build; cmake --build build

# 3) 烧录（C 板 + ST-Link 插好后）
cd 01_firmware\RM2027_School_Comp_Frame; pyocd flash -t stm32f407ighx build\RM_Frame_C.hex

# 4) 跑仿真
cd 02_sim\python; python mecanum_pid_sim.py
```

## 已验证的事实（更新到 2026-10-17）

- Arm GNU Toolchain 14.2 + CMake 4.4.4 + Ninja + pyOCD 0.45 + Python 3.12（numpy/scipy/matplotlib/pyserial）都在 PATH 上
- 官方框架用 GCC 编译通过：基线 **FLASH 41 840 B / RAM 16 656 B**
- **合并交接包后仍然编译通过：FLASH 46 564 B / RAM 17 112 B**（+4.7 KB 是 BMI088 IMU 驱动 + UsbPrintf）
- `pyocd` 认得到 C 板的芯片型号 `stm32f407ighx`
- MATLAB R2026a 有，**Simulink 没装**（目录只有 5.4 MB 的壳），4 个 `.slx` 打不开
- Keil MDK **未装**（本机没有管理员权限，装不了；见 `tools\install-keil.ps1`）
- 固件 46.6 KB > MDK-Lite 的 32 KB 上限，就算装了 Keil 也得走 License，所以主用 GCC 链
- **Git 只装在 WSL 里**（Windows 侧没有 `git` 命令）。仓库已建好，用 `wsl -- git log` 看；装 Git for Windows 需要管理员
- 与交接包对比：**9 个源码文件里只有 2 个不同** —— `UserTask.cpp`（IMU 全套）和 `Device.cpp`（启用了 IWDG）。其余字节相同，说明两边改的是同一份框架，可以安全互相同步

## 当前真实进度（以交接包 + 本地为准）

| 模块 | 状态 |
|---|---|
| 环境（Keil/ST-Link/串口） | ✅ 在 Mac+Parallels 那台机器上全通（本机走 GCC 链，等效可用）|
| 串口打印 `UsbPrintf` | ✅ 已验证（6 秒 806 字节、tick 对齐）|
| BMI088 IMU + 低通 + 零偏标定 | ✅ 已完成（加速度 0.9951 g，陀螺噪声 σ 4.4→0.8 °/s）|
| PID 理论 | ✅ 已学完（`04_handoff\artifacts\pid_demo.m`，纯 MATLAB 四组演示）|
| 看门狗 | ⚠️ 已启用，但**喂狗条件写反了** → 见 `03_docs\电池上电第一坑-喂狗条件.md` |
| 底盘解算 `WheelsSpeedCalc()` | ❌ 还是四个 0（纸笔 20 分钟的事，**下一步第一件**）|
| 电机 CAN / 转动 | ❌ 一步未动（原来卡在没有电池和夹具；**电池已解决**）|
| 遥控器 DBUS | ❓ 遥控器在借用清单里，但尚未验证解析 |

## 已知待办

- [x] **电池**：能借到 —— 但要当场确认三件事：① 是不是 TB47 这类大疆**智能电池**（普通航模平衡充充不了）；② **专用充电器是不是一起借**（这条最容易漏，漏了车动不了）；③ 接口是 XT90 还是别的
- [x] **板子数量**：1 块 C 板够用 —— 不用再买 F407 板了（人在一起，不需要并行抢板）
- [x] **喂狗条件写反** → 已修（`Device.cpp:202` 改成无条件喂狗，电压判断挪出去），并在打印里加了 `vbat=`。推理过程见 `03_docs\电池上电第一坑-喂狗条件.md`
- [ ] 填 `WheelsSpeedCalc()`（不需要任何硬件，粘贴版在 `03_docs\A线-麦轮逆解粘贴版.md`）
- [ ] 电机 CAN 反馈：**拔掉三相动力线**，只接编码器线 + CAN + 24V，电调设 ID=5，手转电机轴看 `feedback.angle/speed`。接线见 `03_docs\接线-台架与整车.md`
- [ ] 电机真转：优先把 M2006 装到车架上（车架就是最好的夹具）；`SetTargetSpeed(5)` 若电机本体跟着扭 = 固定不合格，立即断电
- [ ] 插板 + ST-Link 跑一次 `pyocd flash`（本机烧录链路仍未验证）
- [ ] 装 Git for Windows（需要管理员），否则只有 WSL 里能提交
- [ ] **Parallels Pro 试用 2026-10-27 到期**（10 天）—— 到期后那台机器上的 Keil 环境会停摆，代码务必以本仓库为准
- [ ] 台架测试可以电池直连，但**最终供电链必须是 电池 → 裁判系统模块 → 分配**（检录会查），别到最后才发现要重画分电板
