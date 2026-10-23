//
// Created by LEGION on 2021/10/4.
//
#include "ChassisTask.h"

PID_Regulator_t pidRegulator = {//此为储存pid参数的结构体，四个底盘电机共用
        .kp = 60,
        .ki = 0,
        .kd = 0,
        .componentKpMax = 16384,
        .componentKiMax = 0,
        .componentKdMax = 0,
        .outputMax = 16384 //3508电机输出电流上限，可以调小，勿调大
};

MOTOR_INIT_t chassisMotorInit = {//四个底盘电机共用的初始化结构体
        .speedPIDp = &pidRegulator,
        .anglePIDp = nullptr,
        ._motorID = MOTOR_ID_1,
        // 2026-10-20：官方规格 3591/187 = 19.2032，框架原来写 19.0 会差 1.07%
        //（表现为"车走得挺直，但 5 m 总是差 5 cm 左右"）
        .reductionRatio = 19.2032f,
        .ctrlType = SPEED_Single,
};
Motor CMFL(MOTOR_ID_1,&chassisMotorInit);//定义左前轮电机
Motor CMFR(MOTOR_ID_2,&chassisMotorInit);//定义右前轮电机
Motor CMBL(MOTOR_ID_3,&chassisMotorInit);//定义左后轮电机
Motor CMBR(MOTOR_ID_4,&chassisMotorInit);//定义右后轮电机

uint8_t ChassisStopFlag = 1;
float FBVelocity,LRVelocity,RTVelocity;

void ChassisStart(){

}
/**
 * @brief 底盘任务的处理函数，定时执行
 * @callergraph void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) in Device.cpp
 */
void ChassisHandle() {
    if(ChassisStopFlag == 0) {
        WheelsSpeedCalc(FBVelocity, LRVelocity, RTVelocity);
    }
    CMFL.Handle();
    CMFR.Handle();
    CMBL.Handle();
    CMBR.Handle();
}
/**
 * @brief 用于控制任务控制底盘速度
 * @param _fbV 底盘前后方向速度
 * @param _lrV 底盘左右方向速度
 * @param _rtV 底盘旋转速度
 */
void ChassisSetVelocity(float _fbV,float _lrV,float _rtV){
    ChassisStopFlag = 0;
    FBVelocity = _fbV;
    LRVelocity = _lrV;
    RTVelocity = _rtV;
}
/**
 * @brief 执行急停模式的底盘任务处理
 */
void ChassisStop(){
    ChassisStopFlag = 1;
    CMFL.Stop();
    CMFR.Stop();
    CMBL.Stop();
    CMBR.Stop();
}

void WheelsSpeedCalc(float fbVelocity, float lrVelocity, float rtVelocity) {
    float CMFLSpeed, CMFRSpeed, CMBLSpeed, CMBRSpeed;

    rtVelocity = RPM2RADpS(rtVelocity);

    //计算四个轮子线速度，单位：m/s
    /**
     * @brief 麦轮逆运动学：把底盘三速度 (前后/左右/自转) 解算成四个轮子的线速度。
     * @param fbVelocity 前后 m/s，向前为正
     * @param lrVelocity 左右 m/s，向左为正
     * @param rtVelocity 自转 rad/s（函数第一行已把 RPM 转过来），俯视逆时针为正
     *
     * 约定与自检（单位 m/s，k = (轮距+轴距)/2 = 0.39）：
     *   fb=1, lr=0, ω=0  →  四个都是 +1.00      （直行）
     *   fb=0, lr=1, ω=0  →  -1, +1, +1, -1      （向左平移）
     *   fb=0, lr=0, ω=1  →  -0.39, +0.39, -0.39, +0.39（原地逆时针转）
     *
     * ⚠️ 上车后如果方向不对，**只改符号**，不要改公式结构：
     *   ① 直行反了    → 四个式子整体取反（电机方向反）
     *   ② 横移方向反  → 只翻 lrVelocity 那四项
     *   ③ 自转方向反  → 只翻 k*rtVelocity 那四项
     *   ④ 单个轮子反  → 只翻那一个轮（常见于 CAN ID 接错位）
     */
    const float k = (TRACK_WIDTH + WHEEL_BASE) * 0.5f;   // 0.39 m，旋转时轮子的等效力臂

    CMFLSpeed = fbVelocity - lrVelocity - k * rtVelocity;  // 左前
    CMFRSpeed = fbVelocity + lrVelocity + k * rtVelocity;  // 右前
    CMBLSpeed = fbVelocity + lrVelocity - k * rtVelocity;  // 左后
    CMBRSpeed = fbVelocity - lrVelocity + k * rtVelocity;  // 右后

    //计算四个轮子角速度，单位：rad/s
    CMFLSpeed = CMFLSpeed /(WHEEL_DIAMETER/2.0f);
    CMFRSpeed = CMFRSpeed /(WHEEL_DIAMETER/2.0f);
    CMBLSpeed = CMBLSpeed /(WHEEL_DIAMETER/2.0f);
    CMBRSpeed = CMBRSpeed /(WHEEL_DIAMETER/2.0f);
    //控制底盘电机转速
    CMFL.SetTargetSpeed(RADpS2RPM(CMFLSpeed));
    CMFR.SetTargetSpeed(RADpS2RPM(CMFRSpeed));
    CMBL.SetTargetSpeed(RADpS2RPM(CMBLSpeed));
    CMBR.SetTargetSpeed(RADpS2RPM(CMBRSpeed));
}
