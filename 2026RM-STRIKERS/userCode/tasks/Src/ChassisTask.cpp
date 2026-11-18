//
// Created by LEGION on 2021/10/4.
//
#include "ChassisTask.h"

PID_Regulator_t pidRegulator = {//此为储存pid参数的结构体，四个底盘电机共用
        /* 2026-11-03 审计修复：和主线 01_firmware 的**实测最终值**对齐。
         *   kp 60 → 120        ：60 是第一次台架单电机时的值，落地跟踪只有 88~90%
         *   ki 保持 0           ：纯 P 有静差，但现阶段够用；要加就从 ki=0.05 / kiMax=1500 起
         *   outputMax 16384→12000：满量程 16384 = 20 A/电机，四轮 80 A，
         *                          **超过电池架 35 A 持续上限** → 会掉压/触发保护。
         *                          12000 ≈ 14.6 A/电机。不够时先修供电/助跑，别直接调大。 */
        .kp = 120,
        .ki = 0,
        .kd = 0,
        .componentKpMax = 16384,
        .componentKiMax = 0,
        .componentKdMax = 0,
        .outputMax = 12000
};

MOTOR_INIT_t chassisMotorInit = {//四个底盘电机共用的初始化结构体
        .speedPIDp = &pidRegulator,
        .anglePIDp = nullptr,
        ._motorID = MOTOR_ID_1,
        .reductionRatio = 19.2032f,   /* 官方 3591/187（原 19.0 差 1.07%）*/
        .ctrlType = SPEED_Single,
};
/* 2026-11-03 审计修复：绑定改成实测表（见 ChassisTask.h 里的 ID_WHEEL_*）*/
Motor CMFL(ID_WHEEL_FL,&chassisMotorInit);//左前轮
Motor CMFR(ID_WHEEL_FR,&chassisMotorInit);//右前轮
Motor CMBL(ID_WHEEL_BL,&chassisMotorInit);//左后轮
Motor CMBR(ID_WHEEL_BR,&chassisMotorInit);//右后轮

uint8_t ChassisStopFlag = 1;
float FBVelocity,LRVelocity,RTVelocity;

/* 四个轮子目标转速（输出轴 rpm），WheelsSpeedCalc 写、串口打印读 */
float wheelTargetRpm[4] = {0};

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
    /* 目标清零：否则串口里 t1~t4 会一直显示上一次的速度，分不清"没下发"和"下发了没转" */
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
     * @brief 此处四句代码需要结合底盘的三个速度，计算处四个轮子的位置对应的线速度。
     * @param fbVelocity,lrVelocity,rtVelocity
     * @return CMFLSpeed CMFRSpeed CMBLSpeed CMBRSpeed
     */
    // 麦轮逆运动学：由底盘三速度解出四个轮子的线速度（m/s）
    // 注意：上面 RPM2RADpS 已经把 rtVelocity 换成 rad/s，所以 k 必须用"米"，
    //       这样 k * rtVelocity 才是 m/s，和 fb / lr 同量纲。
    const float k = (WHEEL_BASE + TRACK_WIDTH) / 2.0f;   // 0.39 m（半轴距 + 半轮距）

    CMFLSpeed = fbVelocity - lrVelocity - k * rtVelocity;
    CMFRSpeed = fbVelocity + lrVelocity + k * rtVelocity;
    CMBLSpeed = fbVelocity + lrVelocity - k * rtVelocity;
    CMBRSpeed = fbVelocity - lrVelocity + k * rtVelocity;

    //计算四个轮子角速度，单位：rad/s
    CMFLSpeed = CMFLSpeed /(WHEEL_DIAMETER/2.0f);
    CMFRSpeed = CMFRSpeed /(WHEEL_DIAMETER/2.0f);
    CMBLSpeed = CMBLSpeed /(WHEEL_DIAMETER/2.0f);
    CMBRSpeed = CMBRSpeed /(WHEEL_DIAMETER/2.0f);

    /* 2026-11-03：先存成"输出轴 rpm"给遥测（t1~t4），再乘方向补偿下发 */
    wheelTargetRpm[0] = RADpS2RPM(CMFLSpeed);
    wheelTargetRpm[1] = RADpS2RPM(CMFRSpeed);
    wheelTargetRpm[2] = RADpS2RPM(CMBLSpeed);
    wheelTargetRpm[3] = RADpS2RPM(CMBRSpeed);

    //控制底盘电机转速（乘方向补偿：+1 = 车前进方向）
    CMFL.SetTargetSpeed(wheelTargetRpm[0] * SIGN_WHEEL_FL);
    CMFR.SetTargetSpeed(wheelTargetRpm[1] * SIGN_WHEEL_FR);
    CMBL.SetTargetSpeed(wheelTargetRpm[2] * SIGN_WHEEL_BL);
    CMBR.SetTargetSpeed(wheelTargetRpm[3] * SIGN_WHEEL_BR);
}
