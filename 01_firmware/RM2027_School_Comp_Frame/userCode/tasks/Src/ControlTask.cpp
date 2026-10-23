//
// Created by LEGION on 2021/10/4.
//
#include "ControlTask.h"

/* ===== 2026-10-23 遥控数据合理性判据 =====
 * 实测踩到：接收机没对码时 USART3 上有帧（长度正好 25），解析出来
 * col = -1.52（超出 -1~1）、sL=sR=0（恰好等于 MID_POS）→ 误判"用户选了中档"
 * → UserSetSpeed(-45.7) 把 stopFlag 解开 = 电机在没人操作时被使能。
 * 所以除了"有没有收到帧"，还要判断"收到的数据像不像人给的"。 */
static uint8_t rcDataSane(void)
{
    return (fabsf(RemoteControl::rcInfo.right_col) <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.left_col)  <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.right_rol) <= 1.05f) &&
           (fabsf(RemoteControl::rcInfo.left_rol)  <= 1.05f);
}

/* ===== 台架模式：没有可用遥控器时，自动驱动单个轮子验证接线/ID/方向 =====
 * ⚠️ 上电前必须把轮子垫起来（悬空），否则车会突然窜出去。
 * 波形：+20 rpm 保持 2 s → -20 rpm 保持 2 s → 循环。
 * 想测别的轮子，改 BENCH_WHEEL_IDX（0左前 1右前 2左后 3右后）后重烧。 */
#define DBG_BENCH_WHEEL  1
#define BENCH_WHEEL_IDX  2      /* 现在接的是左后轮 = 2 */

/* 2026-10-23：单轮测试先固定用一个轮子，别听摇杆的。
 * 实测发现：推右摇杆时 left_rol 也在动（`wi` 从 2 变成 0），于是目标跑到了
 * ID1（没接电机）上，看着像"电机不转"。先把下标钉死，把"轮子能不能转"验掉；
 * 通道映射查清后再放开摇杆选择。改这里试别的轮子：0左前 1右前 2左后 3右后。 */
#define TEST_WHEEL_FIXED 2

void CtrlHandle(){
    /* ===== 2026-10-21 安全修复 =====
     * 没有接收机时 rcInfo 是零初始化 → sLeft 默认 0 = MID_POS → 会误判"用户选了中档"
     * → UserSetSpeed(0) 把 stopFlag 解开（实测 stop 从 1 变成 0）。
     * 所以先判断遥控是否在线：100ms 内没收到帧 → 强制进安全态，直接返回。 */
    if (!RemoteAlive() || !rcDataSane()) {
        UserStop();                      /* 机构一律断电 */
#if DBG_BENCH_WHEEL
        /* 台架模式：无人操作时按方波驱动单个轮子（轮子必须悬空！） */
        static uint32_t benchT0 = 0;
        if (benchT0 == 0) benchT0 = HAL_GetTick();
        uint32_t phase = (HAL_GetTick() - benchT0) / 2000u;
        float v = (phase & 1u) ? -20.0f : 20.0f;
        ChassisSetWheelSpeed(BENCH_WHEEL_IDX, v);
#else
        ChassisStop();
#endif
        return;
    }

    if (RemoteControl::rcInfo.sRight == DOWN_POS){
        //右侧下三档，急停模式，请勿更改！！！
        ChassisStop();
        UserStop();
    } else {
        //其他正常模式
        switch (RemoteControl::rcInfo.sLeft)
        {
        case UP_POS: //左侧一档
            // 2026-10-20：满杆系数由 4.2 改成 3.0。
            // 原因：M3508 输出轴空载只有 482 rpm ≈ 3.85 m/s，4.2 会让 PID 长期顶在 16384 饱和
            //（表现是"推满杆反而发死"）。3.0 m/s 留了余量，底盘稳了再往上加。
            // 第三个参数 60 = 自转满杆 60 rpm（≈306.9 rpm 输出轴），不用改。
            ChassisSetVelocity(RemoteControl::rcInfo.right_col*3.0,
                               RemoteControl::rcInfo.right_rol*3.0,RemoteControl::rcInfo.left_rol*60);
            break;
        case MID_POS: //左侧二档：用右摇杆控制 M2006（机构电机）
            // 2026-10-21：台架验证用。满杆 = ±30 rpm（输出轴，36:1 减速后很慢，安全）。
            // 中位 ±8% 死区，防止摇杆零点漂移导致电机自己爬。
            {
                float c = RemoteControl::rcInfo.right_col;
                if (c > -0.08f && c < 0.08f) c = 0.0f;
                UserSetSpeed(c * 30.0f);
            }
            break;
        case DOWN_POS: //左侧三档（SB 下档）：单轮方向校验（调试用，2026-10-23 加）
            /* M3 用：右摇杆推 → 只有 idx 指定的那一个轮子转，其余三轮目标清零。
             * **用左摇杆左右选轮子**（不用重烧固件就能逐个测）：
             *   左摇杆最左 = 0(左前 CMFL) | 稍左 = 1(右前 CMFR) | 稍右 = 2(左后 CMBL) | 最右 = 3(右后 CMBR)
             * 串口 B 行看 wi= 就是当前选中的下标。
             * 判据：给正目标时，这个轮子应朝"车往前开"的方向转；t 有数 r=0 说明 CAN/电调/ID 有问题。
             * 四轮都验证完 → 把本 case 清空，用 UP_POS 正常跑。 */
            {
                float c2 = RemoteControl::rcInfo.right_col;
                if (c2 > -0.08f && c2 < 0.08f) c2 = 0.0f;
#if TEST_WHEEL_FIXED >= 0
                ChassisSetWheelSpeed((uint8_t)TEST_WHEEL_FIXED, c2 * 30.0f);
#else
                float sel = RemoteControl::rcInfo.left_rol;                 // -1(左) .. +1(右)
                int idx = (int)((sel + 1.0f) * 0.5f * 3.999f);              // 0..3
                if (idx < 0) idx = 0;
                if (idx > 3) idx = 3;
                ChassisSetWheelSpeed((uint8_t)idx, c2 * 30.0f);
#endif
                UserStop();      /* 单轮测试时机构保持断电（避免 M2006 莫名解锁）*/
            }
            break;
        default:
            break;
        }
    }
}

