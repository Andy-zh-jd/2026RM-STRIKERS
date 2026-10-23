//
// Created by LEGION on 2021/10/4.
//
#include "ChassisTask.h"

/* 2026-10-23：底盘电流指令上限。
 * 满量程 16384 = 20 A/电机，四轮就是 80 A —— 电池架持续上限只有 35 A。
 * 起步阶段先压到 12000（≈14.6 A/电机）；上台阶确实不够时再临时放到 16384，
 * 并记住：**功率问题优先修供电/操作（助跑），不是把电流调大。** */
#define CHASSIS_IQ_MAX   12000

PID_Regulator_t pidRegulator = {//此为储存pid参数的结构体，四个底盘电机共用
        .kp = 60,
        .ki = 0,
        .kd = 0,
        .componentKpMax = 16384,
        .componentKiMax = 0,
        .componentKdMax = 0,
        .outputMax = CHASSIS_IQ_MAX //3508电机输出电流上限，可以调小，勿调大
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

/* 四个轮子的目标转速（输出轴 rpm），WheelsSpeedCalc 写、打印读 */
float wheelTargetRpm[4] = {0};

/* 当前单轮调试选中的轮子下标（0=左前 1=右前 2=左后 3=右后），打印用 */
uint8_t g_selWheel = 0;

/* 2026-10-23：单轮调试模式标志。1 = 绕过逆解，直接按 wheelTargetRpm 驱动四轮 */
static uint8_t manualWheelMode = 0;

void ChassisStart(){

}
/**
 * @brief 底盘任务的处理函数，定时执行
 * @callergraph void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) in Device.cpp
 */
void ChassisHandle() {
    if (manualWheelMode) {
        /* 单轮调试：四个轮子的目标已经在 ChassisSetWheelSpeed 里摆好，直接下发 */
        CMFL.SetTargetSpeed(wheelTargetRpm[0]);
        CMFR.SetTargetSpeed(wheelTargetRpm[1]);
        CMBL.SetTargetSpeed(wheelTargetRpm[2]);
        CMBR.SetTargetSpeed(wheelTargetRpm[3]);
    } else if(ChassisStopFlag == 0) {
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
    manualWheelMode = 0;          // 退出单轮调试
    ChassisStopFlag = 0;
    FBVelocity = _fbV;
    LRVelocity = _lrV;
    RTVelocity = _rtV;
}

/* 2026-10-23：单轮调试（M3 方向校验用）。
 * 用法：SB 拨到下档（sL=158），右摇杆推 → 只有 idx 指定的那个轮子转。
 * 逐轮测：改 ControlTask.cpp 里传进来的 idx（0/1/2/3），确认每个轮子的
 * "正目标 = 车往前时的那个轮子转向"。
 * 注意：本函数会清另外三个轮子的目标，所以是"绝对安全"的单轮模式。 */
void ChassisSetWheelSpeed(uint8_t idx, float rpm){
    manualWheelMode = 1;
    ChassisStopFlag = 1;          // 逆解不再覆盖
    for (uint8_t i = 0; i < 4; ++i) wheelTargetRpm[i] = 0;
    if (idx < 4) { wheelTargetRpm[idx] = rpm; g_selWheel = idx; }
}

/**
 * @brief 执行急停模式的底盘任务处理
 */
void ChassisStop(){
    manualWheelMode = 0;
    ChassisStopFlag = 1;
    for (uint8_t i = 0; i < 4; ++i) wheelTargetRpm[i] = 0;
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

    //计算四个轮子角速度，单位：rad/s；再换算成输出轴 RPM（打印要用）
    wheelTargetRpm[0] = RADpS2RPM(CMFLSpeed / (WHEEL_DIAMETER/2.0f));
    wheelTargetRpm[1] = RADpS2RPM(CMFRSpeed / (WHEEL_DIAMETER/2.0f));
    wheelTargetRpm[2] = RADpS2RPM(CMBLSpeed / (WHEEL_DIAMETER/2.0f));
    wheelTargetRpm[3] = RADpS2RPM(CMBRSpeed / (WHEEL_DIAMETER/2.0f));
    //控制底盘电机转速
    CMFL.SetTargetSpeed(wheelTargetRpm[0]);
    CMFR.SetTargetSpeed(wheelTargetRpm[1]);
    CMBL.SetTargetSpeed(wheelTargetRpm[2]);
    CMBR.SetTargetSpeed(wheelTargetRpm[3]);
}
