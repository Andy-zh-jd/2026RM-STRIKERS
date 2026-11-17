//
// Created by LEGION on 2021/10/4.
//

#ifndef RM_FRAME_C_CHASSISTASK_H
#define RM_FRAME_C_CHASSISTASK_H
#include "main.h"
#include "Motor.h"

#define RPM2RADpS(num)(num * (3.1415926f/30.0f))
#define RADpS2RPM(num)(num / (3.1415926f/30.0f))
#define WHEEL_DIAMETER             0.1525f//麦轮直径 m
#define REDUCTION_RATIO 19.0f
#define TRACK_WIDTH            0.40f //轮距
#define WHEEL_BASE             0.38f //轴距
#define DEG2RAD                0.0174532925f   // °/s → rad/s

/* ===================== 2026-11-16 高速稳定/可操作性参数 =====================
 *
 * 背景：落地实测（44 rpm 目标）四轮跟踪 88~90%、轮间差 2~6%、稳态 gz≈±1.5°/s
 *      （3.6 s 偏 ~5°）。而偏航造成的横向位移 = 车速 × sin(航向) —— 车速一高，
 *      同样的 1.5°/s 就会从"每秒几毫米"变成"每秒几厘米"，就是"高速侧移明显"。
 *      另外满杆斜推 / 前进+自转时，逆解出的轮速会超过 450 rpm，超限的轮子
 *      被电机卡住、其余轮子照走 → 实际速度矢量方向偏离摇杆方向（也会被感知成侧移）。
 *
 * 下面每条都可以单独关掉（置 0 / 改回 1.0）做 A/B 对比。
 * ⚠️ 唯一必须先确认再开的是 CHASSIS_IMU_YAW_EN（要先在台架上确认 IMU 的
 *    yaw 轴和正方向，步骤见 03_docs/底盘高速侧移-诊断与修正.md）。
 * ======================================================================== */

/* ---- ① 四轮饱和归一化 ----
 * 逆解出的轮速超过电机能力时，四个轮子**等比缩放**（保住速度矢量方向）。
 * 不做的话：超限的轮子卡在 450 rpm，没超限的按指令走，方向就歪了。 */
#define CHASSIS_WHEEL_RPM_MAX   450.0f  /* 输出轴 rpm。M3508 24V 空载实测≈482，留 ~7% 余量 */
#define CHASSIS_DESAT_EN        1

/* ---- ② 速度指令斜坡 ----
 * 摇杆是阶跃输入，直接进逆解会让电流瞬间打满（四轮 ≈58 A，电池架持续上限 35 A），
 * 轮子空转打滑。限制加速度 = 让车"像车一样"起步，也顺便降低电机/电调压力。 */
#define CHASSIS_SLEW_EN         1
#define CHASSIS_ACC_LIN         6.0f    /* m/s²（M3508 四轮约能到 6~7，取 6 留余量） */
#define CHASSIS_DEC_LIN         10.0f   /* m/s²（松杆减速可以比加速快：刹车/急停更有底） */
#define CHASSIS_ACC_ANG         40.0f   /* rad/s²（0→满杆 6.28 rad/s 约 0.16 s） */

/* ---- ③ 转弯侧向加速度限制 ----
 * 车体速度矢量旋转时需要的侧向加速度 a_lat = |ω|·|v|。
 * 麦轮侧向抓地只有 ~0.45g（≈4.5 m/s²），超过就一定侧滑（甩尾/推头）。
 * 限制器**只砍平移速度、保留转向角速度**（转弯半径变大，但姿态可控）。
 * 3 m/s + 60 rpm 满转 → a_lat=18.8，物理上根本做不到，必然滑。 */
#define CHASSIS_ALAT_EN         1
#define CHASSIS_ALAT_MAX        5.0f    /* m/s²。麦轮侧向抓地估算 ≈0.45g≈4.5：5.0 保证"不失控"、
                                         * 不保证脏地面不打滑；实车若高速转弯甩尾，先降到 4.0~4.5
                                         * （见新文档 §5 调参表）；要更灵活可以到 6.0（更滑）。 */

/* ---- ④ IMU 直行航向锁 ----
 * 直行（|ω_cmd| < 0.3 rad/s）时锁住航向：陀螺积分得到航向角，角度误差 → 角速度修正。
 * 这是"高速侧移"的**根治项**：轮径误差/轮间静差/瞬时扰动造成的偏航会被实时拉回。
 *
 * 默认 0（关）：必须先按文档在台架上确认三件事再改 1 ——
 *   1) 哪个轴是 yaw（手转车体 90°，看 gx/gy/gz 哪个变化最大）
 *   2) gz 正方向是不是车体逆时针（逆时针转，gz 应该 +）
 *   3) 修正方向对不对（开 1 后给一个 10° 航向扰动，看车是不是往回转而不是越转越快）
 * 传感器坏/读不到时硬件自动退回"无航向锁"（IMU_Ready() 为假），不会把车带偏。 */
#define CHASSIS_IMU_YAW_EN      0
#define CHASSIS_IMU_YAW_AXIS    2       /* 0=gx 1=gy 2=gz（BMI088 默认平装时 yaw=gz） */
#define CHASSIS_IMU_YAW_SIGN    (+1.0f) /* gz 正 = 车体逆时针 时为 +1，反之为 -1 */
#define CHASSIS_YAW_KP          3.0f    /* 角度误差(rad) → 角速度修正(rad/s)，时间常数≈1/Kp·(1+Kd) */
#define CHASSIS_YAW_KD          0.25f   /* 角速度误差阻尼，抑制 200 Hz 采样下的抖动 */
#define CHASSIS_YAW_TURN_THR    0.30f   /* rad/s：超过它认为"驾驶员在转弯"，不做航向锁 */
#define CHASSIS_YAW_MIN_SPEED   0.15f   /* m/s：车基本不动时不做航向修正（防停车时自己慢慢转） */
#define CHASSIS_YAW_CORR_MAX    1.5f    /* rad/s：修正量上限（防止坏数据把车带飞） */

/* ---- ⑤ 每轮等效滚动半径/效率修正 ----
 * 四个麦轮的直径、辊子阻力、相对地面的滚动半径不可能完全一致：
 * 同样的轮速，左右两组的实际地面速度差 1% 就是高速跑偏的主因之一。
 * 先用 RAM 日志测出跑偏方向，再微调这 4 个数（1.0=不修正）： */
#define WHEEL_TRIM_FL           1.0f
#define WHEEL_TRIM_FR           1.0f
#define WHEEL_TRIM_BL           1.0f
#define WHEEL_TRIM_BR           1.0f

/* ---- ⑥ 摇杆标定 ----
 * 遥控器归一化后满杆只有 ±0.98 左右（中位还有 q0/q3 偏置），死区 0.10。
 * 这两条是"满杆对应多少车体速度"的唯一来源： */
#define CHASSIS_VXY_MAX         3.0f    /* m/s，满杆平移速度（麦轮纯前进极限约 3.6） */
#define CHASSIS_RT_MAX          60.0f   /* rpm，满杆自转（左摇杆左右），60 rpm = 360°/s */

/* ===================== 车载 RAM 日志（下地测试用） =====================
 * 为什么：跑车时不能连 USB（线会拽住车）。固件每 20 ms 把底盘关键量存进 RAM，
 * 跑完插 ST-Link 用 tools/read_log.py 以 attach 模式读出来（不清 RAM）。
 * 只在车动的时候记，1024 帧 ≈ 20.5 s，够记录一次高速直行/斜推/转弯。
 * 列定义（全部 int16，×10 存）：
 *   [0..3] t1..t4 目标输出轴 rpm（车体系，正=前进）
 *   [4..7] r1..r4 实测输出轴 rpm（车体系，已乘 SIGN）
 *   [8]    yaw 轴陀螺 °/s（已滤波去零偏，未乘 SIGN）
 *   [9]    饱和归一化比例 ×1000（1000=没限幅）
 *   [10]   实际下发等效自转 ω ×1000（rad/s）
 *   [11]   航向锁误差 ×10（°，未开航向锁时无意义） */
#define WHEEL_LOG_N      1024
#define WHEEL_LOG_COLS   12
extern int16_t  g_wheelLog[WHEEL_LOG_N][WHEEL_LOG_COLS];
extern volatile uint16_t g_wheelLogIdx;
extern volatile uint32_t g_wheelLogTotal;

/* ===================== 物理位置 → 电调 ID 对应表 =====================
 * 2026-10-23：实测发现电调的 ID 和"物理位置"对不上，而且 C620 的 SET 键很小、
 * 改 ID 麻烦。所以**不改硬件，改这张表**：框架里的 CMFL/CMFR/CMBL/CMBR
 * 只是四个对象名，绑哪个 ID 由这里决定。改完重烧即生效，逆解不用动。
 *
 * 2026-10-23 最终确认（**以"手转轮子看哪个 aN 变"为准**，不要用 LED 标签推）：
 *   左前 = ID 1
 *   左前 = ID 1
 *   右前 = ID 4
 *   左后 = ID 3
 *   右后 = ID 2
 * （与电调 LED 的绿灯闪数一致：左前闪 1、右前闪 4、左后闪 3、右后闪 2）
 *
 * ⚠️⚠️ **ID 是位掩码，不是普通数字**（2026-10-23 踩过的坑）：
 *   框架里 GET_MOTOR_POS(ID) = log2(ID)，而 MOTOR_ID_1..8 = 0x01,0x02,0x04,0x08,...
 *   所以 "ID4" 必须写成 MOTOR_ID_4 = 0x08，写 4 会被当成 MOTOR_ID_3（log2(4)=2）；
 *   写 3 更糟：log2(3)=1.58→截断成 1，会和 ID2 抢同一个槽（表现为 a2 恒 0）。
 *   ==> 一律用 MOTOR_ID_x 常量，不要写字面数字。
 *
 * ⚠️ 两条硬规则：
 *   1) 四个 ID 必须**互不相同**，否则同一 ID 的两个电调会撞车（橙灯闪 2 次、输出关闭）
 *   2) 不要用 ID 5（0x1FF 帧被 M2006 占了），也不要用 6~8（框架没建对象）
 */
#define ID_WHEEL_FL   MOTOR_ID_3   /* 左前 = 电调 ID 3 */
#define ID_WHEEL_FR   MOTOR_ID_4   /* 右前 = 电调 ID 4 */
#define ID_WHEEL_BL   MOTOR_ID_1   /* 左后 = 电调 ID 1 */
#define ID_WHEEL_BR   MOTOR_ID_2   /* 右后 = 电调 ID 2 */

/* ===================== 电机方向补偿（2026-10-23 实测） =====================
 * 逐轮实测：正目标时
 *   左前 → 车前进方向 ✅   左后 → 车前进方向 ✅
 *   右前 → 车后退方向 ❌   右后 → 车后退方向 ❌
 * 原因：底盘左右两侧电机是镜像安装的，同一"前进"要求两侧转子转向相反。
 *
 * 这里统一成「+1 = 车前进方向」，在**下发目标**和**显示回读**两处都乘这个符号，
 * 于是串口里的 t / r 永远在同一个（车体）坐标系里，便于判断"同号"。
 * 以后如果换了电机/改了安装方式，只改这四个数即可。 */
#define SIGN_WHEEL_FL   (+1.0f)
#define SIGN_WHEEL_FR   (-1.0f)
#define SIGN_WHEEL_BL   (+1.0f)
#define SIGN_WHEEL_BR   (-1.0f)
/*枚举类型定义------------------------------------------------------------*/

/*
 * 底盘任务错误信息枚举
 */
typedef enum{
    NONE
}CHASSIS_ERROR_E;

/*结构体定义--------------------------------------------------------------*/

/*
 * 底盘任务状态结构体
 */
typedef struct{

}CHASSIS_STATE_T;

/*
 * 底盘任务控制结构体
 */
typedef struct{


}CHASSIS_CONTROL_T;



/*结构体成员取值定义组------------------------------------------------------*/
/*外部变量声明-------------------------------------------------------------*/

extern CHASSIS_STATE_T     chassisState;
extern CHASSIS_CONTROL_T   chassisControl;

extern float FBVelocity,LRVelocity,RTVelocity;

/* 2026-11-16：运动控制内部状态快照（只读，给打印 / SWD / RAM 日志用）
 *   chassisYawDeg        陀螺积分航向（°）
 *   chassisYawTargetDeg  直行锁目标航向（°）
 *   chassisYawCorr       航向环输出的角速度修正（rad/s）
 *   chassisDesatScale    饱和归一化比例（1.0=没限幅）
 *   chassisOmegaEff      最终下发的等效自转（rad/s，含归一化）
 *   chassisDbgVx/Vy      最终下发的等效平移（m/s，含归一化）
 *   chassisImuOk         IMU_Ready() 的结果（0=坏/没读到，底盘自动退回无航向锁） */
extern volatile float chassisYawDeg, chassisYawTargetDeg, chassisYawCorr;
extern volatile float chassisDesatScale, chassisOmegaEff, chassisDbgVx, chassisDbgVy;
extern volatile uint8_t chassisImuOk;
/* 航向锁误差（°，已折到 ±180），打印/SWD/日志用 */
float ChassisYawErrDeg(void);

/* 2026-11-17：静止判定（1 = 四轮无目标、无转速，已连续 1.5 s）
 * 给 IMU 陀螺零偏自动学习用：只有真静止才学习，搬车/推车时不能学。 */
uint8_t ChassisIsStationary(void);

/* 2026-10-23：四个轮子的**目标**转速（输出轴 rpm），供串口打印/调试用。
 * 下标：0=左前(CMFL,ID1) 1=右前(CMFR,ID2) 2=左后(CMBL,ID3) 3=右后(CMBR,ID4) */
extern float wheelTargetRpm[4];

/* 单轮调试时当前选中的轮子下标（0=左前 1=右前 2=左后 3=右后），打印用 */
extern uint8_t g_selWheel;

/*外部函数声明-------------------------------------------------------------*/

void WheelsSpeedCalc(float fbVelocity, float lrVelocity, float rtVelocity);
void ChassisStop();
void ChassisSetVelocity(float _fbV,float _lrV,float _rtV);

/* 2026-10-23：单轮调试入口（M3 方向校验用）
 * 只给指定的一个轮子下目标转速，其余三轮目标清零；绕过麦轮逆解。
 * idx: 0=左前 1=右前 2=左后 3=右后；rpm 为输出轴转速（正负=方向）
 * 退出方式：调 ChassisStop() 或 ChassisSetVelocity() */
void ChassisSetWheelSpeed(uint8_t idx, float rpm);
#endif //RM_FRAME_C_CHASSISTASK_H

