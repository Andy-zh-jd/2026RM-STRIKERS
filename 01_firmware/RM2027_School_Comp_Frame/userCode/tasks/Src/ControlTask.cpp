//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"
void CtrlHandle(){
    /* ===== 2026-10-21 安全修复 =====
     * 没有接收机时 rcInfo 是零初始化 → sLeft 默认 0 = MID_POS → 会误判"用户选了中档"
     * → UserSetSpeed(0) 把 stopFlag 解开（实测 stop 从 1 变成 0）。
     * 所以先判断遥控是否在线：100ms 内没收到帧 → 强制进安全态，直接返回。 */
    if (!RemoteAlive()) {
        ChassisStop();
        UserStop();
        return;
    }

    if (RemoteControl::rcInfo.sRight == DOWN_POS){
        //右侧下三档，急停模式，请勿更改！！！
        ChassisStop();
        UserStop();
    } else {
        //其他正常模式
        switch (RemoteControl::rcInfo.sLeft)
        {
        case UP_POS: //左侧一档
            // 2026-10-20：满杆系数由 4.2 改成 3.0。
            // 原因：M3508 输出轴空载只有 482 rpm ≈ 3.85 m/s，4.2 会让 PID 长期顶在 16384 饱和
            //（表现是"推满杆反而发死"）。3.0 m/s 留了余量，底盘稳了再往上加。
            // 第三个参数 60 = 自转满杆 60 rpm（≈306.9 rpm 输出轴），不用改。
            ChassisSetVelocity(RemoteControl::rcInfo.right_col*3.0,
                               RemoteControl::rcInfo.right_rol*3.0,RemoteControl::rcInfo.left_rol*60);
            break;
        case MID_POS: //左侧二档：用右摇杆控制 M2006（机构电机）
            // 2026-10-21：台架验证用。满杆 = ±30 rpm（输出轴，36:1 减速后很慢，安全）。
            // 中位 ±8% 死区，防止摇杆零点漂移导致电机自己爬。
            {
                float c = RemoteControl::rcInfo.right_col;
                if (c > -0.08f && c < 0.08f) c = 0.0f;
                UserSetSpeed(c * 30.0f);
            }
            break;
        case DOWN_POS: //左侧三档

            break;
        default:
            break;
        }
    }
}

