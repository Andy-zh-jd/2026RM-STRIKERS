//
// Created by LEGION on 2021/10/4.
//

#ifndef RM_FRAME_C_USERTASK_H
#define RM_FRAME_C_USERTASK_H
#include "main.h"
#include "Device.h"
#include "Servo.h"
#include "Motor.h"
/*枚举类型定义------------------------------------------------------------*/


/*结构体定义--------------------------------------------------------------*/


/*结构体成员取值定义组------------------------------------------------------*/
/*外部变量声明-------------------------------------------------------------*/

extern Servo ClawServo,TurnLServo,TurnRServo,ShovelLServo,ShovelRServo;

/*外部函数声明-------------------------------------------------------------*/

void UserStop();
void UserHandle();
void UserInit();
/* 给机构电机（UserMotor，M2006，ID5）设目标转速，单位：输出轴 rpm，正负 = 方向。
 * 2026-10-21 加：让遥控器能直接控制 M2006（用于台架验证“电机按指令转”）。 */
void UserSetSpeed(float _rpm);
/* BTS7960 四推杆：SA=总开关/爬台阶，SD=步进，所有输出 24V 全压。 */
bool JackActive(void);
bool JackChassisAllowed(void);
/* ---- PCA9685 十六路舵机驱动（软件 I2C，B15=SCL / B13=SDA）----
 * 由 2026RM-STRIKERS（Keil 工程）并入，详见 UserTask.cpp 里的 PCA9685 段。 */
uint8_t PCA_Init(void);
void    PCA_SetPulse(uint8_t ch, uint16_t us);
void    PCA_SetAngle(uint8_t ch, float angle);
uint16_t PCA_ReadOff(uint8_t ch);
void    PCA_Diag(void);      /* 总线自检：结果打进 C 页 bS/bD/pS/pD/scan */
void    PIN_Scan(void);      /* 候选引脚扫描：结果打进 C 页 pins= */
/* 慢任务：在主循环 while(1) 里调用（放 IMU 的 SPI 读，避免拖死中断） */
void UserPollSlow(void);
#endif //RM_FRAME_C_USERTASK_H
