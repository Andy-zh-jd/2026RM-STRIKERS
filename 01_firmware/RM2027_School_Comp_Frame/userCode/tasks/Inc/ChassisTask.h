//
// Created by LEGION on 2021/10/4.
//

#ifndef RM_FRAME_C_CHASSISTASK_H
#define RM_FRAME_C_CHASSISTASK_H
#include "main.h"
#include "Motor.h"

#define RPM2RADpS(num)(num * (3.1415926f/30.0f))
#define RADpS2RPM(num)(num / (3.1415926f/30.0f))
#define WHEEL_DIAMETER             0.1525f//麦轮直径 m
#define REDUCTION_RATIO 19.0f
#define TRACK_WIDTH            0.40f //轮距
#define WHEEL_BASE             0.38f //轴距
/*枚举类型定义------------------------------------------------------------*/

/*
 * 底盘任务错误信息枚举
 */
typedef enum{
    NONE
}CHASSIS_ERROR_E;

/*结构体定义--------------------------------------------------------------*/

/*
 * 底盘任务状态结构体
 */
typedef struct{

}CHASSIS_STATE_T;

/*
 * 底盘任务控制结构体
 */
typedef struct{


}CHASSIS_CONTROL_T;



/*结构体成员取值定义组------------------------------------------------------*/
/*外部变量声明-------------------------------------------------------------*/

extern CHASSIS_STATE_T     chassisState;
extern CHASSIS_CONTROL_T   chassisControl;

extern float FBVelocity,LRVelocity,RTVelocity;

/* 2026-10-23：四个轮子的**目标**转速（输出轴 rpm），供串口打印/调试用。
 * 下标：0=左前(CMFL,ID1) 1=右前(CMFR,ID2) 2=左后(CMBL,ID3) 3=右后(CMBR,ID4) */
extern float wheelTargetRpm[4];

/* 单轮调试时当前选中的轮子下标（0=左前 1=右前 2=左后 3=右后），打印用 */
extern uint8_t g_selWheel;

/*外部函数声明-------------------------------------------------------------*/

void WheelsSpeedCalc(float fbVelocity, float lrVelocity, float rtVelocity);
void ChassisStop();
void ChassisSetVelocity(float _fbV,float _lrV,float _rtV);

/* 2026-10-23：单轮调试入口（M3 方向校验用）
 * 只给指定的一个轮子下目标转速，其余三轮目标清零；绕过麦轮逆解。
 * idx: 0=左前 1=右前 2=左后 3=右后；rpm 为输出轴转速（正负=方向）
 * 退出方式：调 ChassisStop() 或 ChassisSetVelocity() */
void ChassisSetWheelSpeed(uint8_t idx, float rpm);
#endif //RM_FRAME_C_CHASSISTASK_H
