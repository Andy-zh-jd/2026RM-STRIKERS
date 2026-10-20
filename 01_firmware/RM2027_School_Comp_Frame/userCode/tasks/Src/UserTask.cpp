//
// Created by LEGION on 2021/10/4.
//
#include "UserTask.h"
#include <stdio.h>
#include <stdarg.h>
#include "RemoteControl.h"          // rcInfo（遥控器）
extern volatile uint8_t key_raw_state;
// 2026-10-17：vccBat 没写在任何头文件里，要自己 extern（Device.cpp:117）
// 用途：接电池后先看这个数对不对（换算系数 /22*222 疑似笔误，要用万用表核对）
extern volatile float vccBat;
// 2026-10-20：底盘四个电机对象定义在 ChassisTask.cpp，这里只是引用（CMFL = ID1 左前轮）
extern Motor CMFL;

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

    if (isAccel)
    {
        CS_ACCEL_L();
        HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3, 100); // 地址 + dummy + 数据
        CS_ACCEL_H();
        return rx[2]; // 加速度计要跳过 1 个 dummy
    }
    else
    {
        CS_GYRO_L();
        HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, 100); // 地址 + 数据
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
        HAL_SPI_Transmit(&hspi1, tx, 2, 100);          // 写不用 Receive
        CS_ACCEL_H();
    } else {
        CS_GYRO_L();
        HAL_SPI_Transmit(&hspi1, tx, 2, 100);
        CS_GYRO_H();
    }
}

int16_t g_gyro[3] = {0}, g_accel[3] = {0};

/* 连续读 n 个字节 */
static void IMU_ReadBytes(uint8_t isAccel, uint8_t reg, uint8_t *buf, uint8_t n)
{
    uint8_t tx[8] = {0};
    uint8_t rx[8] = {0};
    uint8_t len = (uint8_t)(n + (isAccel ? 2 : 1));   // 加速度计多 1 个 dummy
    tx[0] = (uint8_t)(reg | 0x80);

    if (isAccel) {
        CS_ACCEL_L();
        HAL_SPI_TransmitReceive(&hspi1, tx, rx, len, 100);
        CS_ACCEL_H();
        for (uint8_t i = 0; i < n; i++) buf[i] = rx[2 + i];
    } else {
        CS_GYRO_L();
        HAL_SPI_TransmitReceive(&hspi1, tx, rx, len, 100);
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
    .ki = 0,
    .kd = 0,
    .componentKpMax = 10000,
    .componentKiMax = 0,
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
}



/***
 * 用户自定义任务主循环
 */

/* ---------- 拼行缓冲区：攒够一整行再一次性发 ---------- */
static char g_line[128];

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


void UserHandle()
{
    static uint32_t cnt = 0;
    static uint8_t  page = 0;        // A/B 两页交替，避免一行太长被截断

    IMU_Update();                    // ★ 每 1ms 采样（1kHz）

    if (++cnt >= 100) {              // 每 100ms 打一行，A/B 各 5 Hz
        cnt = 0;
        g_line[0] = '\0';

        if (page == 0) {
            /* ===== A 行：姿态 + 电池电压 ===== */
            AppendF2("gx=", g_gyroLpf[0]);       // 陀螺 °/s（滤波后）
            AppendF2("gy=", g_gyroLpf[1]);
            AppendF2("gz=", g_gyroLpf[2]);
            AppendF2("ax=", g_accelG[0]);        // 加速度 g（静止 az≈1）
            AppendF2("ay=", g_accelG[1]);
            AppendF2("az=", g_accelG[2]);
            AppendF2("vbat=", vccBat);           // 电池电压（换算可疑，用万用表对）
        } else {
            /* ===== B 行：电机 CMFL(ID1) + 遥控器 ===== */
            AppendF2("rpm=",  CMFL.state.speed);        // 输出轴转速 RPM（反馈）
            AppendF2("tgt=",  CMFL.targetSpeed);        // 目标转速 RPM（指令）
            AppendI("cur=",   (int)CMFL.state.moment);  // 实际电流，±16384 ≈ ±20A
            AppendF2("ang=",  CMFL.state.angle);        // 输出轴累计角度（度）
            AppendF2("T=",    CMFL.state.temperature);  // 电机温度（℃），0=没收到帧
            AppendI("stop=",  (int)CMFL.stopFlag);      // 1=不输出（安全态）
            AppendI("sL=",    (int)RemoteControl::rcInfo.sLeft);   // 97上/0中/158下
            AppendI("sR=",    (int)RemoteControl::rcInfo.sRight);
            AppendF2("col=",  RemoteControl::rcInfo.right_col);    // -1~1
        }
        page ^= 1;
        UsbPrintf("%s\r\n", g_line);
    }
    UserMotor.Handle();
}