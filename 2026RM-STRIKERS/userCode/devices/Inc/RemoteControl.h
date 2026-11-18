//
// Created by LEGION on 2021/10/10.
//

#ifndef RM_FRAME_C_REMOTECONTROL_H
#define RM_FRAME_C_REMOTECONTROL_H

#include "Device.h"

#define SBUS_RX_BUF_NUM 36u
#define RC_FRAME_LENGTH 25u

/* 诊断用：上一次收到的遥控帧长度（0=没收到，25=SBUS，18=DJI DBUS）*/
extern volatile uint16_t g_rcLen;
#define RC_CH_VALUE_OFFSET 1024

/* ===== 2026-11-03 审计修复：开关通道的"上档"判据 =====
 * optional[0]/optional[1] 是**原始 11 bit**（0~2047），中位就是 1024。
 * 原来两处都写 `> 1000` 当"SA 拨到高 / 爬台阶"的判据，问题有两个：
 *   ① 中位 1024 > 1000 → 三位开关停在中档就被判成"上档"；
 *   ② 发射机上**没配置**的通道默认输出就是中位 → 永久判成"上档"。
 * 后果实测过：底盘一直 ChassisStop()（完全不动）、机械臂在 ArmControl() 第一行就 return。
 * 阈值取 1400（= 中位 1024 + 376），只认"明确拨到上档"的值：
 *   三位开关上档 ≈1684、两位开关上档 ≈1684/2047、旋钮拧到底 = 2047 → 都能过；
 *   中位 1024 和没配置的通道 → 全部判为"低"。
 * ⚠ 换发射机/改通道配置后，先看串口 op0=/op1= 的实测值，再决定要不要动这个数。 */
#define RC_SWITCH_HIGH      1400

/* 摇杆死区：实测中位仍有偏置（q0=39 / q3=47 → 0.058 / 0.070），
 * 不处理会让轮子在没有操作时自己慢慢爬。 */
#define RC_STICK_DEADZONE   0.10f
/*枚举类型定义------------------------------------------------------------*/
typedef enum{
    UP_POS = 97u,MID_POS = 0u,DOWN_POS = 158u
}SWITCH_STATE_E;
/*结构体定义--------------------------------------------------------------*/
typedef struct {
    int32_t ch[4];
    uint32_t s[2];
    int16_t optional[2];
}rc_t;

typedef struct {
    int32_t x,y,z,press_l,press_r;
}mouse_t;

typedef struct {
    uint32_t v;
}keyboard_t;

typedef struct {
    rc_t rc;
    mouse_t mouse;
    keyboard_t key;
}RC_ctrl_t;
typedef struct {
    float left_col,left_rol;
    float right_col,right_rol;
    SWITCH_STATE_E sLeft;
    uint32_t pat;
    SWITCH_STATE_E sRight;
    int16_t optional[2];
}RC_Info_t;
/*类型定义----------------------------------------------------------------*/
class RemoteControl : public Device {
public:
    static RC_Info_t rcInfo;
    static RC_ctrl_t rc_ctrl;

    static void init();
    static void ITHandle();
    static uint8_t sbus_rx_buf[2][SBUS_RX_BUF_NUM];
    static void sbus_to_rc(volatile const uint8_t *sbus_buf);
};

/*结构体成员取值定义组------------------------------------------------------*/

/*外部变量声明-------------------------------------------------------------*/

/*外部函数声明-------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif
extern void USART3_IRQHandler(void);
#ifdef __cplusplus
}
#endif

/* ===== 2026-11-03 审计修复：遥控"在线"判定 =====
 * 为什么需要：接收机没对码/线掉了的时候，rcInfo 会停在最后一次的数值（或者被
 * 全 0 帧刷成 col=-1.52、sL=sR=0=中位）。没有这个判定，CtrlHandle 就会拿
 * 陈旧/垃圾数据继续给底盘速度 —— 这就是"没人碰车它自己跑"的来源。
 * 判据：100 ms 内收到过一帧"合法"（非全 0）的 SBUS 帧。 */
extern volatile uint32_t rc_last_tick;
uint8_t RemoteAlive(void);

#endif //RM_FRAME_C_REMOTECONTROL_H
