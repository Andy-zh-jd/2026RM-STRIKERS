//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"
void CtrlHandle(){
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
        case MID_POS: //左侧二档

            break;
        case DOWN_POS: //左侧三档

            break;
        default:
            break;
        }
    }
}

