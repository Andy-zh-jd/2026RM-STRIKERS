//
// Created by LEGION on 2021/10/4.
//
#include "UserTask.h"
#include <stdio.h>
#include <stdarg.h>
#include <math.h>              /* fabsf/sinf：机械臂限速与测试摆动 */
#include <string.h>            /* strncat/strncpy：PIN_Scan 结果拼字符串 */
#include "RemoteControl.h"          // rcInfo（遥控器）
#include "ChassisTask.h"            // 2026-10-23：wheelTargetRpm/底盘接口（打印四轮目标用）
#include "tim.h"                    // TIM1: 4x BTS7960 PWM
extern volatile uint8_t key_raw_state;
// 2026-10-17：vccBat 没写在任何头文件里，要自己 extern（Device.cpp:117）
// 用途：接电池后先看这个数对不对（换算系数 /22*222 疑似笔误，要用万用表核对）
extern volatile float vccBat;
// 2026-10-20：底盘四个电机对象定义在 ChassisTask.cpp，这里只是引用（CMFL = ID1 左前轮）
extern Motor CMFL, CMFR, CMBL, CMBR;
/* 2026-10-23 诊断计数（定义在 Motor.cpp 的 CAN 接收中断里） */
extern volatile uint32_t g_rx203, g_rxOther;

/* ===== 固件版本标识（2026-11-20） =====
 * 为什么：车上固件可能和仓库不一致（队友本地改过、没提交/没推送）。
 * 规则：**每次烧录前**更新下面的 TAG，并把 git commit 记到 03_docs/实测记录.md。
 * 读取：tools/backup_firmware.py 把板上固件读回来，会自动扫出 "RM2027_C ..." 版本串；
 *       也可以按 ELF 符号 g_fwVersion 直接用 SWD 读字符串。 */
#define FW_VERSION_TAG "2026-11-20 CAN-fix + strafe-deadzone"
extern "C" {
__attribute__((used)) volatile const char g_fwVersion[] =
    "RM2027_C " FW_VERSION_TAG " (" __DATE__ " " __TIME__ ")";
}
/* 只用来引用版本串首字节，防止 -Wl,--gc-sections 把上面这段只读数据整个丢掉 */
volatile char g_fwVersionMark;

/* ===== 调试开关 =====
 * DBG_MINIMAL = 1 → 只打印，不读 IMU（跳过 SPI）、不碰机构。
 *   用来隔离「USB 能枚举但 COM 口打不开」这类问题：
 *   最小模式下 USB 正常 → 说明是某个阻塞外设（最可能是 SPI 读 IMU 超时）拖死了 1ms 中断。
 * DBG_UART = 1 → 同一行数据同时从 UART1（PA9=TX，115200）发出去，USB 挂了也能用 CH340 看。
 *   ⚠️ HAL_UART_Transmit 是阻塞的（60 字节≈5ms），塞进 1ms 中断会拖垮 USB 控制传输，默认关。 */
/* 2026-11-16：**打开 IMU**（底盘高速跑偏修正需要陀螺仪）。
 * 为什么现在敢开：10-23 已定位到当年的 USB 问题是「在 1ms 中断里发 USB」，
 *   后来改成"中断拼字符串、主循环发送"，并且 IMU 的 SPI 读也放在主循环；
 *   SysTick 优先级已提到 0（HAL 超时在中断里有效）。
 * 配套：spi.c 的 SPI1 分频已从 256 改成 32（328 kHz → 2.6 MHz），单次读 ~50 µs。
 * 如果上电后 COM 口又打不开：先把 DBG_MINIMAL 改回 1（会退回不读 IMU 的旧行为）。 */
#define DBG_MINIMAL 0/* 1 = IMU 只读一次 chip id 然后彻底不读（体检用）*/
#define DBG_IMU_IDONLY 0
/* IMU 周期读取的分频：主循环里执行，每 IMU_DIV 毫秒读一次。
 * 2026-11-16：从 5（200 Hz）改成 1（1 kHz）——航向锁要的是"最新角速度"，
 * 200 Hz 的 5 ms 保持 + 5 ms 传输会带来最多 ~10 ms 的相位滞后。
 * 主循环空闲（while(1) 只做 UserPollSlow），2.6 MHz SPI 读一次约 50 µs，占用 ~5%。 */
#define IMU_DIV 1
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

/* 2026-11-16：最近一次**成功**读 IMU 的时刻（HAL_GetTick 单位）。
 * 给 IMU_Ready() 判"数据是不是新鲜的"：主循环被卡住/传感器死掉时，
 * 底盘航向锁必须立刻退出，而不是拿着几秒前的角速度继续算。 */
volatile uint32_t g_imuLastTick = 0;

/* 2026-11-17：IMU 读次数定义在后面 UserPollSlow() 附近；这里提前声明给"静止零偏学习"用 */
extern uint32_t g_imuReadCount;

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

    /* ===== 2026-11-17：静止零偏自动学习 =====
     * 为什么：上电标定只能消掉"那一刻"的零偏；温漂/残余零偏（约 0.1 °/s）在长跑里会积成几度航向。
     * 三个条件都满足才学（防止把真实转动学进零偏）：
     *   1) 车确实静止：ChassisIsStationary() = 四轮无目标、无转速，连续 1.5 s
     *   2) 三轴滤波后角速度都很小（|°/s| < 1）—— 排除"车停着但被人慢慢转"
     *   3) 已经过 100 次周期读 —— 避开启动瞬间
     * 学习率：1 kHz 下 τ≈1.2 s；单次误差限幅 ±30 LSB（±1.8 °/s），不会跳变。 */
    if (g_imuReadCount > 100u && ChassisIsStationary()) {
        uint8_t quiet = 1;
        for (int i = 0; i < 3; i++) {
            if (g_gyroLpf[i] > 1.0f || g_gyroLpf[i] < -1.0f) { quiet = 0; break; }
        }
        if (quiet) {
            for (int i = 0; i < 3; i++) {
                float diff = (float)g_gyro[i] - g_gyroBias[i];
                if (diff >  30.0f) diff =  30.0f;
                if (diff < -30.0f) diff = -30.0f;
                g_gyroBias[i] += diff * 0.0008f;
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        g_gyroDps[i] = ((float)g_gyro[i] - g_gyroBias[i]) / GYRO_SEN;      // 去零偏
        g_gyroLpf[i] = g_gyroLpf[i] * GYRO_LPF
                     + g_gyroDps[i] * (1.0f - GYRO_LPF);                    // 低通
        g_accelG[i]  = (float)g_accel[i] / ACC_SEN;
    }

    /* 2026-11-16：刷新"IMU 数据新鲜度"时间戳（IMU_Ready 用） */
    g_imuLastTick = HAL_GetTick();
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

/* ===== 2026-11-13：舵机结构换向表 =====
 *   +1 = 逻辑角与物理角同向
 *   -1 = 反向：在**输出端**镜像（物理角 = 180 - 逻辑角）
 *
 * 为什么放在这里而不是改摇杆符号：
 *   所有角度下发都经过 PCA_SetAngle()（初始姿态 / 自愈恢复 / 点动刷新 / 自检），
 *   在这里镜像一次，全链路统一；armAngle[] 仍是"逻辑角"，臂控逻辑和限位都不用动。
 *
 * 以后任何舵机因结构改动换向，只改这一行对应的数。 */
static const float ARM_DIR[16] = {
    -1.0f,  /* ch0 底座：结构前后反转（本次换向）*/
    +1.0f,  /* ch1 大臂 */
    +1.0f,  /* ch2 小臂 */
    +1.0f,  /* ch3 腕俯仰 */
    +1.0f,  /* ch4 腕旋转 */
    +1.0f,  /* ch5 夹爪 */
    +1.0f, +1.0f, +1.0f, +1.0f, +1.0f, +1.0f, +1.0f, +1.0f, +1.0f, +1.0f
};

/* 设角度 0~180°（ARM_DIR = -1 的通道在输出端镜像） */
void PCA_SetAngle(uint8_t ch, float angle)
{
    if (ch >= 16) return;

    /* 2026-11-20：允许**负角**（机械行程起点可能落在 500µs 之外）：
     *   us = 500 + 2000 × angle / 180   →  angle=-18 时 us = 300µs
     * 逻辑角先钳到 -45~225（镜像后可能变成 181~225），再做 ARM_DIR 镜像。*/
    if (angle < -45.0f) angle = -45.0f;
    if (angle > 225.0f) angle = 225.0f;

    if (ARM_DIR[ch] < 0.0f) angle = 180.0f - angle;   /* 反向通道：物理角镜像 */

    float usf = SERVO_US_MIN + (SERVO_US_MAX - SERVO_US_MIN) * angle / 180.0f;

    /* ⚠ 脉冲硬护栏：PCA_SetPulse() 里 us==0 表示**卸力**！
     *   负角算出 0 或负数时强转 uint16_t 会变成 6 万多 → 卸力/满占空把舵机顶死。*/
    if (usf < 300.0f)  usf = 300.0f;      /* 300µs = -18° */
    if (usf > 2600.0f) usf = 2600.0f;     /* 2600µs = 189° */

    PCA_SetPulse(ch, (uint16_t)usf);
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

/* ========================================================================
 * BTS7960 四推杆（24V 全压）
 *
 * 只使用两个开关：
 *   SA = optional[0] : 低=正常底盘；高=爬台阶
 *   SD = optional[1] : 爬台阶模式下，每翻一次进入下一步
 *
 * 步骤：
 *   IDLE -> 前伸 -> 前保持(可开车) -> 前缩
 *        -> 后伸 -> 后保持(可开车) -> 后缩 -> DONE
 *
 * 前 BTS: PE9/PE11 (TIM1_CH1/CH2)
 * 后 BTS: PE13/PE14 (TIM1_CH3/CH4)
 * 限位: PC6 前、PI6 后（常闭串联到 GND，可选）
 * ======================================================================== */
#define JACK_PWM_ARR          8399U          /* 20 kHz */
#define JACK_PWM_DUTY_MAX     JACK_PWM_ARR   /* 24V 推杆：100% */
#define JACK_PWM_RAMP_STEP    20U
#define JACK_SD_DEBOUNCE_MS   30U

/* 没装外部限位时保持 0；装好常闭限位后改 1。 */
#define JACK_LIMIT_ENABLED    0
#define JACK_LIMIT_ACTIVE     GPIO_PIN_SET

typedef enum {
    JACK_OFF = 0,
    JACK_IDLE,
    JACK_FRONT_OUT,
    JACK_FRONT_HOLD,
    JACK_FRONT_IN,
    JACK_REAR_OUT,
    JACK_REAR_HOLD,
    JACK_REAR_IN,
    JACK_DONE
} JackState;

typedef enum {
    JACK_STOP = 0,
    JACK_OUT,
    JACK_IN
} JackCmd;

typedef struct {
    TIM_HandleTypeDef* htim;
    uint32_t rCh;
    uint32_t lCh;
    GPIO_TypeDef* limPort;
    uint16_t limPin;
    uint8_t  lastCmd;
    uint16_t deadMs;
    uint16_t duty;
} JackPair;

static JackPair jackFront = {
    &htim1, TIM_CHANNEL_1, TIM_CHANNEL_2,
    GPIOC, GPIO_PIN_6, JACK_STOP, 0, 0
};

static JackPair jackRear = {
    &htim1, TIM_CHANNEL_3, TIM_CHANNEL_4,
    GPIOI, GPIO_PIN_6, JACK_STOP, 0, 0
};

static JackState jackState = JACK_OFF;
static uint8_t  jackSaPrev = 0;
static uint8_t  jackSdRawPrev = 0;
static uint8_t  jackSdStable = 0;
static uint32_t jackSdChangeMs = 0;
static uint8_t  jackEStop = 0;   /* UserStop() 置位；SA 回低后清除 */

static void JackPwmPins_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOE, &gpio);
}

static void JackPwm_Init(void)
{
    htim1.Init.Prescaler = 0;
    htim1.Init.Period = JACK_PWM_ARR;
    HAL_TIM_PWM_Init(&htim1);

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, 0);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_4, 0);

    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4);
}

static void JackLimit_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_Init(GPIOI, &gpio);
}

static void JackSetDuty(JackPair* p, uint16_t r, uint16_t l)
{
    __HAL_TIM_SET_COMPARE(p->htim, p->rCh, r);
    __HAL_TIM_SET_COMPARE(p->htim, p->lCh, l);
}

static void JackStop(JackPair* p)
{
    p->duty = 0;
    p->lastCmd = JACK_STOP;
    JackSetDuty(p, 0, 0);
}

static void JackStopAll(void)
{
    JackStop(&jackFront);
    JackStop(&jackRear);
}

static uint8_t JackLimitHit(JackPair* p)
{
#if JACK_LIMIT_ENABLED
    return HAL_GPIO_ReadPin(p->limPort, p->limPin) == JACK_LIMIT_ACTIVE;
#else
    (void)p;
    return 0;
#endif
}

static void JackPairOut(JackPair* p)
{
    if (p->duty < JACK_PWM_DUTY_MAX) {
        p->duty += JACK_PWM_RAMP_STEP;
        if (p->duty > JACK_PWM_DUTY_MAX) p->duty = JACK_PWM_DUTY_MAX;
    }
    JackSetDuty(p, p->duty, 0);
}

static void JackPairIn(JackPair* p)
{
    if (p->duty < JACK_PWM_DUTY_MAX) {
        p->duty += JACK_PWM_RAMP_STEP;
        if (p->duty > JACK_PWM_DUTY_MAX) p->duty = JACK_PWM_DUTY_MAX;
    }
    JackSetDuty(p, 0, p->duty);
}

static void JackPairUpdate(JackPair* p, JackCmd cmd)
{
    if (cmd != p->lastCmd) {
        JackStop(p);
        p->deadMs = 200;
        p->lastCmd = cmd;
    }
    if (p->deadMs) {
        p->deadMs--;
        return;
    }

    if (cmd == JACK_OUT && !JackLimitHit(p)) {
        JackPairOut(p);
    } else if (cmd == JACK_IN) {
        JackPairIn(p);
    } else {
        JackStop(p);
    }
}

bool JackActive(void)
{
    return RemoteControl::rcInfo.optional[0] > 1000;
}

bool JackChassisAllowed(void)
{
    return (jackState == JACK_FRONT_HOLD) ||
           (jackState == JACK_REAR_HOLD)  ||
           (jackState == JACK_DONE);
}

static void JackAdvance(void)
{
    switch (jackState) {
        case JACK_IDLE:       jackState = JACK_FRONT_OUT;  break;
        case JACK_FRONT_OUT:  jackState = JACK_FRONT_HOLD; break;
        case JACK_FRONT_HOLD: jackState = JACK_FRONT_IN;   break;
        case JACK_FRONT_IN:   jackState = JACK_REAR_OUT;   break;
        case JACK_REAR_OUT:   jackState = JACK_REAR_HOLD;  break;
        case JACK_REAR_HOLD:  jackState = JACK_REAR_IN;    break;
        case JACK_REAR_IN:    jackState = JACK_DONE;       break;
        case JACK_DONE:
        case JACK_OFF:
        default:              jackState = JACK_IDLE;       break;
    }
    /* 每次切步都先停，再让下一步重新软启动。 */
    JackStopAll();
}

static void JackApply(void)
{
    switch (jackState) {
        case JACK_FRONT_OUT:
            JackPairUpdate(&jackFront, JACK_OUT);
            JackStop(&jackRear);
            break;
        case JACK_FRONT_IN:
            JackPairUpdate(&jackFront, JACK_IN);
            JackStop(&jackRear);
            break;
        case JACK_REAR_OUT:
            JackPairUpdate(&jackRear, JACK_OUT);
            JackStop(&jackFront);
            break;
        case JACK_REAR_IN:
            JackPairUpdate(&jackRear, JACK_IN);
            JackStop(&jackFront);
            break;
        case JACK_FRONT_HOLD:
        case JACK_REAR_HOLD:
        case JACK_DONE:
        case JACK_OFF:
        case JACK_IDLE:
        default:
            JackStopAll();
            break;
    }
}

static void JackLimitCheck(void)
{
    if (jackState == JACK_FRONT_OUT && JackLimitHit(&jackFront)) {
        jackState = JACK_FRONT_HOLD;
        JackStopAll();
    } else if (jackState == JACK_REAR_OUT && JackLimitHit(&jackRear)) {
        jackState = JACK_REAR_HOLD;
        JackStopAll();
    }
}

void JackTask(void)
{
    const uint32_t now = HAL_GetTick();
    const uint8_t sa = JackActive() ? 1U : 0U;
    const uint8_t sd = (RemoteControl::rcInfo.optional[1] > 1000) ? 1U : 0U;

    /* SA 低：退出爬台阶，推杆全停；SD 边沿忽略。 */
    if (!sa) {
        jackEStop = 0;              /* 必须回到 SA 低才解除急停锁存 */
        if (jackState != JACK_OFF) {
            jackState = JACK_OFF;
            JackStopAll();
        }
        jackSaPrev = 0;
        jackSdRawPrev = sd;
        jackSdStable = sd;
        jackSdChangeMs = now;
        return;
    }

    /* 急停锁存：SC 下档或遥控丢失时，UserStop() 置位。 */
    if (jackEStop) {
        if (jackState != JACK_OFF) {
            jackState = JACK_OFF;
            JackStopAll();
        }
        return;
    }

    /* SA 上升沿：进入 IDLE，等待第一次 SD 翻转。 */
    if (!jackSaPrev) {
        jackSaPrev = 1;
        jackState = JACK_IDLE;
        JackStopAll();
        jackSdRawPrev = sd;
        jackSdStable = sd;
        jackSdChangeMs = now;
        return;
    }

    /* SD 消抖后的边沿：翻一次走一步。 */
    if (sd != jackSdRawPrev) {
        jackSdRawPrev = sd;
        jackSdChangeMs = now;
    } else if (sd != jackSdStable && (now - jackSdChangeMs) >= JACK_SD_DEBOUNCE_MS) {
        jackSdStable = sd;
        JackAdvance();
    }

    JackLimitCheck();
    JackApply();
}

/* ---------- ⑥ 机械臂：6 关节 ---------- */
#define ARM_CH_NUM     6
#define ARM_MAX_RATE   0.15f      /* 每 ms 最多 0.15° = 150°/s（硬上限）*/

/* ===== 通道 ↔ 关节 对应表（2026-11-20 实车核对）=====
 * 实测这颗车上 **CH4 是夹爪、CH5 是腕旋转**（和下面旧注释相反）。
 * 所以把 ArmControl / armRate / 限位 里的 4、5 两列都按这张表对调了。
 * 如果接线又换回来，只改这两个宏 + 把 armRate/限位/初值 的 4、5 列换回去。*/
#define CH_WRISTROT   5      /* 腕旋转 */
#define CH_GRIP       4      /* 夹爪   */
#define STICK_DEADZONE 0.08f

/* ===== 2026-11-14：每关节点动速率表（°/ms，满杆）=====
 * 为什么需要：舵机扭矩固定，**动得越快、启动冲击越大**，齿轮越容易扫。
 * 大臂 ch1 在「夹着矿石抬升」时扭矩不足、空转发烫（齿轮打滑）→ 降到 0.04（原 0.08）。
 * ⚠️ 降速只能减小冲击和发热，**不能增加扭矩**；根本解决还要靠减力臂 / 加弹簧 / 换金属齿舵机。*/
static const float armRate[ARM_CH_NUM] = {
    0.15f,  /* ch0 底座   */
    0.04f,  /* ch1 大臂   ← 夹矿抬升会打滑，减半 */
    0.10f,  /* ch2 小臂   */
    0.12f,  /* ch3 腕俯仰 */
    0.06f,  /* ch4 夹爪   （对调后，原 ch5 的值）*/
    0.15f   /* ch5 腕旋转 （对调后，原 ch4 的值）*/
};

/* ★ 测试开关：1 = 6 个舵机持续慢速摆动（没接遥控器时用来验证接线）
              0 = 正常模式（遥控器控制）*/
#define ARM_TEST_SWEEP 0

/* ★ 开机自检：1 = 上电时 6 个舵机各摆 ±12 度（台架验证接线用）
 *              0 = 不摆（上车/比赛用：否则每次上电机械臂都会动，且阻塞约 1.7 s）*/
#define ARM_SELFTEST_ON_BOOT  0

/* 角度限位（装好后按实物调！现在是保守值）
 * 2026-11-14 实测：底座 ch0 的软件限位 10/170 正好等于机械行程两端 →
 * 推到端部时舵机一直顶住结构（嗡嗡 + 抖动）。两端各收 10° 留余量。 */
float armLimitMin[ARM_CH_NUM] = { 20, 20, 20, 30,   0,   0 };
float armLimitMax[ARM_CH_NUM] = {160,160,160,150,  70, 180 };   /* [4]=夹爪70 [5]=腕旋转180（对调）*/
float armAngle[ARM_CH_NUM]    = { 90, 90, 90, 90,  30, 90 };    /* [4]夹爪30张开 [5]腕旋转90 */

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

/* ===== SC 上档 = 机械臂收纳姿态（2026-11-20 从台架那套移植）=====
 * SC 拨到**上档**时，六路舵机自动走到下面这组角度并锁住（忽略摇杆），
 * 方便"机械臂收好、只开车"（SB 上档驾驶）。SC 回中/下档恢复正常操作。
 *
 * ★ 想改姿态：只改 armParkAngle[6] 这 6 个数（顺序 = 通道号，见 CH_* 表）：
 *      [0] 底座  [1] 大臂  [2] 小臂  [3] 腕俯仰  [CH_GRIP] 夹爪  [CH_WRISTROT] 腕旋转
 *   必须落在 armLimitMin/Max 之间，否则会被夹回限位（会抽一下）。
 * ★ 想调归位速度：ARM_PARK_RATE（°/ms，0.06 = 60°/s）。
 * ★ 想关掉：ARM_PARK_ON_SC_UP 改 0。*/
#define ARM_PARK_ON_SC_UP   1
#define ARM_PARK_RATE       0.06f        /* 归位速度 60°/s */

static const float armParkAngle[ARM_CH_NUM] = {
    /* CH0  CH1  CH2  CH3   CH4(夹爪) CH5(腕旋转) */
        90,  90,  90,  90,   30,       90
};

/* 往目标姿态限速逼近（不是瞬间跳过去 —— 那样电流冲击大、结构也会"哐"一下）*/
static void ArmMoveToward(const float *tgt, float degPerMs)
{
    for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
        float d = tgt[i] - armAngle[i];
        if (d >  degPerMs) d =  degPerMs;
        if (d < -degPerMs) d = -degPerMs;
        ArmJog(i, d);            /* ArmJog 内部会夹到 armLimitMin/Max */
    }
}

#if ARM_TEST_SWEEP
/* 测试模式：6 个通道各自慢速正弦摆动，相位错开，方便逐个观察。
 * 包在 #if 里是为了默认配置下不编译它 —— 否则 sinf() 会引入 libm 依赖，
 * GCC/CMake 那条链没有链接 -lm。 */
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
#endif /* ARM_TEST_SWEEP */

static void ArmControl(void)
{
#if ARM_TEST_SWEEP
    ArmTestSweep();          /* 测试模式：忽略遥控器 */
    return;
#endif
    RC_Info_t &rc = RemoteControl::rcInfo;

    if (rc.sRight == DOWN_POS) return;         /* 急停挡：不动 */

#if ARM_PARK_ON_SC_UP
    /* SC 上档 = 机械臂收到固定姿态并锁住（忽略摇杆），专心开底盘。
     * 放在 JackActive 判断**之前**：爬台阶时也希望手臂先收好。*/
    if (rc.sRight == UP_POS) {
        ArmMoveToward(armParkAngle, ARM_PARK_RATE);
        return;
    }
#endif

    if (JackActive()) return;                   /* 爬台阶时机械臂不要动 */

    if (rc.sLeft == MID_POS) {                 /* 臂模式 1 */
        ArmJog(0,  armRate[0] * Stick(rc.left_rol));    /* 底座（左摇杆横向）*/
        ArmJog(1, -armRate[1] * Stick(rc.left_col));    /* 大臂（左摇杆纵向）*/
        ArmJog(2,  armRate[2] * Stick(rc.right_col));   /* 小臂（右摇杆纵向）*/
        ArmJog(3,  armRate[3] * Stick(rc.right_rol));   /* 腕俯仰（右摇杆横向）*/
    } else if (rc.sLeft == DOWN_POS) {         /* 臂模式 2 */
        ArmJog(CH_WRISTROT, armRate[CH_WRISTROT] * Stick(rc.left_rol));   /* 腕旋转（CH5）*/
        ArmJog(CH_GRIP,     armRate[CH_GRIP]     * Stick(rc.right_col));  /* 夹爪  （CH4）*/
    }
}

/* 已经下发到硬件的角度（"变化才写"用）*/
float armSent[ARM_CH_NUM] = { 90, 90, 90, 90, 30, 90 };   /* [4]夹爪30 [5]腕旋转90（对调）*/

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



/* ---------- PCA 自愈：芯片被复位（SLEEP=1）就自动重新初始化 ---------- */
uint8_t g_pcaRecover = 0;

static void PCA_HealthCheck(void)
{
    uint8_t m1 = PCA_Read8(PCA_MODE1);

    if (m1 & 0x10) {                      /* bit4 = SLEEP，说明芯片复位了 */
        PCA_Init();                       /* 重新配置 PRE_SCALE / MODE1 */
        for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
            PCA_SetAngle(i, armAngle[i]); /* 把所有通道恢复到当前角度 */
            armSent[i] = -999.0f;         /* 强制下一步重新下发 */
        }
        g_pcaRecover++;                   /* 记录恢复次数，便于判断电源是否在抖 */
    }
}

/* 急停：全部卸力。
 * ⚠ 2026-11-14 修的 bug：这里只把脉冲写 0，却没有让 armSent[] 失效。
 *   于是急停解除后，ArmFlushOne() 只在"目标角变化 >0.3°"时才重发 → 没被推动过的
 *   关节永远不会重新发脉冲 → 该关节一直是"软"的（真实踩到：一个关节死活没力）。
 *   修法：卸力时把 armSent 置 -999，恢复后所有通道强制重发一次。 */
static void ArmStopAll(void)
{
    for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
        PCA_SetPulse(i, 0);
        armSent[i] = -999.0f;          /* 失效 → 下次 ArmFlushOne 必然重发 */
    }
}

#if ARM_SELFTEST_ON_BOOT
/* 开机自检：不接遥控器也能验证 6 个舵机（默认不编译，见 ARM_SELFTEST_ON_BOOT）*/
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
#endif /* ARM_SELFTEST_ON_BOOT */
/***
 * 在这里放入xxx.stop()即可使舵机,电机在遥控器急停挡位断电
 */
void UserStop()
{
    UserMotor.Stop();
    JackStopAll();

    /* 正常底盘（SA 低）也会每 1ms 调 UserStop，不能在那里锁存急停；
     * 只有爬台阶模式下的 UserStop（SC 下档/遥控丢失）才锁存。 */
    if (JackActive()) {
        jackEStop = 1;
    }
}

/***
 * 在这里写入初始化内容
 */
void UserInit()
{
    /* 引用一次固件版本串首字节（volatile 读）→ 链接器 --gc-sections 不会把版本串丢掉。
     * 版本串用于 tools/backup_firmware.py 从板上固件识别"这是哪一版"。 */
    g_fwVersionMark = g_fwVersion[0];

    /* ---- 机械臂 PCA9685 ----
     * 顺序有讲究：PIN_Scan() 会把 PE9/11/13/14、PC6、PI6 临时改成输入来探测，
     * 所以机械臂初始化（含引脚扫描）必须排在 JackPwmPins_Init() 之前，
     * 否则推杆的 TIM1 PWM 复用功能会被扫描覆盖掉。 */
    PIN_Scan();                     /* 扫出哪两个针上真的接了外部上拉 */
    PCA_Diag();                     /* 总线自检：通信不通时先看 C 页的 bS/bD/pS/pD */
    {
        uint8_t pcaOk = PCA_Init();
        if (pcaOk) {
            for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
                PCA_SetAngle(i, armAngle[i]);   /* 先到初始姿态 */
                HAL_Delay(5);
            }
#if ARM_SELFTEST_ON_BOOT
            ArmSelfTest();
#endif
        }
        /* pcaOk = 0 说明 I2C 没通 → C 页 pca= 不会是 33 */
    }

    /* BTS7960 推杆初始化：必须放在 DBG_IMU_IDONLY 的 return 之前。 */
    JackPwmPins_Init();
    JackPwm_Init();
    JackLimit_Init();
    JackStopAll();

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

    /* ---- 2b. 陀螺仪配置（2026-11-17 新增）----
     * 原来只配了加速度计，陀螺一直在用复位默认值：2000 Hz ODR / 532 Hz 滤波。
     * 带宽太大 → 静止时原始噪声大（实测 ±8 °/s 级尖峰）、软件滤波后仍有 ±1.5 °/s 抖动、
     * 开机标定残余零偏约 0.1 °/s（15 s 航向漂 1.7°）。
     * 降到 **400 Hz ODR / 116 Hz 数字低通**后，噪声约降一半；对 1 kHz 读取没有影响
     * （同一采样会被读 2~3 次，软件 17 Hz 低通照常工作）。
     * 量程 ±2000 °/s（与 GYRO_SEN=16.384 LSB/°/s 对应），显式写一遍防止被意外改掉。 */
    IMU_WriteReg(0, 0x0F, 0x00);   // GYRO_RANGE: ±2000 °/s
    IMU_WriteReg(0, 0x11, 0x00);   // GYRO_LPM1: normal mode（默认就是，显式写防意外）
    IMU_WriteReg(0, 0x10, 0x02);   // GYRO_BANDWIDTH: 0x02 = 400 Hz ODR / 116 Hz 低通
    HAL_Delay(50);                 // 等新配置生效

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

/* 2026-11-16：给底盘航向锁用的 IMU 健康判断（全部条件都为真才算可用）：
 *   1) 芯片 ID 对（加速度计 0x1E / 陀螺仪 0x0F）—— 防止根本没接/没初始化
 *   2) SPI 没有连续失败（g_spiErr 到 IMU_MAX_FAILS 就判死，见 IMU_Update）
 *   3) 50 ms 内有新数据 —— 主循环被卡住时陀螺数据会变"旧"，此时宁可不用
 * 任意一条不满足 → 底盘自动退回"无航向锁"的旧行为，绝不用坏数据控车。 */
uint8_t IMU_Ready(void)
{
    if (g_gyroId != 0x0F || g_accelId != 0x1E) return 0;
    if (g_spiErr >= IMU_MAX_FAILS) return 0;
    if ((uint32_t)(HAL_GetTick() - g_imuLastTick) > 50u) return 0;
    return 1;
}

/* SWD 快照区：24 个 int32，见 UserHandle 里的赋值说明。
 * 2026-11-16：从 16 扩到 24，给底盘运动控制（航向锁/归一化）加了 8 个观察点，
 * 用 tools/read_swd.py 直接读 RAM，绕开 USB。 */
volatile int32_t g_dbg[24];

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


/* 字符串字段：引脚扫描结果，前面自动加 pins= */
static void AppendS(const char *s)
{
    uint32_t used = strlen(g_line);
    snprintf(&g_line[used], sizeof(g_line) - used, "pins=%s ", s);
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
    static uint8_t  page = 0;        // A/B/C 三页轮播，避免一行太长被截断
    static uint8_t  armEStop = 0;    // 机械臂急停锁存：只在跳变时卸一次力

    /* IMU 降频到 200Hz（每 5ms 读一次）：1ms 中断里做 SPI 太挤，且 200Hz 对姿态足够 */
    static uint32_t imuDiv = 0;
    if (!DBG_MINIMAL) { if (++imuDiv >= IMU_DIV) { imuDiv = 0; imuReadReq = 1; } }

    /* ---- 机械臂 ----
     * 急停判定必须放在这里，不能塞进 UserStop()：
     * CtrlHandle() 在"正常驾驶"分支里每 1 ms 都调 UserStop()，
     * 若 UserStop() 里写 ArmStopAll()，机械臂会被持续卸力、永远动不了。
     * 这里改用跳变锁存，只在进入急停的那一刻写一次 6 个通道。 */
    {
        uint8_t estop;
#if ARM_TEST_SWEEP
        estop = 0;                       /* 台架摆动测试：忽略急停，否则没接遥控器就不动 */
#else
        estop = (RemoteControl::rcInfo.sRight == DOWN_POS) || !RemoteAlive();
#endif
        if (estop) {
            if (!armEStop) { armEStop = 1; ArmStopAll(); }
        } else {
            armEStop = 0;
            ArmControl();       // 先按摇杆算目标角（含限速 / 限位）
            ArmFlushOne();      // 每 1 ms 只写 1 个通道，6 ms 轮完一圈
        }
    }

    JackTask();                     // BTS7960 推杆：SA 总开关 + SD 步进

    if (++cnt >= 100) {              // 每 100 ms 打一行，A/B/C 轮播，各约 3.3 Hz
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

        /* ===== 2026-11-16：底盘运动控制观察点（给 read_swd.py / 调参用）=====
         * [16] 陀螺积分航向 °×100      [17] 航向锁误差 °×100
         * [18] 饱和归一化比例 ×1000    [19] 实际下发 ω rad/s×1000
         * [20] 航向环修正 rad/s×1000   [21] 实际下发 vx m/s×100
         * [22] 实际下发 vy m/s×100     [23] IMU_Ready()（1=可用） */
        g_dbg[16] = (int32_t)(chassisYawDeg * 100.0f);
        g_dbg[17] = (int32_t)(ChassisYawErrDeg() * 100.0f);
        g_dbg[18] = (int32_t)(chassisDesatScale * 1000.0f);
        g_dbg[19] = (int32_t)(chassisOmegaEff * 1000.0f);
        g_dbg[20] = (int32_t)(chassisYawCorr * 1000.0f);
        g_dbg[21] = (int32_t)(chassisDbgVx * 100.0f);
        g_dbg[22] = (int32_t)(chassisDbgVy * 100.0f);
        g_dbg[23] = (int32_t)chassisImuOk;

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
            /* 2026-11-16：IMU 航向锁自检用。手转车体 90°：
             *   yaw 应该跟着变约 90；gx/gy/gz 里变化最大的那个轴就是 yaw 轴。
             *   im=1 表示 IMU 数据健康（底盘航向锁才会真正参与控制）。 */
            AppendF2("yaw=", chassisYawDeg);
            AppendI("im=", (int)chassisImuOk);
            /* 2026-10-23：四路电调的**转子角度**（0~8191，C620 反馈帧原始值）。
             * 作用：区分"没收到帧"和"收到帧但电机不转"——
             *   a_n 一直是 0        → 这个 ID 没有反馈帧（CAN/供电/ID 问题）
             *   a_n 是 0~8191 的数   → 帧在正常到达（链路通，只是电机没转）
             * 手转电机轴时 a_n 明显变化 = 最快的"链路通不通"判据。 */
            AppendI("a1=", (int)(uint16_t)CMFL.feedback.angle);
            AppendI("a2=", (int)(uint16_t)CMFR.feedback.angle);
            AppendI("a3=", (int)(uint16_t)CMBL.feedback.angle);
            AppendI("a4=", (int)(uint16_t)CMBR.feedback.angle);
        } else if (page == 1) {
            /* ===== B 行：机构电机(M2006) 详情 + 四个底盘轮子 + 档位 ===== */
            AppendF2("m=",   UserMotor.state.speed);  // M2006 输出轴 rpm（反馈）
            AppendF2("tgt=", UserMotor.targetSpeed);  // M2006 目标 rpm
            AppendI("cur=",  (int)UserMotor.state.moment);
            AppendI("st=",   (int)UserMotor.stopFlag);
            /* 2026-10-23：回读也乘方向补偿，和 t 在同一个（车体）坐标系里，
             * 这样"t 与 r 同号"才等价于"轮子朝期望方向转"。右侧两个轮子是镜像安装的。 */
            AppendF2("r1=",  CMFL.state.speed * SIGN_WHEEL_FL);
            AppendF2("r2=",  CMFR.state.speed * SIGN_WHEEL_FR);
            AppendF2("r3=",  CMBL.state.speed * SIGN_WHEEL_BL);
            AppendF2("r4=",  CMBR.state.speed * SIGN_WHEEL_BR);
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
        } else {
            /* ===== C 页：机械臂 6 个关节角（短行，肉眼看极限用）=====
             * 2026-11-14：原来 C 页是长诊断行，现场看不清、也不方便标定限位。
             * 改成：6 个关节角 + ch0 当前限位，每次 C 页都打（≈每 300 ms 一次）；
             * 总线诊断（pca/off0/scan/bS/bD）每 10 次 C 页才打一次，保持短行。 */
            PCA_HealthCheck();
            for (uint8_t i = 0; i < ARM_CH_NUM; i++) {
                char nm[4];
                nm[0] = 'a'; nm[1] = (char)('0' + i); nm[2] = '='; nm[3] = '\0';
                AppendI(nm, (int)armAngle[i]);
            }
            AppendI("l0=", (int)armLimitMin[0]);      /* ch0 下限（逻辑角）*/
            AppendI("h0=", (int)armLimitMax[0]);      /* ch0 上限（逻辑角）*/
            AppendI("rcv=", (int)g_pcaRecover);
            /* 2026-11-14：轮显 6 个通道的 OFF 回读（每次页面只读 1 个，避免阻塞 1ms 中断）
             *   o=0     → 该通道输出常低 = 卸力/没配脉冲（舵机必然是"软"的）
             *   o≈100~500 → 正在输出 PWM（舵机会有力）*/
            {
                static uint8_t ocIdx = 0;
                AppendI("oc=", (int)ocIdx);
                AppendI("o=",  (int)PCA_ReadOff(ocIdx));
                ocIdx = (uint8_t)((ocIdx + 1) % ARM_CH_NUM);
            }
            {
                static uint8_t diagCnt = 0;
                if (++diagCnt >= 10) {
                    diagCnt = 0;
                    AppendI("pca=",  (int)g_pcaMode1);
                    AppendI("off0=", (int)PCA_ReadOff(0));
                    AppendI("scan=", (int)g_scanAddr);
                    AppendI("bS=",   (int)g_busScl);
                    AppendI("bD=",   (int)g_busSda);
                }
            }
        }
        page = (uint8_t)((page + 1) % 3);
        /* ★ 不许在这里调 UsbPrintf/CDC_Transmit_FS（实测会把 USB 控制传输搞死：
         * 设备能枚举、COM 口打不开）。这里只把拼好的行交给主循环去发。 */
        if (DBG_USB_PRINT && !g_txReady) {
            memcpy(g_tx, g_line, sizeof(g_tx));
            g_txReady = 1;
        }
    }
    UserMotor.Handle();
}