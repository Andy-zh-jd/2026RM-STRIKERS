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
/* 2026-11-16：摇杆曲线指数项（0 = 纯线性，1 = 纯三次曲线）。
 * 目的：提高中位附近的**分辨率**（可操作性），满杆速度不变。
 * 原来的写法是"死区内归零、死区外直接输出原值"——跨过死区瞬间从 0 跳到 0.10，
 * 低速微调时会一冲一冲的，高速时又因为摇杆太敏感不好走直线。 */
#define RC_EXPO     0.30f

/* 死区重映射 + 三次曲线：
 *   |v| ≤ 死区          → 0
 *   死区 < |v| ≤ 1     → (|v|-dz)/(1-dz) 先线性铺满 0~1，再过 expo 曲线
 * expo 曲线：out = (1-e)*n + e*n^3
 *   n=0.5 → 0.3875（中位更细腻）；n=1 → 1（满杆不掉速度） */
static float rcCurve(float v)
{
    float a = fabsf(v);
    if (a <= RC_DEADZONE) return 0.0f;
    float n = (a - RC_DEADZONE) / (1.0f - RC_DEADZONE);
    if (n > 1.0f) n = 1.0f;
    n = (1.0f - RC_EXPO) * n + RC_EXPO * n * n * n;
    return (v > 0.0f) ? n : -n;
}

/* 驾驶模式：右摇杆上下=前后，右摇杆左右=横移，左摇杆左右=自转。
 * 2026-11-16：抽成一个函数，正常驾驶和爬台阶共用，避免两处符号/系数改漏。
 * 注意符号：lr 正 = 向左，所以右推（right_rol 正）取负；
 *           ω 正 = 逆时针，所以左推（left_rol 正）取负。 */
static void DriveFromSticks(void)
{
    ChassisSetVelocity( rcCurve(RemoteControl::rcInfo.right_col) * CHASSIS_VXY_MAX,
                       -rcCurve(RemoteControl::rcInfo.right_rol) * CHASSIS_VXY_MAX,
                       -rcCurve(RemoteControl::rcInfo.left_rol)  * CHASSIS_RT_MAX);
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
            DriveFromSticks();
        } else {
            ChassisStop();          /* SB 中/下 = 机械臂模式，底盘必须停 */
        }
        UserStop();                 /* 推杆、M2006 全部停 */
        return;
    }

    /* 爬台阶：只有“保持/完成”步骤允许底盘动；伸/缩过程中底盘必须停。 */
    if (JackChassisAllowed()) {
        DriveFromSticks();
    } else {
        ChassisStop();
    }
}
