# 交龙校内赛 · 电控工作区

三个人三条线，各自在一个文件夹里干活，别交叉改。

| 文件夹 | 是什么 | 谁 |
|---|---|---|
| `01_firmware\RM2027_School_Comp_Frame` | 官方框架的本地副本，已配成 **VS Code + GCC/CMake + pyOCD** 一键编译烧录（Ctrl+Shift+B 编译） | A 线 |
| `02_sim\python` | 麦轮运动学 + PID 五个仿真实验，跑 `python mecanum_pid_sim.py` 出图 | B 线 |
| `02_sim\matlab` | 官方 4 个 PID 模型（**本机没装 Simulink，打不开**，只作对照） | B 线 |
| `03_docs` | 本地构建说明、麦轮逆解粘贴版、上车对符号清单 | 三人 |
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

## 已验证的事实（2026-10-12）

- Arm GNU Toolchain 14.2 + CMake 4.4.4 + Ninja + pyOCD 0.45 + Python 3.12（numpy/scipy/matplotlib/pyserial）都在 PATH 上
- 官方框架用 GCC 编译通过：**FLASH 41 840 B / RAM 16 656 B**，向量表 SP=0x20020000、Reset=0x0800719D（Thumb 位正确）
- `pyocd` 认得到 C 板的芯片型号 `stm32f407ighx`
- MATLAB R2026a 有，**Simulink 没装**（目录只有 5.4 MB 的壳），4 个 `.slx` 打不开
- Keil MDK **未装**（本机没有管理员权限，装不了；见 `tools\install-keil.ps1`）
- 固件 41.8 KB > MDK-Lite 的 32 KB 上限，就算装了 Keil 也得走 License，所以主用 GCC 链
- **Git 只装在 WSL 里**（Windows 侧没有 `git` 命令）。仓库已经建好并提交过一次，用 `wsl -- git log` 看；要装 Git for Windows 需要管理员

## 已知待办

- [ ] 装 Git for Windows（管理员）或用 WSL 的 git 提交，否则三个人没法协作
- [ ] 插上 C 板 + ST-Link 跑一次 `pyocd flash`，验证烧录链路（编译链已验证，烧录未验证）
- [ ] 买电池 + 大疆专用充电器（借用清单里没有！）
- [ ] 再买 1–2 块 F407 最小系统板 + ST-Link，让三个人能同时写代码
