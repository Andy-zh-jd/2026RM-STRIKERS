//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"
#include "RemoteControl.h"
#include <math.h>          /* fabsf：摇杆数据合理性判据 */

/* ==========================================================================
 * 2026-11-03 审计修复（本文件 3 处实质缺陷）
 *
 * 修复前 CtrlHandle 的问题：
 *   ① **没有"遥控在线"判定**：接收机掉线/线掉了以后 rcInfo 停在最后一次的值，
 *      底盘就按那个旧速度一直跑 —— 这是"没人碰车它自己动"的典型来源。
 *   ② **SB 中/下档分支是空的**：`case MID_POS: break;` 什么都不做，于是
 *      ChassisStopFlag 仍是 0、上一条速度继续生效 → 一边操作机械臂一边乱开。
 *      （已经在 10-23 被抓到过一次：单轮调试目标残留导致轮子自转，同一种错法。）
 *   ③ **爬台阶判据用 `> 1000`**：optional[] 是原始 11 bit，中位就是 1024，
 *      于是"开关停在中档"或"通道没配置"都会被判成上档 → 底盘被永久 ChassisStop()。
 *      这是"底盘完全不动、且串口看不出原因"的直接原因。改用 RemoteControl.h 的
 *      RC_SWITCH_HIGH(1400)。
 *
 * 现在的优先级（从上到下，任何一条命中就 return）：
 *   ① 遥控离线 / 数据不合理 → 底盘断电 + 机构卸力
 *   ② SC 下档              → 急停
 *   ③ 爬台阶模式           → 底盘停（机械臂在 ArmControl() 里同样跳过）
 *   ④ SB 上档              → 驾驶
 *   ⑤ SB 中档/下档         → 机械臂模式，底盘必须停
 * ========================================================================== */

/* SA = optional[0]，拨到高 = 爬台阶。阈值见 RemoteControl.h（不能用 1000）。 */
static bool ClimbMode(void)
{
    return RemoteControl::rcInfo.optional[0] >= RC_SWITCH_HIGH;
}

/* 遥控数据合理性：接收机没对码时通道会读到 0 → col = (0-1024)/671 = -1.52。
 * 这种数据不能拿来开车（会把底盘驱动到 -45 rpm）。 */
static uint8_t rcDataSane(void)
{
    return (fabsf(RemoteControl::rcInfo.right_col) <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.left_col)  <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.right_rol) <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.left_rol)  <= 1.05f);
}

/* 摇杆死区：中位偏置实测 q0=39 / q3=47（0.058 / 0.070），不处理会让轮子自己爬 */
static float rcDeadzone(float v)
{
    if (v > -RC_STICK_DEADZONE && v < RC_STICK_DEADZONE) return 0.0f;
    return v;
}

/* 正常驾驶。2026-10-23 实测标定，符号不要随便动：
 *   右摇杆上推 → 前进   ：fb 正 = 前进，一致
 *   右摇杆右推 → 车右移 ：公式里 lr 正 = 向左，**要取负**
 *   左摇杆右推 → 顺时针 ：公式里 ω 正 = 俯视逆时针，**要取负**
 * 满杆 3.0 m/s（原来 4.2）：M3508 输出轴空载只有 482 rpm ≈ 3.85 m/s，
 * 4.2 会让 PID 长期顶在 12000 饱和，表现是"推满杆反而发死"。 */
static void driveChassis(void)
{
    ChassisSetVelocity(rcDeadzone(RemoteControl::rcInfo.right_col) * 3.0f,
                       -rcDeadzone(RemoteControl::rcInfo.right_rol) * 3.0f,
                       -rcDeadzone(RemoteControl::rcInfo.left_rol) * 60.0f);
}

/* 急停/离线锁存：1 = 正在锁存（串口 stp= 字段读它）。
 * 只用来保证"进入急停那一刻"卸一次力，不是每帧都卸。 */
uint8_t g_stopLatch = 0;

void CtrlHandle()
{
    /* 急停/离线只在"进入的那一瞬间"卸一次力。
     * 不能在 1ms 中断里每帧都调 UserStop()：里面 ArmStopAll() 是 6 次软件 I2C 写，
     * 每帧都做会把 1ms 任务拖垮（机械臂卸力由跳变锁存保证）。 */
    /* ① 遥控必须在线且数据合理 —— 否则底盘断电，而不是保持上一次速度 */
    if (!RemoteAlive() || !rcDataSane()) {
        ChassisStop();
        if (!g_stopLatch) { g_stopLatch = 1; UserStop(); }
        return;
    }

    /* ② SC 下档 = 急停，优先级最高，请勿更改 */
    if (RemoteControl::rcInfo.sRight == DOWN_POS) {
        ChassisStop();
        if (!g_stopLatch) { g_stopLatch = 1; UserStop(); }
        return;
    }

    g_stopLatch = 0;

    /* ③ 爬台阶模式：底盘必须停 */
    if (ClimbMode()) {
        ChassisStop();
        return;
    }

    /* ④⑤ 驾驶 / 机械臂模式 */
    switch (RemoteControl::rcInfo.sLeft) {
    case UP_POS:            /* 上档：底盘听摇杆 */
        driveChassis();
        break;
    case MID_POS:           /* 中档：机械臂模式 1（关节 0~3）→ 底盘必须停 */
    case DOWN_POS:          /* 下档：机械臂模式 2（关节 4~5）→ 底盘必须停 */
    default:
        ChassisStop();
        break;
    }
}
