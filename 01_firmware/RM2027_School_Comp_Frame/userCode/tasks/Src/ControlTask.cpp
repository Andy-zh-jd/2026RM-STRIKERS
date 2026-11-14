//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"
#include "RemoteControl.h"

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

/* 只保留 SA / SD 两个推杆开关：
 *   SA 低 = 正常底盘；
 *   SA 高 = 爬台阶，SD 每翻一次进入下一步。
 * 底盘在“保持”步骤允许用摇杆行驶，因此抬起来以后可以开上台阶。 */
void CtrlHandle()
{
    if (!RemoteAlive() || !rcDataSane()) {
        ChassisStop();
        UserStop();
        return;
    }

    /* SC 下档仍然是急停；如果这台车没装 SC，保持中位即可。 */
    if (RemoteControl::rcInfo.sRight == DOWN_POS) {
        ChassisStop();
        UserStop();
        return;
    }

    if (!JackActive()) {
        /* ===== 2026-11-14 修：操作机械臂时轮子跟着转 =====
         * 原因：合并后的版本让底盘"不看 SB，摇杆直接驾驶"，而机械臂也用同一批摇杆
         *       （SB 中档 → 底座/大臂/小臂/腕俯仰；SB 下档 → 腕旋转/夹爪）。
         *       于是推杆操作机械臂时，底盘同时被同样的摇杆驱动 → 轮子跟着转。
         * 修法：SB 当"模式开关"，两套执行机构互斥：
         *       SB 上档(97)  = 驾驶模式：底盘动，机械臂不动
         *       SB 中档(0)   = 机械臂模式 1：底盘强制停
         *       SB 下档(158) = 机械臂模式 2：底盘强制停
         * 机械臂那边（UserHandle→ArmControl）本来就是按 SB 中/下档分辨的，不用改。 */
        if (RemoteControl::rcInfo.sLeft == UP_POS) {
            ChassisSetVelocity(rcDeadzone(RemoteControl::rcInfo.right_col) * 3.0f,
                               -rcDeadzone(RemoteControl::rcInfo.right_rol) * 3.0f,
                               -rcDeadzone(RemoteControl::rcInfo.left_rol) * 60.0f);
        } else {
            ChassisStop();          /* SB 中/下 = 机械臂模式，底盘必须停 */
        }
        UserStop();                 /* 推杆、M2006 全部停 */
        return;
    }

    /* 爬台阶：只有“保持/完成”步骤允许底盘动；伸/缩过程中底盘必须停。 */
    if (JackChassisAllowed()) {
        ChassisSetVelocity(rcDeadzone(RemoteControl::rcInfo.right_col) * 3.0f,
                           -rcDeadzone(RemoteControl::rcInfo.right_rol) * 3.0f,
                           -rcDeadzone(RemoteControl::rcInfo.left_rol) * 60.0f);
    } else {
        ChassisStop();
    }
}
