# A 线第一个里程碑：点灯 + 串口打印

官方框架里 **没有 `printf` 重定向**（`syscalls.c` 没实现 `_write`），所以你直接写 `printf` 什么都不会发生。
两条路，任选一条，5 分钟搞定：

## 方案 1：走 USB-CDC（C 板 **micro USB** 口，插电脑就是一个 COM 口）

> 注：C 板是 **micro USB**（手册 P.8），不是 Type-C。Type-C 那个是 **裁判系统模块**上的检录口，别搞混。

`Src\usbd_cdc_if.c` 里已经有 `CDC_Transmit_FS(uint8_t* Buf, uint16_t Len)`。新建一个 `Src\retarget.c`：

```c
#include "usbd_cdc_if.h"
#include <stdio.h>

int _write(int fd, char *ptr, int len) {
    (void)fd;
    CDC_Transmit_FS((uint8_t *)ptr, (uint16_t)len);
    return len;
}
```

在 Keil 工程里把这个文件加进去（GCC 这边 CMake 的 `GLOB Src/*.*` 会自动带上）。
打印：`printf("spd=%d,%d\n", a, b);` —— 电脑上 `python -m serial.tools.miniterm COMx 115200` 就能看。

⚠️ `CDC_Transmit_FS` 是**非阻塞**的：上一包没发完时它直接返回 `USBD_BUSY`，数据就丢了。
调试打印够用；不要放在 1 ms 中断里高频打（会拖慢控制）。要高频就用方案 2 的 UART+DMA。

## 方案 2：走 UART（更稳，适合 1 kHz 打印）

框架里 `USART3` 被遥控器 DBUS 占了（100000 波特，不可动）；**`USART1` 和 `USART6` 已经初始化在 115200，可以直接拿来打印**（见 `Src\usart.c`）。
把 `_write` 指到 `HAL_UART_Transmit(&huart1, ...)`，或者干脆自己写一个：

```c
void dbg(const char *fmt, ...) {
    char buf[128];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, n, 10);
}
```
接线：C 板 UART 的 TX → USB 转 TTL 的 RX（共地），波特率跟 `MX_USART1_UART_Init` 里的一致。

## 点灯

框架里 RGB 灯已经在跑了（`Device.cpp` 的 `aRGB_led_change()`，1 ms 中断里调用，`period` 默认 5000）。
按 C 板的 KEY 键会在 5000/500 之间切换 —— **灯在变色就说明 1 ms 中断活着**，这是最快的"板子活着"判据。

想自己点：`aRGB_led_show(0xff00ff00);`（ARGB，A 是亮度，0xff 最亮）。

## 顺序建议

1. 烧进去 → 灯在变色 → 证明时钟/中断/烧录都对
2. 加 `_write` → `printf("boot %d\n", 123);` → 电脑能看到
3. `printf` 打 `RemoteControl::rcInfo.right_col` → 推杆有数 → **遥控链路通了**
4. 才开始动电机（一个轮子，垫起来）
