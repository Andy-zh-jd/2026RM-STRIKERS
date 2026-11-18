//
// Created by LEGION on 2021/10/10.
//

#ifndef RM_FRAME_C_REMOTECONTROL_H
#define RM_FRAME_C_REMOTECONTROL_H

#include "Device.h"

#define SBUS_RX_BUF_NUM 36u
#define RC_FRAME_LENGTH 25u
#define RC_CH_VALUE_OFFSET 1024
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

/* 2026-10-21：遥控「在线」判断。
 * 为什么需要：没有接收机时 rcInfo 是零初始化，sLeft 默认 = 0 = MID_POS，
 * 于是 CtrlHandle 会以为"用户选了中档" → 解开 stopFlag → 电机失去安全态（实测踩到）。
 * RemoteAlive() 超过 100ms 没收到合法 DBUS 帧就返回 0，上层据此强制进安全态。 */
extern volatile uint32_t rc_last_tick;
uint8_t RemoteAlive(void);

/* ===== 2026-11-03 审计修复：开关通道的"上档"判据 =====
 * optional[0]/optional[1] 是**原始 11 bit**（0~2047），中位就是 1024。
 * 原来 JackActive() 写 `> 1000`，问题两个：
 *   ① 中位 1024 > 1000 → 三位开关停中档就被判成"爬台阶"；
 *   ② 发射机上**没配置**的通道默认输出中位 → 永久判成"爬台阶"。
 * 后果（11-02 实车实测）：底盘一直 ChassisStop()、机械臂在 ArmControl() 第一行 return，
 * 串口里却完全看不出来（当时没打印 optional[]）。
 * 1400 = 中位 1024 + 376，只认明确拨到上档的值（三位开关上档≈1684 / 两位≈1684~2047）。
 * ⚠ 换发射机后先看串口 op0=/op1= 的实测值再决定要不要动这个数。 */
#define RC_SWITCH_HIGH      1400

#endif //RM_FRAME_C_REMOTECONTROL_H
