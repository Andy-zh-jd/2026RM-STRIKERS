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

/* ===================== 物理位置 → 电调 ID 对应表 =====================
 * 2026-10-23：实测发现电调的 ID 和"物理位置"对不上，而且 C620 的 SET 键很小、
 * 改 ID 麻烦。所以**不改硬件，改这张表**：框架里的 CMFL/CMFR/CMBL/CMBR
 * 只是四个对象名，绑哪个 ID 由这里决定。改完重烧即生效，逆解不用动。
 *
 * 2026-10-23 最终确认（**以"手转轮子看哪个 aN 变"为准**，不要用 LED 标签推）：
 *   左前 = ID 1
 *   左前 = ID 1
 *   右前 = ID 4
 *   左后 = ID 3
 *   右后 = ID 2
 * （与电调 LED 的绿灯闪数一致：左前闪 1、右前闪 4、左后闪 3、右后闪 2）
 *
 * ⚠️⚠️ **ID 是位掩码，不是普通数字**（2026-10-23 踩过的坑）：
 *   框架里 GET_MOTOR_POS(ID) = log2(ID)，而 MOTOR_ID_1..8 = 0x01,0x02,0x04,0x08,...
 *   所以 "ID4" 必须写成 MOTOR_ID_4 = 0x08，写 4 会被当成 MOTOR_ID_3（log2(4)=2）；
 *   写 3 更糟：log2(3)=1.58→截断成 1，会和 ID2 抢同一个槽（表现为 a2 恒 0）。
 *   ==> 一律用 MOTOR_ID_x 常量，不要写字面数字。
 *
 * ⚠️ 两条硬规则：
 *   1) 四个 ID 必须**互不相同**，否则同一 ID 的两个电调会撞车（橙灯闪 2 次、输出关闭）
 *   2) 不要用 ID 5（0x1FF 帧被 M2006 占了），也不要用 6~8（框架没建对象）
 */
#define ID_WHEEL_FL   MOTOR_ID_1   /* 左前 = 电调 ID 1 */
#define ID_WHEEL_FR   MOTOR_ID_4   /* 右前 = 电调 ID 4 */
#define ID_WHEEL_BL   MOTOR_ID_3   /* 左后 = 电调 ID 3 */
#define ID_WHEEL_BR   MOTOR_ID_2   /* 右后 = 电调 ID 2 */
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
