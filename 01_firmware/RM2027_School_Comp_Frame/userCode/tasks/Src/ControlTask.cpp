//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"
#include "RemoteControl.h"
#include <math.h>          /* fabsf */

/* ==========================================================================
 * 2026-11-03 通路修复：**底盘必须跟着 SB 分档**
 *
 * 原来这里写的是"正常底盘：不再依赖 SB，右/左摇杆直接驾驶"，于是**不管 SB 在哪个
 * 档位，右摇杆都在开车**。可机械臂模式 2（SB 下档）里右摇杆上下是**夹爪** ——
 * 结果就是"一边夹爪、一边把车往前推"。
 * 11-03 实测证据（串口同时刻）：grip=58 到位的同一帧，t1..t4 = 94 / 203 / 485 rpm。
 *
 * 现在的分档（与操作习惯、与老工程的实测逻辑一致）：
 *   SB 上档 → 底盘驾驶（右摇杆前后/横移、左摇杆自转）
 *   SB 中档 → 机械臂模式 1（关节 0~3）→ **底盘必须停**
 *   SB 下档 → 机械臂模式 2（夹爪）    → **底盘必须停**
 * 优先级：离线/数据不合理 > SC 下档急停 > 爬台阶 > SB 分档。
 * ========================================================================== */

/* 遥控数据合理性：接收机没对码时 sLeft/sRight 会“恰好”等于 MID_POS。 */
static uint8_t rcDataSane(void)
{
    return (fabsf(RemoteControl::rcInfo.right_col) <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.left_col)  <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.right_rol) <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.left_rol)  <= 1.05f);
}

#define RC_DEADZONE 0.10f

static float rcDeadzone(float v)
{
    if (v > -RC_DEADZONE && v < RC_DEADZONE) return 0.0f;
    return v;
}

/* 底盘驾驶：右摇杆=前后+横移，左摇杆=自转（符号是 10-23 实测标定，别动） */
/* ===== 底盘方向表（+1 / -1）2026-11-14 =====
 * 电机接线整体反了 → 三个轴同时取反即可（等价于所有轮子转速取负）。
 * 只想反某一个轴（比如只有前进/后退反了），就把 ALL 改回 +1，再改对应那一行。
 *
 *   ALL = -1              → 整车反向（前进变后退、左移变右移、自转反向）
 *   ALL = +1, FB = -1     → 只反前后
 *   ALL = +1, LR = -1     → 只反左右平移
 *   ALL = +1, RT = -1     → 只反自转
 */
#define CHASSIS_DIR_ALL   (+1.0f)   /* 整车总方向：接线反了就把这里改成 -1（2026-11-14 已改回）*/
#define CHASSIS_DIR_FB    (+1.0f)   /* 前后     */
#define CHASSIS_DIR_LR    (+1.0f)   /* 左右平移 */
#define CHASSIS_DIR_RT    (+1.0f)   /* 原地自转 */

static void driveChassis(void)
{
    const float fb = rcDeadzone(RemoteControl::rcInfo.right_col) * 3.0f;
    const float lr = -rcDeadzone(RemoteControl::rcInfo.right_rol) * 3.0f;
    const float rt = -rcDeadzone(RemoteControl::rcInfo.left_rol) * 60.0f;

    ChassisSetVelocity(CHASSIS_DIR_ALL * CHASSIS_DIR_FB * fb,
                       CHASSIS_DIR_ALL * CHASSIS_DIR_LR * lr,
                       CHASSIS_DIR_ALL * CHASSIS_DIR_RT * rt);
}

void CtrlHandle()
{
    /* ① 遥控离线 / 数据不合理 → 底盘断电，绝不保持上一次速度 */
    if (!RemoteAlive() || !rcDataSane()) {
        ChassisStop();
        UserStop();
        return;
    }

    /* ② SC 下档 = 急停，优先级最高 */
    if (RemoteControl::rcInfo.sRight == DOWN_POS) {
        ChassisStop();
        UserStop();
        return;
    }

    /* ③ 爬台阶（SA 高）：只有"保持/完成"步骤允许开；
     *    这一段**不能**调 UserStop() —— 那会把 jackEStop 锁存，推杆就再也动不了。 */
    if (JackActive()) {
        if (JackChassisAllowed()) {
            driveChassis();
        } else {
            ChassisStop();
        }
        return;
    }

    /* ④ 正常模式：按 SB 分档。中/下档是机械臂模式 → 底盘必须停。 */
    switch (RemoteControl::rcInfo.sLeft) {
    case UP_POS:                 /* 上档：驾驶 */
        driveChassis();
        UserStop();              /* 机构电机 M2006 / 推杆保持断电 */
        break;

    case MID_POS:                /* 中档：臂模式 1（关节 0~3）*/
    case DOWN_POS:               /* 下档：臂模式 2（夹爪）*/
    default:
        ChassisStop();
        break;
    }
}
