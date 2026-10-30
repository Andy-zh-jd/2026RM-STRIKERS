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
        /* 2026-10-30 晚：整车装好、四轮验收完成，开始调参。
         *
         * 基线实测（落地直线，kp=120/ki=0）：目标 44 rpm → 实测 40 rpm，跟踪率 88~90%，
         * 四轮间差 2~6%，稳态偏航 gz ≈ ±1.5°/s（跑 3.6 s 只偏 5°）→ 机械和电气都健康，
         * 所以现在的问题就是**纯 P 的静差**。
         *
         * 本轮改动：kp 120→180（起步更快、静差减半），
         *           ki 0→0.10 + componentKiMax 0→1500（消掉剩余静差）。
         *
         * ⚠️ 三条调参常识（踩过的坑）：
         *   · 框架 errSum **不乘 dt**，1 kHz 下 ki 的量级是 0.0x~0.1（不是仿真里的 10）
         *   · ki 必须和 componentKiMax 一起改：钳位是 ±componentKiMax/ki，只改 ki 会积分饱和
         *   · ki=0.5/kiMax=6000 在空载轮上会振荡（积分权限≈7.3 A 太大），别一步加太猛
         *   · 调参要**在高转速（60~100% 额定）上先调**，3% 额定转速区间摩擦主导，怎么调都抖 */
        .kp = 180,
        .ki = 0.10f,
        .kd = 0,
        .componentKpMax = 16384,
        .componentKiMax = 1500,
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
Motor CMFL(ID_WHEEL_FL,&chassisMotorInit);//定义左前轮电机（电调 ID 见表：ChassisTask.h）
Motor CMFR(ID_WHEEL_FR,&chassisMotorInit);//定义右前轮电机
Motor CMBL(ID_WHEEL_BL,&chassisMotorInit);//定义左后轮电机
Motor CMBR(ID_WHEEL_BR,&chassisMotorInit);//定义右后轮电机

uint8_t ChassisStopFlag = 1;
float FBVelocity,LRVelocity,RTVelocity;

/* 2026-10-23：单轮调试/直接指定目标模式标志。1 = 绕过逆解，直接按 wheelTargetRpm 驱动四轮 */
static uint8_t manualWheelMode = 0;

/* 四个轮子的目标转速（输出轴 rpm），WheelsSpeedCalc 写、打印读 */
float wheelTargetRpm[4] = {0};

/* 2026-10-23：直接指定四个轮子的目标（绕过逆解），用于"对角轮对照实验"。
 * 用法：a=左前 b=右前 c=左后 d=右后 的**车体坐标**目标 rpm（内部乘 SIGN）。
 * 之后 ChassisHandle 会按 manualWheelMode 一直驱动它们，直到 ChassisSetVelocity/ChassisStop。 */
void ChassisSetWheelPattern4(float a, float b, float c, float d){
    manualWheelMode = 1;
    ChassisStopFlag = 0;
    wheelTargetRpm[0] = a;
    wheelTargetRpm[1] = b;
    wheelTargetRpm[2] = c;
    wheelTargetRpm[3] = d;
}

/* 当前单轮调试选中的轮子下标（0=左前 1=右前 2=左后 3=右后），打印用 */
uint8_t g_selWheel = 0;

/* ===== 2026-10-30：车载 RAM 日志（下地测试专用）=====
 * 为什么需要：下地跑车时不能连着电脑（USB 线会把车拽住），所以把数据先存进 RAM，
 * 跑完再插 ST-Link 用 tools\read_log.py 读出来。
 * 每 20 ms 存一帧：t1~t4（目标 rpm×10）、r1~r4（实测 rpm×10）、gz（陀螺 ×10），
 * 共 512 帧 ≈ 10.2 秒，够跑一次直线或一次台阶。
 * 读法：pyocd 用 attach 模式（不能 reset，否则 RAM 清空），见 tools\read_log.py。 */
#define WHEEL_LOG_N    1024
int16_t  g_wheelLog[WHEEL_LOG_N][9];
volatile uint16_t g_wheelLogIdx   = 0;    /* 下一个要写的行（环形） */
volatile uint32_t g_wheelLogTotal = 0;    /* 累计写了多少帧（用来排序） */

extern float g_gyroLpf[3];   /* 定义在 UserTask.cpp */

static void WheelLogSample(void)
{
    /* **只在车动的时候记录**：否则静止 10 秒就把缓冲占满了，真正跑车的数据被覆盖。
     * 判据：任一目标 >0.5 rpm 或任一实测 >2 rpm 就认为在动。 */
    float at = 0.0f, ar = 0.0f;
    for (int i = 0; i < 4; i++) {
        float t = wheelTargetRpm[i];
        float r = (i == 0) ? CMFL.state.speed : (i == 1) ? CMFR.state.speed
                : (i == 2) ? CMBL.state.speed : CMBR.state.speed;
        at += (t < 0.0f ? -t : t);
        ar += (r < 0.0f ? -r : r);
    }
    if (at < 0.5f && ar < 2.0f) return;

    int16_t *p = g_wheelLog[g_wheelLogIdx];
    p[0] = (int16_t)(wheelTargetRpm[0] * 10.0f);
    p[1] = (int16_t)(wheelTargetRpm[1] * 10.0f);
    p[2] = (int16_t)(wheelTargetRpm[2] * 10.0f);
    p[3] = (int16_t)(wheelTargetRpm[3] * 10.0f);
    p[4] = (int16_t)(CMFL.state.speed * 10.0f);
    p[5] = (int16_t)(CMFR.state.speed * 10.0f);
    p[6] = (int16_t)(CMBL.state.speed * 10.0f);
    p[7] = (int16_t)(CMBR.state.speed * 10.0f);
    p[8] = (int16_t)(g_gyroLpf[2] * 10.0f);
    g_wheelLogIdx = (uint16_t)((g_wheelLogIdx + 1u) % WHEEL_LOG_N);
    g_wheelLogTotal++;
}

void ChassisStart(){

}
/**
 * @brief 底盘任务的处理函数，定时执行
 * @callergraph void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) in Device.cpp
 */
void ChassisHandle() {
    if (manualWheelMode) {
        /* 单轮调试：四个轮子的目标已经在 ChassisSetWheelSpeed 里摆好，直接下发 */
        CMFL.SetTargetSpeed(wheelTargetRpm[0] * SIGN_WHEEL_FL);
        CMFR.SetTargetSpeed(wheelTargetRpm[1] * SIGN_WHEEL_FR);
        CMBL.SetTargetSpeed(wheelTargetRpm[2] * SIGN_WHEEL_BL);
        CMBR.SetTargetSpeed(wheelTargetRpm[3] * SIGN_WHEEL_BR);
    } else if(ChassisStopFlag == 0) {
        WheelsSpeedCalc(FBVelocity, LRVelocity, RTVelocity);
    }
    CMFL.Handle();
    CMFR.Handle();
    CMBL.Handle();
    CMBR.Handle();

    /* 车载日志：每 20 ms 记一帧（1 kHz 中断里分频） */
    static uint8_t logDiv = 0;
    if (++logDiv >= 20u) {
        logDiv = 0;
        WheelLogSample();
    }
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
    //控制底盘电机转速（乘方向补偿：+1 = 车前进方向）
    CMFL.SetTargetSpeed(wheelTargetRpm[0] * SIGN_WHEEL_FL);
    CMFR.SetTargetSpeed(wheelTargetRpm[1] * SIGN_WHEEL_FR);
    CMBL.SetTargetSpeed(wheelTargetRpm[2] * SIGN_WHEEL_BL);
    CMBR.SetTargetSpeed(wheelTargetRpm[3] * SIGN_WHEEL_BR);
}
