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
#define REDUCTION_RATIO         19.2032f   /* 2026-10-30：官方 3591/187，原写 19.0 差 1.07%（5 m 差 5 cm）*/
#define TRACK_WIDTH            0.40f //轮距
#define WHEEL_BASE             0.38f //轴距

/* ===================== 物理位置 → 电调 ID（2026-10-30 实测最终版）=====================
 * ⚠ 2026-11-03 审计修复：原来这里是 MOTOR_ID_1/2/3/4 顺序直连，和实车对不上。
 *   实测（以"手转轮子看哪个 aN 变"为准，不是看电调 LED 标签）：
 *     左前 = ID 3 / 右前 = ID 4 / 左后 = ID 1 / 右后 = ID 2
 *   用错的绑定 → 右前右后拿到的其实是左后左前的指令，车会原地打转而不是前进。
 *
 * ⚠⚠ ID 是**位掩码**，不是普通数字：GET_MOTOR_POS(ID)=log2(ID)，MOTOR_ID_1..8 = 0x01,0x02,0x04,0x08
 *   写字面数字 4 会抢到 MOTOR_ID_3 的槽（曾误判"右前电调坏了"，浪费约 1 小时）。
 *   ⚠ 必须用 MOTOR_ID_x 常量；四个 ID 必须互不相同。
 *
 * 如果换过电调 / 别人动过车，**不要相信这张表**，按 03_docs/M3-逐轮验收表.md
 * 单轮法重测，然后只改这四行。 */
#define ID_WHEEL_FL   MOTOR_ID_3   /* 左前 = 电调 ID 3 */
#define ID_WHEEL_FR   MOTOR_ID_4   /* 右前 = 电调 ID 4 */
#define ID_WHEEL_BL   MOTOR_ID_1   /* 左后 = 电调 ID 1 */
#define ID_WHEEL_BR   MOTOR_ID_2   /* 右后 = 电调 ID 2 */

/* ===================== 电机方向补偿（2026-10-30 实测）=====================
 * 逐轮实测：正目标时 左前→朝前 ✅ 左后→朝前 ✅ 右前→朝后 ❌ 右后→朝后 ❌
 * 原因：底盘左右两侧电机镜像安装，同一个"前进"要求两侧转子转向相反。
 * 统一成「+1 = 车前进方向」，在**下发目标**和**显示回读**两处都乘，
 * 于是串口里 t / r 永远在同一个（车体）坐标系里，便于判断"同号"。
 * 换电机/改安装方式后只改这四个数。 */
#define SIGN_WHEEL_FL   (+1.0f)
#define SIGN_WHEEL_FR   (-1.0f)
#define SIGN_WHEEL_BL   (+1.0f)
#define SIGN_WHEEL_BR   (-1.0f)
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

/* 2026-11-03：四个轮子的**目标**转速（输出轴 rpm）+ 安全态标志，供串口打印用。
 * 下标：0=左前 1=右前 2=左后 3=右后。原来是"零遥测"状态，出故障只能猜。 */
extern float wheelTargetRpm[4];
extern uint8_t ChassisStopFlag;

/*外部函数声明-------------------------------------------------------------*/

void WheelsSpeedCalc(float fbVelocity, float lrVelocity, float rtVelocity);
void ChassisStop();
void ChassisSetVelocity(float _fbV,float _lrV,float _rtV);
#endif //RM_FRAME_C_CHASSISTASK_H
