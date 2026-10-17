# 01_firmware · 本地构建说明（GCC 链）

这是官方框架 `RM2027_School_Comp_Frame` 的工作副本。桌面那份是原件，别改。

## 怎么编

```powershell
cd 01_firmware\RM2027_School_Comp_Frame
cmake -G Ninja -B build          # 只需第一次，或删过 build/ 之后
cmake --build build
```

或者在 VS Code 里打开这个文件夹，`Ctrl+Shift+B`（任务里还有 flash / erase / reset / 串口监视器）。

产物：`build\RM_Frame_C.elf / .hex / .bin / .map`。当前基线 **FLASH 46 564 B（4.44%）、RAM 17 112 B（13.06%）**（含交接包合并进来的 BMI088 IMU + UsbPrintf；纯框架基线是 41 840 B / 16 656 B）。

> 2026-10-17：已把交接包 `04_handoff\code\` 的 `UserTask.cpp`（IMU 全套）和 `Device.cpp`（启用 IWDG）合并进来，**GCC 链下零错误编译通过**。其余 7 个文件与交接包字节相同。

## 怎么烧

```powershell
pyocd flash -t stm32f407ighx build\RM_Frame_C.hex
```
`stm32f407ighx` 就是 C 板的 **STM32F407IGH6**。没有探针时 `pyocd list` 会说 No available debug probes。

## 我对 CMakeLists.txt 动了 3 处（都写了注释，别删）

原因是工程路径 `F:\Program Files\交龙校内赛\...` **既含空格又含中文**，而 CubeMX 生成的 CMakeLists 把绝对路径直接喂给 `ld` 和 `objcopy`：

| # | 原来 | 改成 | 不改会怎样 |
|---|---|---|---|
| 1 | `-T ${LINKER_SCRIPT}`（绝对路径） | `-T ../STM32F407IGHx_FLASH.ld`（相对 build 目录） | `cannot open linker script file F:/Program` |
| 2 | `-Wl,-Map=<绝对路径>` | `-Wl,-Map=RM_Frame_C.map` | 同样的空格截断 |
| 3 | objcopy 用 `$<TARGET_FILE:...>`（绝对路径） | 用 `$<TARGET_FILE_NAME:...>` + 文件名 | 中文路径变乱码：`No such file or directory` |

顺带把编译优化加了 `-O2`（原来没有，-O0 下软浮点跑 1 kHz 闭环会吃力），并在 POST_BUILD 里加了 `arm-none-eabi-size`，编完直接看到占用。

⚠️ **如果以后用 STM32CubeMX 重新生成代码，这 3 处会被覆盖**（它按 `CMakeLists_template.txt` 重写）。重新生成后照上表再改一遍，或者先别动 `.ioc`。

## Keil 的事

- 工程文件在 `MDK-ARM\RM_Frame_C.uvprojx`，本机**没装 Keil**（当前账户不是管理员，装不了；`tools\install-keil.ps1` 会拉 UAC）
- 就算装了：**MDK-Lite 限 32 KB，而本固件 46.6 KB**，不买 License 只能看代码不能链接
- 结论：**日常用 GCC 链**，Keil 只作对照/应急

## TODO 在哪（框架留的空 + 交接后的现状）

| 文件 | 现状 | 顺序 |
|---|---|---|
| `userCode\tasks\Src\ChassisTask.cpp` → `WheelsSpeedCalc()` | ❌ 四个轮速还是 `0` | **第 1 个填**（无需硬件；粘贴版在 `03_docs`） |
| `userCode\tasks\Src\UserTask.cpp` → `UserInit()` / `UserHandle()` | ⚠️ **IMU 已占用这两个函数**（BMI088 + 标定 + 100ms 打印）；机构代码要**加在它旁边**，别覆盖 | 第 2 个 |
| `userCode\tasks\Src\ControlTask.cpp` → `CtrlHandle()` 的 `MID_POS`/`DOWN_POS` | ❌ 空 | 第 3 个（等机构能动了再定） |
| `userCode\devices\Src\Device.cpp:203` 喂狗条件 | ⚠️ `if (vccBat < 10)` 逻辑反了，上电池会 1 秒复位 | **上电池前必看** `03_docs\电池上电第一坑-喂狗条件.md` |

粘贴版逆解见 `03_docs\A线-麦轮逆解粘贴版.md`；串口打印怎么开见 `03_docs\A线-串口打印printf.md`（交接包里已经用 `UsbPrintf` 实现了，现在不用自己写 `_write`）。
