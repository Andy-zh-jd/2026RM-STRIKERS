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
        /* 正常底盘：不再依赖 SB，右/左摇杆直接驾驶。 */
        ChassisSetVelocity(rcDeadzone(RemoteControl::rcInfo.right_col) * 3.0f,
                           -rcDeadzone(RemoteControl::rcInfo.right_rol) * 3.0f,
                           -rcDeadzone(RemoteControl::rcInfo.left_rol) * 60.0f);
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
