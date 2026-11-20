//
// Created by LEGION on 2021/10/4.
//
#include "ChassisTask.h"
#include "UserTask.h"          /* 2026-11-16：IMU_Ready() / g_gyroLpf[] 给航向锁用 */
#include <math.h>

/* 2026-10-23：底盘电流指令上限。
 * 满量程 16384 = 20 A/电机，四轮就是 80 A —— 电池架持续上限只有 35 A。
 * 起步阶段先压到 12000（≈14.6 A/电机）；上台阶确实不够时再临时放到 16384，
 * 并记住：**功率问题优先修供电/操作（助跑），不是把电流调大。** */
#define CHASSIS_IQ_MAX   12000

PID_Regulator_t pidRegulator = {//此为储存pid参数的结构体，四个底盘电机共用
        /* 2026-10-23：**调参阶段推迟到整车装配完成后**（见 03_docs/M3-逐轮验收表.md）。
         * 现在只做"逐轮有效性验证"，只需要"能转 + 方向对"，不需要精度，
         * 所以退回纯 P（ki=0 不起积分，绝不会因积分饱和而振荡，测试结果干净）。
         *
         * 已踩过的两个坑（调参时别忘）：
         *   · 纯 P 必然有静差：t3=29 时 r3 只到 8~17
         *   · ki=0.5 / kiMax=6000 在空载轮上会振荡（积分权限 ≈7.3A 太大）
         *     → 调参时从 ki=0.05 / kiMax=1500 起步，且**在高转速（60~100% 额定）上先调**
         *   · 框架 errSum 不乘 dt，1kHz 下 ki 量级是 0.0x~0.1（不是仿真里的 10）*/
        .kp = 120,
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
Motor CMFL(ID_WHEEL_FL,&chassisMotorInit);//定义左前轮电机（电调 ID 见表：ChassisTask.h）
Motor CMFR(ID_WHEEL_FR,&chassisMotorInit);//定义右前轮电机
Motor CMBL(ID_WHEEL_BL,&chassisMotorInit);//定义左后轮电机
Motor CMBR(ID_WHEEL_BR,&chassisMotorInit);//定义右后轮电机

uint8_t ChassisStopFlag = 1;
float FBVelocity,LRVelocity,RTVelocity;

/* 四个轮子的目标转速（输出轴 rpm，车体系：正=车前进方向），WheelsSpeedCalc 写、打印读 */
float wheelTargetRpm[4] = {0};

/* 当前单轮调试选中的轮子下标（0=左前 1=右前 2=左后 3=右后），打印用 */
uint8_t g_selWheel = 0;

/* 2026-10-23：单轮调试模式标志。1 = 绕过逆解，直接按 wheelTargetRpm 驱动四轮 */
static uint8_t manualWheelMode = 0;

/* ===================== 2026-11-16：运动控制内部状态 =====================
 * 全部只在 1 kHz 的 ChassisHandle() 里读写，不进中断/主循环，不需要临界区。 */
static float slewVx = 0.0f;        /* 斜坡后的车体前后速度指令 m/s */
static float slewVy = 0.0f;        /* 斜坡后的车体左右速度指令 m/s（+左） */
static float slewW  = 0.0f;        /* 斜坡后的自转指令 rad/s（+逆时针） */
static float yawDeg = 0.0f;        /* 陀螺积分航向（°，连续，Wrap 到 ±180） */
static float yawTargetDeg = 0.0f;  /* 直行锁目标航向（°） */
static uint8_t yawHold = 0;        /* 1 = 正在航向锁（直行意图） */
static float gzYawDps = 0.0f;      /* 当前 yaw 轴角速度 °/s（已按 CHASSIS_IMU_YAW_SIGN 换算） */

/* 对外快照（打印 / SWD / RAM 日志） */
volatile float chassisYawDeg = 0.0f;
volatile float chassisYawTargetDeg = 0.0f;
volatile float chassisYawCorr = 0.0f;
volatile float chassisDesatScale = 1.0f;
volatile float chassisOmegaEff = 0.0f;
volatile float chassisDbgVx = 0.0f;
volatile float chassisDbgVy = 0.0f;
volatile uint8_t chassisImuOk = 0;

/* ===================== 2026-11-16：车载 RAM 日志 =====================
 * 定义见 ChassisTask.h 的列说明。读法：tools/read_log.py（ST-Link attach，不清 RAM）。 */
int16_t  g_wheelLog[WHEEL_LOG_N][WHEEL_LOG_COLS];
volatile uint16_t g_wheelLogIdx   = 0;    /* 下一个要写的行（环形） */
volatile uint32_t g_wheelLogTotal = 0;    /* 累计写了多少帧 */

/* ===================== 内部小工具 =====================
 * 注意：本工程用软浮点（CMake 没开 -mfloat-abi=hard），这些函数每 1 ms 只算几次，
 * 对 1 ms 周期没有压力；但不要在 1 ms 中断里加循环/查表这类大开销。 */
static float SlewToward(float cur, float target, float maxStep)
{
    float d = target - cur;
    if (d >  maxStep) d =  maxStep;
    if (d < -maxStep) d = -maxStep;
    return cur + d;
}

/* 把角度差折到 ±180°（航向锁的误差计算必须用这个，否则过 ±180 会疯转） */
static float WrapDeg180(float deg)
{
    while (deg >  180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

/* 航向锁误差（°，±180）—— 给打印/SWD/日志用 */
float ChassisYawErrDeg(void)
{
    return WrapDeg180(chassisYawTargetDeg - chassisYawDeg);
}

/* 四个轮子实测转速，换算到**车体系**（正 = 车前进方向）。
 * 2026-11-16：原来只在打印时乘 SIGN，现在航向/日志都要用，统一成函数避免漏乘。 */
static float WheelRpmBody(uint8_t idx)
{
    switch (idx) {
        case 0: return CMFL.state.speed * SIGN_WHEEL_FL;
        case 1: return CMFR.state.speed * SIGN_WHEEL_FR;
        case 2: return CMBL.state.speed * SIGN_WHEEL_BL;
        default:return CMBR.state.speed * SIGN_WHEEL_BR;
    }
}

/* ===================== 2026-11-17：静止判定（给 IMU 零偏自动学习用） =====================
 * 判据：四个轮子**目标≈0** 且**实测转速≈0**，并且连续 1.5 s。1 kHz 调用。
 * 为什么要"目标≈0"：如果车被顶住/卡住，驾驶员还压着杆，轮速可能为 0 但车其实在使劲，
 * 这时陀螺可能有振动，不该学零偏。 */
static uint16_t stationCnt = 0;        /* 连续静止的毫秒数（饱和到 60000） */

static void StationaryUpdate(void)
{
    uint8_t still = 1;
    for (uint8_t i = 0; i < 4; i++) {
        if (fabsf(wheelTargetRpm[i]) > 0.5f) { still = 0; break; }
        if (fabsf(WheelRpmBody(i)) > 3.0f)   { still = 0; break; }
    }
    if (!still) { stationCnt = 0; return; }
    if (stationCnt < 60000u) stationCnt++;
}

uint8_t ChassisIsStationary(void)
{
    return (uint8_t)(stationCnt >= 1500u);   /* 1 ms 周期 → 1.5 s */
}


/* 每 20 ms 采一帧（分频在 ChassisHandle 里做）。
 * 只在"车在动"时记录，否则静止 20 秒就把 1024 帧缓冲占满了。 */
static void WheelLogSample(void)
{
    float at = 0.0f, ar = 0.0f;
    for (uint8_t i = 0; i < 4; i++) {
        float t = wheelTargetRpm[i];
        float r = WheelRpmBody(i);
        at += (t < 0.0f ? -t : t);
        ar += (r < 0.0f ? -r : r);
    }
    if (at < 0.5f && ar < 2.0f) return;    /* 没在动：不记 */

    int16_t *p = g_wheelLog[g_wheelLogIdx];
    p[0] = (int16_t)(wheelTargetRpm[0] * 10.0f);
    p[1] = (int16_t)(wheelTargetRpm[1] * 10.0f);
    p[2] = (int16_t)(wheelTargetRpm[2] * 10.0f);
    p[3] = (int16_t)(wheelTargetRpm[3] * 10.0f);
    p[4] = (int16_t)(WheelRpmBody(0) * 10.0f);
    p[5] = (int16_t)(WheelRpmBody(1) * 10.0f);
    p[6] = (int16_t)(WheelRpmBody(2) * 10.0f);
    p[7] = (int16_t)(WheelRpmBody(3) * 10.0f);
    /* 陀螺是**未乘 SIGN 的原始轴**：README/文档里标定轴和正方向时看的就是它 */
    p[8] = (int16_t)(g_gyroLpf[CHASSIS_IMU_YAW_AXIS] * 10.0f);
    p[9] = (int16_t)(chassisDesatScale * 1000.0f);
    p[10] = (int16_t)(chassisOmegaEff * 1000.0f);
    p[11] = (int16_t)(WrapDeg180(chassisYawTargetDeg - chassisYawDeg) * 10.0f);

    g_wheelLogIdx = (uint16_t)((g_wheelLogIdx + 1u) % WHEEL_LOG_N);
    g_wheelLogTotal++;
}

void ChassisStart(){

}

/**
 * @brief 航向积分 + IMU 健康快照（每 1 ms 都更新，**与是否在驾驶无关**）
 *
 * 为什么不放在 WheelsSpeedCalc() 里：停车/机械臂模式（ChassisStop）时逆解不跑，
 * 但台架标定轴/符号、看 A 页 yaw、RAM 日志都需要"连续的航向"。
 * 放在 ChassisHandle 最前面，保证 1 kHz 稳定积分。
 * IMU 不健康时 gzYawDps=0：底盘自动退回"无航向锁"的旧行为。
 */
static void YawUpdate(void)
{
    uint8_t imuOk = IMU_Ready();
    chassisImuOk = imuOk;
    if (imuOk) {
        gzYawDps = g_gyroLpf[CHASSIS_IMU_YAW_AXIS] * CHASSIS_IMU_YAW_SIGN;
        /* 1 kHz 用最新陀螺值累加。短时间（秒级）足够准；长时间会因零偏慢慢漂，
         * 所以直行锁的目标每次进入直行都会重置，不依赖长期积分。 */
        yawDeg = WrapDeg180(yawDeg + gzYawDps * 0.001f);
    } else {
        gzYawDps = 0.0f;
    }
    chassisYawDeg = yawDeg;
}

/**
 * @brief 底盘任务的处理函数，定时执行（1 kHz，从 TIM10 的 1 ms 中断调）
 * @callergraph void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) in Device.cpp
 */
void ChassisHandle() {
    YawUpdate();                    /* 先更新航向（台架/日志/航向锁共用） */

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

    /* 2026-11-17：静止判定（放在电机 Handle 之后，用本周期最新的实测转速） */
    StationaryUpdate();

    /* 车载日志：1 kHz 分频到 50 Hz */
    static uint8_t logDiv = 0;
    if (++logDiv >= 20u) {
        logDiv = 0;
        WheelLogSample();
    }
}

/**
 * @brief 用于控制任务控制底盘速度
 * @param _fbV 底盘前后方向速度 m/s（+前进）
 * @param _lrV 底盘左右方向速度 m/s（+向左）
 * @param _rtV 底盘旋转速度 rpm（+俯视逆时针）
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
    /* 2026-11-16：把运动控制状态一起复位。
     * 否则急停后再起步时，斜坡/航向锁还带着急停前的旧状态，第一下会窜。 */
    slewVx = slewVy = slewW = 0.0f;
    yawHold = 0;
    chassisYawCorr = 0.0f;
    chassisDesatScale = 1.0f;
    chassisOmegaEff = 0.0f;
    chassisDbgVx = chassisDbgVy = 0.0f;
    for (uint8_t i = 0; i < 4; ++i) wheelTargetRpm[i] = 0;
    CMFL.Stop();
    CMFR.Stop();
    CMBL.Stop();
    CMBR.Stop();
}

/**
 * @brief 底盘运动学解算 + 运动控制（1 kHz）
 *
 * 数据流（每一层都可以单独关，方便 A/B）：
 *   摇杆速度指令
 *     → ① 斜坡限幅（CHASSIS_SLEW_EN）        防打滑/电流冲击
 *     → ② IMU 航向环（CHASSIS_IMU_YAW_EN）   直行锁航向，修高速跑偏
 *     → ③ 侧向加速度限制（CHASSIS_ALAT_EN）  防高速转弯侧滑
 *     → ④ 麦轮逆解 → 四轮线速度
 *     → ⑤ 饱和归一化（CHASSIS_DESAT_EN）     超电机能力时等比缩放，保住速度矢量方向
 *     → ⑥ 每轮修正系数 WHEEL_TRIM_* → 输出轴 rpm 目标（车体系）
 *     → 乘方向补偿 SIGN → Motor::SetTargetSpeed()
 *
 * ⚠️ 改方向/换电机后：**只改 SIGN_WHEEL_x**，不要动这里公式结构。
 *    改了运动学后先用 02_sim/python 的仿真对一遍正解闭合。
 */
void WheelsSpeedCalc(float fbVelocity, float lrVelocity, float rtVelocity) {
    const float dt = 0.001f;                                   // 1 kHz
    const float k  = (TRACK_WIDTH + WHEEL_BASE) * 0.5f;        // 0.39 m，旋转等效力臂
    /* 450 rpm 输出轴 → 轮子地面线速度上限（m/s） */
    const float wheelLinMax = CHASSIS_WHEEL_RPM_MAX * 3.1415926f * WHEEL_DIAMETER / 60.0f;

    float vx = fbVelocity;                 // m/s，车体前后
    float vy = lrVelocity;                 // m/s，车体左右（+左）
    float w  = RPM2RADpS(rtVelocity);      // rad/s，车体自转（+逆时针）

    /* ================= ① 指令斜坡 =================
     * 摇杆是阶跃输入。不限加速度时，1 ms 内目标从 0 跳到 376 rpm，
     * P 环误差 376 rpm → 电流指令 kp*376=45120，被限幅到 12000 = 14.6 A/轮，
     * 四轮 58 A 瞬间拉爆电池架 35 A 上限 → 电压跌落 → 四轮同时掉速 → 车"发飘"。
     * 限 6 m/s² 后，0→3 m/s 用 0.5 s，电流峰值显著下降，而且轮胎不空转。 */
#if CHASSIS_SLEW_EN
    /* 加速/减速用不同斜率：|目标| 比当前小（或两者反向）就是在刹车。
     * 刹车斜率可以更大 —— 松杆后 0.3 s 内把 3 m/s 收到 0，操作上"跟手"。 */
    {
        float stepX = (fabsf(vx) >= fabsf(slewVx) && vx * slewVx >= 0.0f)
                      ? CHASSIS_ACC_LIN * dt : CHASSIS_DEC_LIN * dt;
        float stepY = (fabsf(vy) >= fabsf(slewVy) && vy * slewVy >= 0.0f)
                      ? CHASSIS_ACC_LIN * dt : CHASSIS_DEC_LIN * dt;
        slewVx = SlewToward(slewVx, vx, stepX);
        slewVy = SlewToward(slewVy, vy, stepY);
        slewW  = SlewToward(slewW,  w,  CHASSIS_ACC_ANG * dt);
    }
#else
    slewVx = vx; slewVy = vy; slewW = w;
#endif
    vx = slewVx; vy = slewVy; w = slewW;
    /* ================= ⑦ 横移前后偏移交叉补偿（标定值，默认 0） =================
     * 纯横移时前向理论分量应为 0，但麦轮辊子摩擦/装配不对称会产生与 vy 成正比的
     * 纵向漂移（实测 10~20%）。给 vx 叠加上 -K·vy 抵消它：
     *   K>0 = 向左平移（vy>0）时车会向前漂；向右平移 vy 变号 → 自动反向补偿。
     * 标定方法见 ChassisTask.h 的 CHASSIS_STRAFE_FB_COMP 注释（需要卷尺量 2 m）。 */
    vx -= CHASSIS_STRAFE_FB_COMP * vy;

    float vmagSlew = sqrtf(vx * vx + vy * vy);   /* 平移速度大小（后面两处都要用） */

    /* ================= ② IMU 直行航向锁 =================
     * 只有直行意图（|ω| < CHASSIS_YAW_TURN_THR）才锁航向：
     *   · 进入直行时把当前航向记成目标；之后只修正"漂"出去的那部分
     *   · 驾驶员在转弯（|ω| 大）时不锁，只做轻量的角速度阻尼
     * 为什么这样设计：航向锁只负责"把直行跑偏拉回来"，
     * 不做"命令角速度 → 目标航向"的轨迹跟踪（那需要更细的标定，先不上）。
     * 关键前提：IMU 坏掉 / 读不到时 IMU_Ready()=0，整段自动跳过，行为退回旧版。 */
    float gzDps = gzYawDps;                /* yaw 轴角速度：YawUpdate() 每 1 ms 更新 */
    uint8_t imuOk = chassisImuOk;          /* IMU 健康状态：YawUpdate() 判好 */

    float wEff = w;                        /* 最终参与逆解的自转指令 rad/s */
    float wCorr = 0.0f;
#if CHASSIS_IMU_YAW_EN
    if (imuOk) {
        float gzRad = gzDps * DEG2RAD;
        if (vmagSlew < CHASSIS_YAW_MIN_SPEED) {
            /* 车基本停着：不做航向修正（否则陀螺零偏积分几分钟后，停着的车会自己慢慢转），
             * 目标航向跟随当前 —— 下次起步从"现在的朝向"重新锁。 */
            yawHold = 0;
            yawTargetDeg = yawDeg;
        } else if (fabsf(w) > CHASSIS_YAW_TURN_THR) {
            /* 转弯中：不锁航向，目标持续跟随当前，保证下一段直行从"现在的朝向"开始锁 */
            yawHold = 0;
            yawTargetDeg = yawDeg;
        } else {
            if (!yawHold) {
                /* 刚进入直行：把当前航向记为目标（此时修正量为 0，没有跳变） */
                yawHold = 1;
                yawTargetDeg = yawDeg;
            }
            float errRad = WrapDeg180(yawTargetDeg - yawDeg) * DEG2RAD;
            /* PD：角度误差给回复力，角速度项抑制 200 Hz 采样下的抖动 */
            wCorr = CHASSIS_YAW_KP * errRad - CHASSIS_YAW_KD * gzRad;
            INRANGE(wCorr, -CHASSIS_YAW_CORR_MAX, CHASSIS_YAW_CORR_MAX);
            wEff = w + wCorr;
        }
    } else {
        /* IMU 中途掉线：清掉锁状态，等恢复后从"当前朝向"重新锁，
         * 否则会拿几秒前（甚至几分钟前）的目标航向去做一次突然的大修正。 */
        yawHold = 0;
        yawTargetDeg = yawDeg;
    }
#else
    yawHold = 0;
    yawTargetDeg = yawDeg;   /* 未启用时只做显示/日志，保证 yawErr 字段不乱 */
#endif
    chassisYawTargetDeg = yawTargetDeg;
    chassisYawCorr = wCorr;

    /* ================= ③ 侧向加速度限制 =================
     * 车体一边平移一边自转时，地面速度矢量要跟着转，需要的侧向加速度：
     *     a_lat = |ω| · |v|
     * 麦轮只有约 0.45g 的侧向抓地。超过就滑，滑的方向/量都不可控 → 侧移。
     * 处理：**只砍平移速度、保留自转角速度**。于是转弯半径被放大到抓地允许的
     * 最小值，但车头朝向仍然听摇杆的（对操作手感最重要）。 */
    {
        float vmag = vmagSlew;    /* 平移速度没被上一级改过，直接复用 */
        if (CHASSIS_ALAT_EN && vmag > 0.05f) {
            float alat = fabsf(wEff) * vmag;
            if (alat > CHASSIS_ALAT_MAX) {
                float s = CHASSIS_ALAT_MAX / alat;
                vx *= s;
                vy *= s;
            }
        }
    }

    /* ================= ④ 麦轮逆解 =================
     * 约定（与 02_sim/python 仿真一致，单位 m/s）：
     *   fb=1, lr=0, ω=0  →  四轮 +1.00（直行）
     *   fb=0, lr=1, ω=0  →  -1, +1, +1, -1（向左平移）
     *   fb=0, lr=0, ω=1  →  ∓k（原地逆时针） */
    float vFL = vx - vy - k * wEff;
    float vFR = vx + vy + k * wEff;
    float vBL = vx + vy - k * wEff;
    float vBR = vx - vy + k * wEff;

    /* ================= ⑤ 饱和归一化 =================
     * 四个轮速是同一个 (vx,vy,ω) 的线性组合：只要有一个超过电机能力，
     * 直接把它削到上限会让速度矢量方向歪掉（这就是"满杆斜推却往一边滑"的来源）。
     * 正确做法：四个**等比**缩放（方向不变，只是整体慢一点）。
     * 例：vx=3, vy=1.5 → FR/BL 需要 3.68 m/s > 3.59 m/s 上限
     *     → 缩放 0.975，实际 2.93/1.46，方向还是摇杆方向。 */
    float wmax = fabsf(vFL);
    if (fabsf(vFR) > wmax) wmax = fabsf(vFR);
    if (fabsf(vBL) > wmax) wmax = fabsf(vBL);
    if (fabsf(vBR) > wmax) wmax = fabsf(vBR);

    float scale = 1.0f;
    if (CHASSIS_DESAT_EN && wmax > wheelLinMax && wmax > 1e-6f) {
        scale = wheelLinMax / wmax;
    }
    chassisDesatScale = scale;
    /* 对外显示"最终真正要执行的等效车体速度"，便于串口/SWD/日志对照 */
    chassisDbgVx    = vx * scale;
    chassisDbgVy    = vy * scale;
    chassisOmegaEff = wEff * scale;

    /* ================= ⑥ 换算成输出轴 rpm 并下发 =================
     * wheelTargetRpm 一律是**车体系**（正 = 车前进方向），
     * 只在最后一行乘 SIGN_WHEEL_x（+1/-1 = 电机安装方向补偿）。 */
    wheelTargetRpm[0] = RADpS2RPM((vFL * scale) / (WHEEL_DIAMETER / 2.0f)) * WHEEL_TRIM_FL;
    wheelTargetRpm[1] = RADpS2RPM((vFR * scale) / (WHEEL_DIAMETER / 2.0f)) * WHEEL_TRIM_FR;
    wheelTargetRpm[2] = RADpS2RPM((vBL * scale) / (WHEEL_DIAMETER / 2.0f)) * WHEEL_TRIM_BL;
    wheelTargetRpm[3] = RADpS2RPM((vBR * scale) / (WHEEL_DIAMETER / 2.0f)) * WHEEL_TRIM_BR;

    CMFL.SetTargetSpeed(wheelTargetRpm[0] * SIGN_WHEEL_FL);
    CMFR.SetTargetSpeed(wheelTargetRpm[1] * SIGN_WHEEL_FR);
    CMBL.SetTargetSpeed(wheelTargetRpm[2] * SIGN_WHEEL_BL);
    CMBR.SetTargetSpeed(wheelTargetRpm[3] * SIGN_WHEEL_BR);
}
