//
// Created by LEGION on 2021/10/4.
//
#include "UserTask.h"
#include <stdio.h>
#include <stdarg.h>
#include "RemoteControl.h"          // rcInfo（遥控器）
#include "ChassisTask.h"            // 2026-10-23：wheelTargetRpm/底盘接口（打印四轮目标用）
extern volatile uint8_t key_raw_state;
// 2026-10-17：vccBat 没写在任何头文件里，要自己 extern（Device.cpp:117）
// 用途：接电池后先看这个数对不对（换算系数 /22*222 疑似笔误，要用万用表核对）
extern volatile float vccBat;
// 2026-10-20：底盘四个电机对象定义在 ChassisTask.cpp，这里只是引用（CMFL = ID1 左前轮）
extern Motor CMFL, CMFR, CMBL, CMBR;
/* 2026-10-23 诊断计数（定义在 Motor.cpp 的 CAN 接收中断里） */
extern volatile uint32_t g_rx203, g_rxOther;

/* ===== 调试开关 =====
 * DBG_MINIMAL = 1 → 只打印，不读 IMU（跳过 SPI）、不碰机构。
 *   用来隔离「USB 能枚举但 COM 口打不开」这类问题：
 *   最小模式下 USB 正常 → 说明是某个阻塞外设（最可能是 SPI 读 IMU 超时）拖死了 1ms 中断。
 * DBG_UART = 1 → 同一行数据同时从 UART1（PA9=TX，115200）发出去，USB 挂了也能用 CH340 看。
 *   ⚠️ HAL_UART_Transmit 是阻塞的（60 字节≈5ms），塞进 1ms 中断会拖垮 USB 控制传输，默认关。 */
#define DBG_MINIMAL 1/* 1 = IMU 只读一次 chip id 然后彻底不读（体检用）*/
#define DBG_IMU_IDONLY 1
/* IMU 周期读取的分频：1ms 中断里每 IMU_DIV 次读一次
 *   IMU_DIV=100 → 10Hz（T1 测试）；5 → 200Hz（最终目标）；1 → 1kHz（之前会卡死）*/
#define IMU_DIV 5
#define DBG_UART    0
/* 2026-10-23 实测结论：**不能在 1ms 中断里调用 CDC_Transmit_FS**。
 * 现象：设备能枚举（COM 口在、PnP 正常），但打开永远报
 *       Cannot configure port / OSError 22 / Win32 错误 87；
 *       把这一行打印关掉（DBG_USB_PRINT 0）→ 串口立刻能打开。
 * 修法：中断里只**拼字符串**（便宜），真正的发送放到主循环 UserPollSlow() 里做。
 * DBG_USB_PRINT = 0 时完全不打 USB，数据仍写 g_dbg[]，可用 tools/read_swd.py 看。 */
#define DBG_USB_PRINT 1

/* IMU 的 SPI 读超时：原来是 100ms —— 一旦 SPI 卡一下，1ms 中断就被占住 100ms，
 * USB 控制传输彻底没机会响应，表现就是「设备能枚举、但 COM 口打不开(参数错误 87)」。
 * 2026-10-20 实测确认：跳过 IMU 后串口立刻正常。所以收紧到 1ms，且 SPI 忙时跳过本帧。 */
#define IMU_SPI_TIMEOUT_MS 1

void UsbPrintf(const char *fmt, ...)
{
    static char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
    {
        if (len > 255)
            len = 255;
        CDC_Transmit_FS((uint8_t *)buf, (uint16_t)len);
    }
}

// read chip id
/* ===================== BMI088 底层驱动 ===================== */
#define CS_ACCEL_L() HAL_GPIO_WritePin(CS1_ACCEL_GPIO_Port, CS1_ACCEL_Pin, GPIO_PIN_RESET)
#define CS_ACCEL_H() HAL_GPIO_WritePin(CS1_ACCEL_GPIO_Port, CS1_ACCEL_Pin, GPIO_PIN_SET)
#define CS_GYRO_L() HAL_GPIO_WritePin(CS1_GYRO_GPIO_Port, CS1_GYRO_Pin, GPIO_PIN_RESET)
#define CS_GYRO_H() HAL_GPIO_WritePin(CS1_GYRO_GPIO_Port, CS1_GYRO_Pin, GPIO_PIN_SET)

uint8_t g_accelId = 0, g_gyroId = 0;

/* 读一个寄存器：isAccel=1 读加速度计，=0 读陀螺仪 */
static uint8_t IMU_ReadReg(uint8_t isAccel, uint8_t reg)
{
    uint8_t tx[3] = {(uint8_t)(reg | 0x80), 0x00, 0x00}; // 最高位置 1 = 读
    uint8_t rx[3] = {0};

    /* SPI 上次没做完 → 直接跳过这一帧，绝不阻塞（否则会拖死 1ms 中断和 USB） */
    if (hspi1.State != HAL_SPI_STATE_READY) return 0;

    if (isAccel)
    {
        CS_ACCEL_L();
        HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3, IMU_SPI_TIMEOUT_MS); // 地址 + dummy + 数据
        CS_ACCEL_H();
        return rx[2]; // 加速度计要跳过 1 个 dummy
    }
    else
    {
        CS_GYRO_L();
        HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, IMU_SPI_TIMEOUT_MS); // 地址 + 数据
        CS_GYRO_H();
        return rx[1]; // 陀螺仪不用 dummy
    }
}

/* 写一个寄存器：isAccel=1 写加速度计，=0 写陀螺仪 */
static void IMU_WriteReg(uint8_t isAccel, uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), data };   // 最高位清 0 = 写

    if (isAccel) {
        CS_ACCEL_L();
        HAL_SPI_Transmit(&hspi1, tx, 2, IMU_SPI_TIMEOUT_MS);          // 写不用 Receive
        CS_ACCEL_H();
    } else {
        CS_GYRO_L();
        HAL_SPI_Transmit(&hspi1, tx, 2, IMU_SPI_TIMEOUT_MS);
        CS_GYRO_H();
    }
}

int16_t g_gyro[3] = {0}, g_accel[3] = {0};
/* SPI 连续失败计数：超过阈值就彻底停读 IMU，避免坏传感器反复拖动 1ms 中断 */
static uint8_t g_spiErr = 0;
#define IMU_MAX_FAILS 5

/* 连续读 n 个字节 */
static void IMU_ReadBytes(uint8_t isAccel, uint8_t reg, uint8_t *buf, uint8_t n)
{
    uint8_t tx[8] = {0};
    uint8_t rx[8] = {0};
    uint8_t len = (uint8_t)(n + (isAccel ? 2 : 1));   // 加速度计多 1 个 dummy
    tx[0] = (uint8_t)(reg | 0x80);

    /* 同上：SPI 忙就跳过，不阻塞 */
    if (hspi1.State != HAL_SPI_STATE_READY) return;

    if (isAccel) {
        CS_ACCEL_L();
        { HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, len, IMU_SPI_TIMEOUT_MS); if (st != HAL_OK) { if (g_spiErr < 255) g_spiErr++; } else g_spiErr = 0; }
        CS_ACCEL_H();
        for (uint8_t i = 0; i < n; i++) buf[i] = rx[2 + i];
    } else {
        CS_GYRO_L();
        { HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, len, IMU_SPI_TIMEOUT_MS); if (st != HAL_OK) { if (g_spiErr < 255) g_spiErr++; } else g_spiErr = 0; }
        CS_GYRO_H();
        for (uint8_t i = 0; i < n; i++) buf[i] = rx[1 + i];
    }
}

/* 读一次六轴数据 */
/* 量程系数（对应你在 UserInit 里设的量程） */
#define ACC_SEN   10920.0f    // ±3g  → 每 1g 对应 10920 LSB
#define GYRO_SEN  16.384f     // ±2000dps → 每 1°/s 对应 16.384 LSB

float g_gyroBias[3] = {0};
float g_gyroDps[3]  = {0};
float g_accelG[3]   = {0};

static void IMU_Update(void);
/* 开机标定：调用前必须让车完全静止（别碰桌子！）*/
void IMU_Calibrate(void)
{
    float sum[3] = {0};
    if (DBG_MINIMAL) return;      /* 最小调试模式下不标定 */
    for (int i = 0; i < 500; i++) {
        IMU_Update();
        sum[0] += g_gyro[0];
        sum[1] += g_gyro[1];
        sum[2] += g_gyro[2];
        HAL_IWDG_Refresh(&hiwdg);      // ★ 加这一行！否则标定跑不完就被复位
        HAL_Delay(2);                 // 总共约 1 秒
    }
    g_gyroBias[0] = sum[0] / 500.0f;
    g_gyroBias[1] = sum[1] / 500.0f;
    g_gyroBias[2] = sum[2] / 500.0f;
}

/* 一阶低通。1kHz 采样下 alpha=0.9 → 截止频率约 17Hz */
#define GYRO_LPF  0.90f

float g_gyroLpf[3] = {0};

static void IMU_Update(void)
{
    if (g_spiErr >= IMU_MAX_FAILS) return;   /* IMU 已判死：不再尝试，绝不拖累 1ms 中断 */
    uint8_t b[6] = {0};

    IMU_ReadBytes(0, 0x02, b, 6);
    g_gyro[0] = (int16_t)((b[1] << 8) | b[0]);
    g_gyro[1] = (int16_t)((b[3] << 8) | b[2]);
    g_gyro[2] = (int16_t)((b[5] << 8) | b[4]);

    IMU_ReadBytes(1, 0x12, b, 6);
    g_accel[0] = (int16_t)((b[1] << 8) | b[0]);
    g_accel[1] = (int16_t)((b[3] << 8) | b[2]);
    g_accel[2] = (int16_t)((b[5] << 8) | b[4]);

    for (int i = 0; i < 3; i++) {
        g_gyroDps[i] = ((float)g_gyro[i] - g_gyroBias[i]) / GYRO_SEN;      // 去零偏
        g_gyroLpf[i] = g_gyroLpf[i] * GYRO_LPF
                     + g_gyroDps[i] * (1.0f - GYRO_LPF);                    // 低通
        g_accelG[i]  = (float)g_accel[i] / ACC_SEN;
    }
}








PID_Regulator_t userPidRegulator = {
    .kp = 60,
    // 2026-10-21：加积分消静差。实测纯 P 时 tgt=20.7 → rpm 只到 14.3（静差 30%）。
    // 注意框架的积分是 errSum += err（不乘 dt），所以 ki 量级是 0.1~1.0，不是 10。
    .ki = 0.5f,
    .kd = 0,
    .componentKpMax = 10000,
    // ki 和 componentKiMax 必须一起改：限幅公式是 INRANGE(errSum, ±componentKiMax/ki)，
    // 只改 ki 而 componentKiMax=0 → 积分立刻被压成 0，等于没加 I。
    // 6000 ≈ 稳态能贡献的最大电流（占满量程 10000 的 60%）。
    .componentKiMax = 6000,
    .componentKdMax = 0,
    .outputMax = 10000 // 2006电机输出电流上限，可以调小，勿调大
};

MOTOR_INIT_t userMotorInit = {
    .speedPIDp = &userPidRegulator,
    .anglePIDp = nullptr,
    ._motorID = MOTOR_ID_1,
    .reductionRatio = 36.0f,
    .ctrlType = SPEED_Single,
};

// 在这里初始化除了4个M3508轮电机之外的电机
Motor UserMotor(MOTOR_ID_5, &userMotorInit);

/***
 * 在这里放入xxx.stop()即可使舵机,电机在遥控器急停挡位断电
 */
void UserStop()
{
    UserMotor.Stop();
}

/***
 * 在这里写入初始化内容
 */
void UserInit()
{
    /* ===== 2026-10-21：IMU 体检模式 =====
     * 只读一次 chip id（预期 加速度计 0x1E / 陀螺仪 0x0F），然后彻底不读 IMU。
     * 目的：判断 BMI088 是活着还是坏了（带 IMU 的完整固件会把 USB 拖死，无法调试）。 */
#if DBG_IMU_IDONLY
    IMU_ReadReg(1, 0x00);          /* 假读一次：把加速度计从 I2C 切到 SPI（CS 上升沿） */
    HAL_Delay(10);
    g_accelId = IMU_ReadReg(1, 0x00);   /* 真正的 ID 读 */
    g_gyroId  = IMU_ReadReg(0, 0x00);
    return;                        /* 不做唤醒、不做标定、不进周期读 */
#else
    /* ---- 1. 先把加速度计从 I2C 切到 SPI ---- */
    IMU_ReadReg(1, 0x00);       // 虚假读取，结果作废
    HAL_Delay(10);

    /* ---- 2. 唤醒加速度计（关键！）---- */
    IMU_WriteReg(1, 0x7D, 0x04);   // ACC_PWR_CTRL: 打开加速度计（默认是关的）
    HAL_Delay(5);
    IMU_WriteReg(1, 0x7C, 0x00);   // ACC_PWR_CONF: 从 suspend 切到 active（默认 0x03）
    HAL_Delay(5);
    IMU_WriteReg(1, 0x40, 0xA8);   // ACC_CONF: 正常模式，ODR 800Hz
    IMU_WriteReg(1, 0x41, 0x00);   // ACC_RANGE: ±3g
    HAL_Delay(50);                 // 等第一次采样完成

    /* ---- 3. 读 ID 存档（顺便复查模式切换没被打断）---- */
    g_accelId = IMU_ReadReg(1, 0x00);
    g_gyroId  = IMU_ReadReg(0, 0x00);

    IMU_Calibrate();      // ★ 标定（这一秒内别碰板子！）
#endif
}



/***
 * 用户自定义任务主循环
 */

/* ===== 2026-10-21：IMU 读取从 1ms 中断搬到主循环 =====
 * 现象：只要在 1ms 中断里做 SPI 读（哪怕 10Hz），USB 就会「能枚举但打不开」。
 * 做法：中断里只置一个请求标志（微秒级），真正的 SPI 传输放到 while(1) 里做。
 * 这也是标准的「中断做实时、主循环做慢速 IO」分层。 */
static volatile uint8_t imuReadReq = 0;
uint32_t g_imuReadCount = 0;      /* 累计实际读了多少次（可在串口看）*/

/* 拼行缓冲区：攒够一整行再一次性发（中断里拼，主循环发） */
static char g_line[192];
static char g_tx[192];
static volatile uint8_t g_txReady = 0;

void UserPollSlow(void)
{
    if (imuReadReq) {
        imuReadReq = 0;
        IMU_Update();
        g_imuReadCount++;
    }

    /* 2026-10-23：USB 打印挪到主循环发（中断里只拼字符串）。
     * 关中断只为了"拷走 + 清标志"这几微秒，发送本身在开中断状态下做。 */
    if (g_txReady) {
        static char local[sizeof(g_tx)];
        __disable_irq();
        memcpy(local, g_tx, sizeof(local));
        g_txReady = 0;
        __enable_irq();
        UsbPrintf("%s\r\n", local);
    }
}

/* SWD 快照区：8 个 int32，见 UserHandle 里的赋值说明 */
volatile int32_t g_dbg[16];

/* 调试打印看哪个电机：
 *   M2006 + C610  → UserMotor（ID5，0x1FF 帧，反馈 0x205，减速比 36）
 *   M3508 + C620  → CMFL     （ID1，0x200 帧，反馈 0x201，减速比 19.2032）
 * 换电机时只改这一行。 */
#define DBG_MOTOR  UserMotor

static void AppendF2(const char *name, float v)
{
    int32_t s = (int32_t)(v * 100.0f);
    const char *sign = "";
    if (s < 0) { sign = "-"; s = -s; }      // 负数单独处理，避免丢符号

    uint32_t used = strlen(g_line);
    snprintf(&g_line[used], sizeof(g_line) - used,
             "%s%s%d.%02d ", name, sign, s / 100, s % 100);
}

/* 整数型字段：开关量、电流原值、档位枚举 */
static void AppendI(const char *name, int v)
{
    uint32_t used = strlen(g_line);
    snprintf(&g_line[used], sizeof(g_line) - used, "%s%d ", name, v);
}


/* 给机构电机（UserMotor，ID5）设目标转速：单位 输出轴 rpm，正负 = 方向。
 * 由 ControlTask.cpp 的 CtrlHandle() 调用（左三档 SB 中位时，右摇杆控制）。
 * 注意：SetTargetSpeed 会清 stopFlag，所以这是“允许输出”的唯一开关；
 *      SC 拨到下档时 CtrlHandle 会调 UserStop() 重新扣上。 */
void UserSetSpeed(float _rpm)
{
    UserMotor.SetTargetSpeed(_rpm);
}
void UserHandle()
{
    static uint32_t cnt = 0;
    static uint8_t  page = 0;        // A/B 两页交替，避免一行太长被截断

    /* IMU 降频到 200Hz（每 5ms 读一次）：1ms 中断里做 SPI 太挤，且 200Hz 对姿态足够 */
    static uint32_t imuDiv = 0;
    if (!DBG_MINIMAL) { if (++imuDiv >= IMU_DIV) { imuDiv = 0; imuReadReq = 1; } }

    if (++cnt >= 100) {              // 每 100ms 打一行，A/B 各 5 Hz
        cnt = 0;
        g_line[0] = '\0';

        /* ===== 2026-10-21：SWD 快照区 =====
         * USB CDC 打不开时，用 ST-Link 直接读这块 RAM 就能看数据（完全绕开 USB）。
         * 单位：转速/电压/摇杆 ×100，电流/档位 原值。 */
        g_dbg[0] = (int32_t)(UserMotor.targetSpeed * 100.0f);
        g_dbg[1] = (int32_t)(UserMotor.state.speed * 100.0f);
        g_dbg[2] = (int32_t)UserMotor.state.moment;
        g_dbg[3] = (int32_t)UserMotor.stopFlag;
        g_dbg[4] = (int32_t)(vccBat * 100.0f);
        g_dbg[5] = (int32_t)RemoteControl::rcInfo.sLeft;
        g_dbg[6] = (int32_t)RemoteControl::rcInfo.sRight;
        g_dbg[7] = (int32_t)(RemoteControl::rcInfo.right_col * 100.0f);
        g_dbg[8]  = (int32_t)(CMFL.state.speed * 100.0f);   // 底盘四轮（ID1~4）
        g_dbg[9]  = (int32_t)(CMFR.state.speed * 100.0f);
        g_dbg[10] = (int32_t)(CMBL.state.speed * 100.0f);
        g_dbg[11] = (int32_t)(CMBR.state.speed * 100.0f);
        g_dbg[12] = (int32_t)CMFL.stopFlag;                  // 底盘安全态
        g_dbg[13] = (int32_t)(CMFL.targetSpeed * 100.0f);   // 底盘目标转速（逆解结果）
        /* 2026-10-23 诊断：原始 CAN 收帧计数（用 tools/read_swd.py 读 g_dbg 的 14/15 槽）
         *   [14] = 收到多少 0x203 帧（ID3 电调的反馈）
         *   [15] = 收到多少其它 ID 的帧
         * 判据：[14] 一直为 0 → 那个电调压根没发帧（硬件问题）；
         *        [14] 在涨而 a2 为 0 → 是代码问题。 */
        g_dbg[14] = (int32_t)g_rx203;
        g_dbg[15] = (int32_t)g_rxOther;

        if (page == 0) {
            /* ===== A 行：姿态 + 电池电压 ===== */
            AppendF2("gx=", g_gyroLpf[0]);       // 陀螺 °/s（滤波后）
            AppendF2("gy=", g_gyroLpf[1]);
            AppendF2("gz=", g_gyroLpf[2]);
            AppendF2("ax=", g_accelG[0]);        // 加速度 g（静止 az≈1）
            AppendF2("ay=", g_accelG[1]);
            AppendF2("az=", g_accelG[2]);
            AppendI("aid=", (int)g_accelId);      // 加速度计 ID，正常 0x1E = 十进制 30
            AppendI("gid=", (int)g_gyroId);       // 陀螺仪 ID，正常 0x0F = 十进制 15
            AppendI("serr=", (int)g_spiErr);      // SPI 连续失败计数
            AppendI("icnt=", (int)g_imuReadCount); // 主循环里实际读了多少次
            AppendF2("vbat=", vccBat);           // 电池电压（换算可疑，用万用表对）
            /* 2026-10-23：四路电调的**转子角度**（0~8191，C620 反馈帧原始值）。
             * 作用：区分"没收到帧"和"收到帧但电机不转"——
             *   a_n 一直是 0        → 这个 ID 没有反馈帧（CAN/供电/ID 问题）
             *   a_n 是 0~8191 的数   → 帧在正常到达（链路通，只是电机没转）
             * 手转电机轴时 a_n 明显变化 = 最快的"链路通不通"判据。 */
            AppendI("a1=", (int)(uint16_t)CMFL.feedback.angle);
            AppendI("a2=", (int)(uint16_t)CMFR.feedback.angle);
            AppendI("a3=", (int)(uint16_t)CMBL.feedback.angle);
            AppendI("a4=", (int)(uint16_t)CMBR.feedback.angle);
        } else {
            /* ===== B 行：机构电机(M2006) 详情 + 四个底盘轮子 + 档位 ===== */
            AppendF2("m=",   UserMotor.state.speed);  // M2006 输出轴 rpm（反馈）
            AppendF2("tgt=", UserMotor.targetSpeed);  // M2006 目标 rpm
            AppendI("cur=",  (int)UserMotor.state.moment);
            AppendI("st=",   (int)UserMotor.stopFlag);
            AppendF2("r1=",  CMFL.state.speed);       // 底盘四轮（ID1~4）
            AppendF2("r2=",  CMFR.state.speed);
            AppendF2("r3=",  CMBL.state.speed);
            AppendF2("r4=",  CMBR.state.speed);
            /* 2026-10-23：四个轮子的**目标**（逆解结果/单轮调试值）。
             * 有了 t1~t4 才能区分"指令没下发"和"电机没转"：
             *   t 有数、r=0      → CAN 没通 / 电调没供电 / ID 错
             *   t=0、推摇杆没反应 → 档位或逆解没跑（看 sL/sR） */
            AppendF2("t1=",  wheelTargetRpm[0]);
            AppendF2("t2=",  wheelTargetRpm[1]);
            AppendF2("t3=",  wheelTargetRpm[2]);
            AppendF2("t4=",  wheelTargetRpm[3]);
            AppendI("wi=",   (int)g_selWheel);   // 单轮调试选中的下标：0左前 1右前 2左后 3右后
            AppendI("sL=",    (int)RemoteControl::rcInfo.sLeft);   // 97上/0中/158下
            AppendI("sR=",    (int)RemoteControl::rcInfo.sRight);
            AppendF2("col=",  RemoteControl::rcInfo.right_col);    // -1~1
            /* 2026-10-23：四通道原始值（已减 1024，范围 ±660），用来查清"哪个摇杆对应哪个通道"。
             * 推杆时看哪个 q 在动，就能把映射定下来（代码里 ch0=right_rol ch1=left_col
             * ch2=right_col ch3=left_rol，实测和发射机设置不一致）。 */
            AppendI("q0=", (int)RemoteControl::rc_ctrl.rc.ch[0]);
            AppendI("q1=", (int)RemoteControl::rc_ctrl.rc.ch[1]);
            AppendI("q2=", (int)RemoteControl::rc_ctrl.rc.ch[2]);
            AppendI("q3=", (int)RemoteControl::rc_ctrl.rc.ch[3]);
        }
        page ^= 1;
        /* ★ 不许在这里调 UsbPrintf/CDC_Transmit_FS（实测会把 USB 控制传输搞死：
         * 设备能枚举、COM 口打不开）。这里只把拼好的行交给主循环去发。 */
        if (DBG_USB_PRINT && !g_txReady) {
            memcpy(g_tx, g_line, sizeof(g_tx));
            g_txReady = 1;
        }
    }
    UserMotor.Handle();
}