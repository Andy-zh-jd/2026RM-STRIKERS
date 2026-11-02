//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"
#include "RemoteControl.h"

// SA = optional[0], high = climb mode. Raw optional channel is 0..2047.
static bool ClimbMode(void)
{
    return RemoteControl::rcInfo.optional[0] > 1000;
}

void CtrlHandle()
{
    if (RemoteControl::rcInfo.sRight == DOWN_POS){
        //右侧下三档，急停模式，请勿更改！！！
        ChassisStop();
        UserStop();
        return;
    }

    // 爬台阶时底盘必须停，机械臂在 ArmControl() 里也会被跳过。
    if (ClimbMode()) {
        ChassisStop();
        return;
    }

    //其他正常模式
    switch (RemoteControl::rcInfo.sLeft)
    {
    case UP_POS: //左侧一档
        ChassisSetVelocity(RemoteControl::rcInfo.right_col*4.2,
                           RemoteControl::rcInfo.right_rol*4.2,RemoteControl::rcInfo.left_rol*60);
        break;
    case MID_POS: //左侧二档
        break;
    case DOWN_POS: //左侧三档
        break;
    default:
        break;
    }
}
