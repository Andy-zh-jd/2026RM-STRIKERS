//
// Created by LEGION on 2021/10/4.
//
#include "UserTask.h"
#include "RemoteControl.h"
#include <stdio.h>
#include <stdarg.h>
extern volatile uint8_t key_raw_state;

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

/* ================= PCA9685 16 路舵机驱动（软件 I2C） ================= */
/* ---------- ① 引脚：通信不通时只改这里 ---------- */
/* 引脚扫描实测：两根线在 B13 / B15 上（F0/F1 是 C 板自带 I2C 上拉，不是线）
   注意：I2C 不是对称的，SDA/SCL 接反时不会报错，只会一直不应答 → 这里试反接 */
#define PCA_SCL_PORT    GPIOB
#define PCA_SCL_PIN     GPIO_PIN_15
#define PCA_SDA_PORT    GPIOB
#define PCA_SDA_PIN     GPIO_PIN_13
/* 备选（如果牛角座走的是 I2C2 那组）：
   #define PCA_SCL_PORT GPIOF / PCA_SCL_PIN GPIO_PIN_1 / SDA GPIOF PIN_0 */

/* ---------- ② PCA9685 寄存器与参数 ---------- */
#define PCA_ADDR        0x40
#define PCA_MODE1       0x00
#define PCA_MODE2       0x01
#define PCA_PRESCALE    0xFE
#define PCA_LED0_ON_L   0x06

/* 舵机脉宽范围（微秒）：MG996R 类 500~2500；SG90 类 1000~2000 */
#define SERVO_US_MIN    500
#define SERVO_US_MAX    2500

/* ---------- ③ 软件 I2C 底层 ---------- */
static void PCA_Delay(void)
{
    for (volatile uint32_t i = 0; i < 25; i++) { __NOP(); }
}

static void PCA_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    g.Mode  = GPIO_MODE_OUTPUT_OD;     /* 开漏：和板上的 2.2k 上拉配成标准 I2C */
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin   = PCA_SCL_PIN;  HAL_GPIO_Init(PCA_SCL_PORT, &g);
    g.Pin   = PCA_SDA_PIN;  HAL_GPIO_Init(PCA_SDA_PORT, &g);

    HAL_GPIO_WritePin(PCA_SCL_PORT, PCA_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(PCA_SDA_PORT, PCA_SDA_PIN, GPIO_PIN_SET);
}

#define SCL_H()   HAL_GPIO_WritePin(PCA_SCL_PORT, PCA_SCL_PIN, GPIO_PIN_SET)
#define SCL_L()   HAL_GPIO_WritePin(PCA_SCL_PORT, PCA_SCL_PIN, GPIO_PIN_RESET)
#define SDA_H()   HAL_GPIO_WritePin(PCA_SDA_PORT, PCA_SDA_PIN, GPIO_PIN_SET)
#define SDA_L()   HAL_GPIO_WritePin(PCA_SDA_PORT, PCA_SDA_PIN, GPIO_PIN_RESET)
#define SDA_RD()  HAL_GPIO_ReadPin(PCA_SDA_PORT, PCA_SDA_PIN)

static void I2C_Start(void)
{
    SDA_H(); SCL_H(); PCA_Delay();
    SDA_L(); PCA_Delay();
    SCL_L(); PCA_Delay();
}

static void I2C_Stop(void)
{
    SDA_L(); SCL_H(); PCA_Delay();
    SDA_H(); PCA_Delay();
}

/* 返回 1 = 收到 ACK（说明从机在线）*/
static uint8_t I2C_WriteByte(uint8_t b)
{
    for (uint8_t i = 0; i < 8; i++) {
        if (b & 0x80) SDA_H(); else SDA_L();
        b <<= 1;
        PCA_Delay();
        SCL_H(); PCA_Delay();
        SCL_L(); PCA_Delay();
    }
    SDA_H();                       /* 释放 SDA，等从机拉低 */
    PCA_Delay();
    SCL_H(); PCA_Delay();
    uint8_t ack = (SDA_RD() == GPIO_PIN_RESET) ? 1 : 0;
    SCL_L(); PCA_Delay();
    return ack;
}

static uint8_t I2C_ReadByte(void)
{
    uint8_t b = 0;
    SDA_H();
    for (uint8_t i = 0; i < 8; i++) {
        PCA_Delay();
        SCL_H(); PCA_Delay();
        b <<= 1;
        if (SDA_RD() == GPIO_PIN_SET) b |= 1;
        SCL_L(); PCA_Delay();
    }
    SDA_H(); PCA_Delay(); SCL_H(); PCA_Delay(); SCL_L();   /* 回 NACK */
    return b;
}

/* ---------- ④ PCA9685 寄存器读写 ---------- */
static void PCA_Write8(uint8_t reg, uint8_t val)
{
    I2C_Start();
    I2C_WriteByte((PCA_ADDR << 1) | 0);
    I2C_WriteByte(reg);
    I2C_WriteByte(val);
    I2C_Stop();
}

static void PCA_Write4(uint8_t reg, uint8_t *buf)
{
    I2C_Start();
    I2C_WriteByte((PCA_ADDR << 1) | 0);
    I2C_WriteByte(reg);                     /* ★ 不能加 0x80！
                                             * PCA9685 的自动递增由 MODE1 的 AI 位控制，
                                             * 不是靠寄存器地址的 bit7。加了会写到未定义寄存器。*/
    for (uint8_t i = 0; i < 4; i++) I2C_WriteByte(buf[i]);
    I2C_Stop();
}

static uint8_t PCA_Read8(uint8_t reg)
{
    uint8_t v;
    I2C_Start();
    I2C_WriteByte((PCA_ADDR << 1) | 0);
    I2C_WriteByte(reg);
    I2C_Start();                             /* 重复起始 */
    I2C_WriteByte((PCA_ADDR << 1) | 1);
    v = I2C_ReadByte();
    I2C_Stop();
    return v;
}

/* 读回某通道的 OFF 计数值（12bit），用来确认数据真的写进芯片了 */
uint16_t PCA_ReadOff(uint8_t ch)
{
    uint8_t reg = (uint8_t)(PCA_LED0_ON_L + 4 * ch + 2);   /* OFF_L */
    uint8_t l = PCA_Read8(reg);
    uint8_t h = PCA_Read8((uint8_t)(reg + 1));
    return (uint16_t)(((h & 0x0F) << 8) | l);
}

/* ---------- ⑤ 总线自检（临时诊断用，问题解决后可删） ---------- */
uint8_t g_busScl = 0, g_busSda = 0, g_scanAddr = 0;
uint8_t g_pdScl  = 0, g_pdSda  = 0, g_scanSlow = 0;

static void DlyLong(void) { for (volatile uint32_t i = 0; i < 300; i++) { __NOP(); } }

/* 慢速扫描（自带时序，约 20kHz）：用来排除"上拉太弱、上升沿太慢"导致的通信失败 */
static uint8_t PCA_ScanSlow(void)
{
    for (uint8_t a = 0x40; a <= 0x78; a++) {
        SDA_H(); SCL_H(); DlyLong();
        SDA_L(); DlyLong();
        SCL_L(); DlyLong();

        uint8_t b = (uint8_t)(a << 1);
        for (uint8_t i = 0; i < 8; i++) {
            if (b & 0x80) SDA_H(); else SDA_L();
            b <<= 1;
            DlyLong();
            SCL_H(); DlyLong();
            SCL_L(); DlyLong();
        }

        SDA_H(); DlyLong();
        SCL_H(); DlyLong();
        uint8_t ack = (HAL_GPIO_ReadPin(PCA_SDA_PORT, PCA_SDA_PIN) == GPIO_PIN_RESET) ? 1 : 0;
        SCL_L(); DlyLong();

        SDA_L(); SCL_H(); DlyLong();
        SDA_H(); DlyLong();

        if (ack) return a;
    }
    return 0;
}

void PCA_Diag(void)
{
    GPIO_InitTypeDef g = {0};

    PCA_GPIO_Init();

    /* ① 输入+上拉：读空闲电平，正常两根都是 1 */
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    g.Pin  = PCA_SCL_PIN;  HAL_GPIO_Init(PCA_SCL_PORT, &g);
    g.Pin  = PCA_SDA_PIN;  HAL_GPIO_Init(PCA_SDA_PORT, &g);
    HAL_Delay(2);
    uint8_t sclHi = (HAL_GPIO_ReadPin(PCA_SCL_PORT, PCA_SCL_PIN) == GPIO_PIN_SET) ? 1 : 0;
    uint8_t sdaHi = (HAL_GPIO_ReadPin(PCA_SDA_PORT, PCA_SDA_PIN) == GPIO_PIN_SET) ? 1 : 0;

    /* ② 开漏：逐个拉低再读回，正常都应该读到 0 */
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin   = PCA_SCL_PIN;  HAL_GPIO_Init(PCA_SCL_PORT, &g);
    g.Pin   = PCA_SDA_PIN;  HAL_GPIO_Init(PCA_SDA_PORT, &g);

    SCL_L(); HAL_Delay(1);
    uint8_t sclOk = (HAL_GPIO_ReadPin(PCA_SCL_PORT, PCA_SCL_PIN) == GPIO_PIN_RESET) ? 1 : 0;
    SCL_H();
    SDA_L(); HAL_Delay(1);
    uint8_t sdaOk = (HAL_GPIO_ReadPin(PCA_SDA_PORT, PCA_SDA_PIN) == GPIO_PIN_RESET) ? 1 : 0;
    SDA_H();
    HAL_Delay(1);

    /* ③ 输入+下拉：判断"线对面到底有没有东西"
          读到 1 = 外部有上拉（PCA 板活着且接上了）
          读到 0 = 只有内部下拉在起作用 → 那根线对面是空的 */
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    g.Pin  = PCA_SCL_PIN;  HAL_GPIO_Init(PCA_SCL_PORT, &g);
    g.Pin  = PCA_SDA_PIN;  HAL_GPIO_Init(PCA_SDA_PORT, &g);
    HAL_Delay(3);
    uint8_t sclPd = (HAL_GPIO_ReadPin(PCA_SCL_PORT, PCA_SCL_PIN) == GPIO_PIN_SET) ? 1 : 0;
    uint8_t sdaPd = (HAL_GPIO_ReadPin(PCA_SDA_PORT, PCA_SDA_PIN) == GPIO_PIN_SET) ? 1 : 0;

    /* 恢复开漏，准备通信 */
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin   = PCA_SCL_PIN;  HAL_GPIO_Init(PCA_SCL_PORT, &g);
    g.Pin   = PCA_SDA_PIN;  HAL_GPIO_Init(PCA_SDA_PORT, &g);

    /* ④ 快速扫描 0x40~0x78（PCA9685 的合法地址范围）*/
    uint8_t found = 0;
    for (uint8_t a = 0x40; a <= 0x78; a++) {
        I2C_Start();
        uint8_t ack = I2C_WriteByte((uint8_t)(a << 1));
        I2C_Stop();
        if (ack && found == 0) found = a;
        PCA_Delay();
    }

    g_busScl   = (uint8_t)(sclHi * 10 + sclOk);   /* 正常 = 11 */
    g_busSda   = (uint8_t)(sdaHi * 10 + sdaOk);   /* 正常 = 11 */
    g_pdScl    = sclPd;                           /* 正常 = 1  */
    g_pdSda    = sdaPd;                           /* 正常 = 1  */
    g_scanAddr = found;                           /* 正常 = 64 (0x40) */

    /* ⑤ 慢速再扫一遍（放在最后，耗时约 0.3 秒）*/
    g_scanSlow = found ? found : PCA_ScanSlow();
}

/* ---------- ⑤-B 候选引脚扫描：找出"哪两个针上真的接了外部上拉" ---------- */
typedef struct { GPIO_TypeDef* port; uint16_t pin; const char* name; } CAND_T;

static const CAND_T g_cand[] = {
    {GPIOB, GPIO_PIN_12, "B12"}, {GPIOB, GPIO_PIN_13, "B13"},
    {GPIOB, GPIO_PIN_14, "B14"}, {GPIOB, GPIO_PIN_15, "B15"},
    {GPIOF, GPIO_PIN_0,  "F0" }, {GPIOF, GPIO_PIN_1,  "F1" },
    {GPIOE, GPIO_PIN_9,  "E9" }, {GPIOE, GPIO_PIN_11, "E11"},
    {GPIOE, GPIO_PIN_13, "E13"}, {GPIOE, GPIO_PIN_14, "E14"},
    {GPIOC, GPIO_PIN_6,  "C6" }, {GPIOI, GPIO_PIN_6,  "I6" },
    {GPIOI, GPIO_PIN_7,  "I7" },
};
#define CAND_N  (sizeof(g_cand)/sizeof(g_cand[0]))

char g_pinStr[80] = "none";      /* 扫到的引脚名，用逗号隔开 */

void PIN_Scan(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Mode  = GPIO_MODE_INPUT;
    g.Pull  = GPIO_PULLDOWN;        /* 内部下拉：外部有上拉的针才会读高 */
    g.Speed = GPIO_SPEED_FREQ_LOW;

    g_pinStr[0] = '\0';
    uint8_t n = 0;

    for (uint32_t i = 0; i < CAND_N; i++) {
        g.Pin = g_cand[i].pin;
        HAL_GPIO_Init(g_cand[i].port, &g);
        HAL_Delay(2);
        if (HAL_GPIO_ReadPin(g_cand[i].port, g_cand[i].pin) == GPIO_PIN_SET) {
            if (n > 0) strncat(g_pinStr, ",", sizeof(g_pinStr) - strlen(g_pinStr) - 1);
            strncat(g_pinStr, g_cand[i].name, sizeof(g_pinStr) - strlen(g_pinStr) - 1);
            n++;
        }
    }
    if (n == 0) strncpy(g_pinStr, "none", sizeof(g_pinStr) - 1);

    /* 扫描把引脚都改成了输入，这里恢复 SDA/SCL 为开漏 */
    PCA_GPIO_Init();
}

/* ---------- ⑥ 舵机输出 ---------- */
/* 设脉宽，单位微秒。us=0 → 输出常低 = 无脉冲 = 舵机卸力 */
void PCA_SetPulse(uint8_t ch, uint16_t us)
{
    uint8_t reg = (uint8_t)(PCA_LED0_ON_L + 4 * ch);
    uint8_t buf[4];

    if (us == 0) {                            /* 卸力：强制输出常低 */
        buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x10;
    } else {
        uint16_t off = (uint16_t)((uint32_t)us * 4096UL / 20000UL);  /* 50Hz→20ms */
        if (off > 4095) off = 4095;
        buf[0] = 0x00;                        /* ON_L  */
        buf[1] = 0x00;                        /* ON_H  */
        buf[2] = (uint8_t)(off & 0xFF);       /* OFF_L */
        buf[3] = (uint8_t)(off >> 8);         /* OFF_H */
    }
    PCA_Write4(reg, buf);
}

/* 设角度 0~180° */
void PCA_SetAngle(uint8_t ch, float angle)
{
    if (angle < 0.0f)   angle = 0.0f;
    if (angle > 180.0f) angle = 180.0f;
    uint16_t us = (uint16_t)(SERVO_US_MIN +
                             (SERVO_US_MAX - SERVO_US_MIN) * angle / 180.0f);
    PCA_SetPulse(ch, us);
}

/* 初始化：返回 1=成功通信 */
uint8_t g_pcaMode1 = 0;

uint8_t PCA_Init(void)
{
    PCA_GPIO_Init();

    PCA_Write8(PCA_MODE1, 0x10);     /* ① 进 sleep（写 PRE_SCALE 必须先睡）*/
    PCA_Write8(PCA_MODE2, 0x04);     /* ② 输出方式：推挽 */
    PCA_Write8(PCA_PRESCALE, 121);   /* ③ 50Hz：25000000/(4096*50)-1 = 121 */
    PCA_Write8(PCA_MODE1, 0xA1);     /* ④ 重启 + 自动递增 + 唤醒 */
    HAL_Delay(3);                    /* 等内部振荡器稳定 */

    for (uint8_t ch = 0; ch < 16; ch++) PCA_SetPulse(ch, 0);   /* 全部先卸力 */

    g_pcaMode1 = PCA_Read8(PCA_MODE1);   /* ⑤ 回读验证 */
    return (g_pcaMode1 == 0xA1 || g_pcaMode1 == 0x21) ? 1 : 0;
}
/* ---------- ⑥ 机械臂：6 关节 ---------- */
#define ARM_CH_NUM     6
#define ARM_MAX_RATE   0.15f      /* 每 ms 最多 0.15° = 150°/s */
#define STICK_DEADZONE 0.08f

/* ★ 测试开关：1 = 6 个舵机持续慢速摆动（没接遥控器时用来验证接线）
              0 = 正常模式（遥控器控制）*/
#define ARM_TEST_SWEEP 0

/* 角度限位（装好后按实物调！现在是保守值）*/
float armLimitMin[ARM_CH_NUM] = { 10, 20, 20, 30,   0,  0 };
float armLimitMax[ARM_CH_NUM] = {170,160,160,150, 180, 70 };
float armAngle[ARM_CH_NUM]    = { 90, 90, 90, 90,  90, 30 };

static float Stick(float v)
{
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0.0f;
    return v;
}

/* 增量式点动：松杆 = 停在原地 */
static void ArmJog(uint8_t ch, float rate)
{
    if (rate >  ARM_MAX_RATE) rate =  ARM_MAX_RATE;
    if (rate < -ARM_MAX_RATE) rate = -ARM_MAX_RATE;

    armAngle[ch] += rate;
    if (armAngle[ch] < armLimitMin[ch]) armAngle[ch] = armLimitMin[ch];
    if (armAngle[ch] > armLimitMax[ch]) armAngle[ch] = armLimitMax[ch];

    
}

/* 每 1ms 调用 */
/* 测试模式：6 个通道各自慢速正弦摆动，相位错开，方便逐个观察 */
static void ArmTestSweep(void)
{
    static uint32_t t = 0;
    t++;
    for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
        float phase = (float)t * 0.0008f + (float)i * 0.7f;
        float mid = (armLimitMin[i] + armLimitMax[i]) * 0.5f;
        float amp = (armLimitMax[i] - armLimitMin[i]) * 0.25f;
        armAngle[i] = mid + amp * sinf(phase);
    }
}

static void ArmControl(void)
{
#if ARM_TEST_SWEEP
    ArmTestSweep();          /* 测试模式：忽略遥控器 */
    return;
#endif
    RC_Info_t &rc = RemoteControl::rcInfo;

    if (rc.sRight == DOWN_POS) return;         /* 急停挡：不动 */

    if (rc.sLeft == MID_POS) {                 /* 臂模式 1 */
        ArmJog(0,  0.15f * Stick(rc.left_rol));   /* 底座  */
        ArmJog(1, -0.08f * Stick(rc.left_col));   /* 大臂  */
        ArmJog(2,  0.10f * Stick(rc.right_col));  /* 小臂  */
        ArmJog(3,  0.12f * Stick(rc.right_rol));  /* 腕俯仰 */
    } else if (rc.sLeft == DOWN_POS) {         /* 臂模式 2 */
        ArmJog(4,  0.15f * Stick(rc.left_rol));   /* 腕旋转 */
        ArmJog(5,  0.06f * Stick(rc.right_col));  /* 夹爪  */
    }
}

/* 已经下发到硬件的角度（"变化才写"用）*/
float armSent[ARM_CH_NUM] = { 90, 90, 90, 90, 90, 30 };

/* 每 1ms 调用：只写 1 个通道，6ms 轮完一圈 */
static void ArmFlushOne(void)
{
    static uint8_t ch = 0;

    if (fabsf(armAngle[ch] - armSent[ch]) > 0.3f) {   /* 变化超过 0.3° 才写 */
        PCA_SetAngle(ch, armAngle[ch]);
        armSent[ch] = armAngle[ch];
    }

    ch++;
    if (ch >= ARM_CH_NUM) ch = 0;
}



/* 急停：全部卸力 */
static void ArmStopAll(void)
{
    for (uint8_t i = 0; i < ARM_CH_NUM; i++) PCA_SetPulse(i, 0);
}

/* 开机自检：不接遥控器也能验证 6 个舵机 */
static void ArmSelfTest(void)
{
    for (uint8_t ch = 0; ch < ARM_CH_NUM; ch++) {
        float base = armAngle[ch];
        float amp  = 12.0f;
        for (float a = base - amp; a <= base + amp; a += 2.0f) {
            PCA_SetAngle(ch, a); HAL_IWDG_Refresh(&hiwdg); HAL_Delay(12);
        }
        for (float a = base + amp; a >= base; a -= 2.0f) {
            PCA_SetAngle(ch, a); HAL_IWDG_Refresh(&hiwdg); HAL_Delay(12);
        }
        PCA_SetAngle(ch, base);
    }
}







void UserStop()
{
    UserMotor.Stop();
    ArmStopAll();              /* 急停：舵机全部卸力 */
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

        /* ---- 机械臂 ---- */
    PIN_Scan();                    /* ★ 扫出哪两个针上真的接了线 */
    PCA_Diag();                    /* ★ 总线自检：先把故障定位出来 */
    uint8_t pcaOk = PCA_Init();

    if (pcaOk) {
        for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
            PCA_SetAngle(i, armAngle[i]);      /* 先到初始姿态 */
            HAL_Delay(5);
        }
        ArmSelfTest();                          /* 自检：不想动就注释掉 */
    }
    /* pcaOk = 0 说明 I2C 没通 → 后面打印里能看到 pcaMode1 不对 */
}



/***
 * 用户自定义任务主循环
 */

/* ---------- 拼行缓冲区：攒够一整行再一次性发 ---------- */
static char g_line[192];

static void AppendF2(const char *name, float v)
{
    int32_t s = (int32_t)(v * 100.0f);
    const char *sign = "";
    if (s < 0) { sign = "-"; s = -s; }      // 负数单独处理，避免丢符号

    uint32_t used = strlen(g_line);
    snprintf(&g_line[used], sizeof(g_line) - used,
             "%s%s%d.%02d ", name, sign, s / 100, s % 100);
}

/* 打印整数，末尾带空格 */
static void AppendI(const char *name, int32_t v)
{
    uint32_t used = strlen(g_line);
    snprintf(&g_line[used], sizeof(g_line) - used, "%s%ld ", name, (long)v);
}

/* 打印字符串（引脚扫描结果），前面自动加 pins= */
static void AppendS(const char *s)
{
    uint32_t used = strlen(g_line);
    snprintf(&g_line[used], sizeof(g_line) - used, "pins=%s ", s);
}

void UserHandle()
{
    static uint32_t cnt = 0;

    IMU_Update();                    // 1kHz 采样
    ArmControl();                    // ★ 加这一行：机械臂控制 + 限速
    ArmFlushOne();                   // ★ 只写 1 个通道（~430 µs）

    if (++cnt >= 100) {
        cnt = 0;
        g_line[0] = '\0';
        /* ---- 机械臂状态 ---- */
        AppendI("mode=", (int32_t)RemoteControl::rcInfo.sLeft);
        AppendI("rl=",   (int32_t)g_rcLen);              /* 遥控帧长：0=没收到 25=SBUS 18=DBUS */
        AppendI("pca=",  (int32_t)g_pcaMode1);          /* ← 加这行 */
        AppendI("bS=",   (int32_t)g_busScl);            /* 正常 11 */
        AppendI("bD=",   (int32_t)g_busSda);            /* 正常 11 */
        AppendI("scan=", (int32_t)g_scanAddr);          /* 正常 64 */
        AppendI("pS=",   (int32_t)g_pdScl);             /* 正常 1 */
        AppendI("pD=",   (int32_t)g_pdSda);             /* 正常 1 */
        AppendI("sc2=",  (int32_t)g_scanSlow);          /* 慢扫，正常 64 */
        AppendS(g_pinStr);                              /* ★ 扫到的针，如 B13,F1 */
        AppendI("a0=",   (int32_t)armAngle[0]);
        AppendI("sent0=",(int32_t)armSent[0]);          /* ← 看下发有没有跟上 */
        AppendI("off0=", (int32_t)PCA_ReadOff(0));      /* 回读 CH0 的 OFF 计数，应在 100~500 */
        AppendI("m1=",   (int32_t)PCA_Read8(PCA_MODE1));/* 应 33(0x21)：SLEEP=0 说明芯片醒着 */
        // ... 你原来的打印 ...
        UsbPrintf("%s\r\n", g_line);
    }

    UserMotor.Handle();
}