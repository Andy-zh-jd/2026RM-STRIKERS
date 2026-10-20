# 麦轮逆解：已落地（2026-10-20）

> ✅ **已经写进 `userCode\tasks\Src\ChassisTask.cpp` 并双链编译通过**（GCC 46 788 B / Keil Code 39 224 B，0 error 0 warning）。
> 同时把 `reductionRatio` 从 19.0 改成 **19.2032**（官方 3591/187）。
> 数值自检：直行 → 四轮 +1.000 m/s；横移 → `-1,+1,+1,-1`；自转 60 rpm → `±2.45 m/s`、输出轴 `±306.9 rpm`。

实际代码：

```cpp
    // k = 旋转时轮子到几何中心的等效力臂，m
    const float k = (TRACK_WIDTH + WHEEL_BASE) * 0.5f;      // (0.40+0.38)/2 = 0.39 m

    // 约定：前进 +，左移 +，逆时针（俯视）转 +
    CMFLSpeed = fbVelocity - lrVelocity - k * rtVelocity;   // 左前
    CMFRSpeed = fbVelocity + lrVelocity + k * rtVelocity;   // 右前
    CMBLSpeed = fbVelocity + lrVelocity - k * rtVelocity;   // 左后
    CMBRSpeed = fbVelocity - lrVelocity + k * rtVelocity;   // 右后
```

⚠️ **顺带发现一个要调的数**：`ControlTask.cpp` 里满杆系数是 `4.2`（= 4.2 m/s），但 **M3508 输出轴空载只有 482 rpm ≈ 3.85 m/s**。
满杆 4.2 m/s → 输出轴 526 rpm、转子 10 101 rpm，**超过电机能力**，PID 会长期顶在 16384 饱和（表现为"推满杆响应发死"）。
建议先把系数改成 **3.0** 左右调，底盘稳了再往上加。

单位：`fbVelocity/lrVelocity` 是 m/s（`ControlTask.cpp` 里 `right_col*4.2` 的 4.2 就是"满杆 = 4.2 m/s"），
`rtVelocity` 进函数时是 RPM，函数第一行 `RPM2RADpS()` 之后是 rad/s —— 所以进 k 之前就已经对上了。
后面那四行 `/(WHEEL_DIAMETER/2)` 是线速度→角速度，`RADpS2RPM` 转成输出轴 RPM，`Motor` 内部再乘减速比，不用你管。

## 上车对符号（5 分钟，别跳过）

在 `ControlTask.cpp` 的 `UP_POS` 分支里，先把三个输入单独给，一次只给一个：

| 测试 | 给什么 | 车应该怎么动 | 不符合怎么办 |
|---|---|---|---|
| 1 | `ChassisSetVelocity(0.3, 0, 0)` | 直着前进 | 后退 → 四个轮子符号全反（电机方向整体反） |
| 2 | `ChassisSetVelocity(0, 0.3, 0)` | 向左平移 | 向右 → 只翻 `lrVelocity` 那四项的符号 |
| 3 | `ChassisSetVelocity(0, 0, 600)` | 逆时针自转 | 顺时针 → 只翻 `k*rtVelocity` 那四项的符号 |
| 4 | 三个都给 | 斜着走不跑偏、自转时不平移 | 某个轮反了 → 单独翻那一个轮（通常是那个电机的 CAN ID 接错位） |

**测试前先把车垫起来（轮子悬空）**，用 `printf` 打四个 `state.speed` 看方向，比看车动安全得多。

## 三个会咬人的细节

1. **轮径要用卡尺实测**。`WHEEL_DIAMETER` 现在是 `0.1525f`（152.5 mm）——这是估计值，量出来差 2 mm，走 5 m 就差 6 cm。
2. **减速比填错了**。`ChassisTask.cpp` 里 `reductionRatio = 19.0f`，真实值 3591/187 ≈ **19.2032**，差 1.07% —— 导致"车走直线但距离总差一点"。改成 `19.2032f`。
3. **符号别在车上瞎试**。先在 `02_sim\python` 里跑实验一（运动学自检，逆解→正解闭合）确认矩阵对，再上车只对"电机方向"这一件事。

## 配套的自检

```powershell
cd 02_sim\python; python mecanum_pid_sim.py
```
实验一打印逆解/正解闭合误差（应 < 1e-9）和各工况的轮速、转子 RPM；实验五告诉你为什么必须四轮闭环（开环 2 m 偏 424 mm）。
