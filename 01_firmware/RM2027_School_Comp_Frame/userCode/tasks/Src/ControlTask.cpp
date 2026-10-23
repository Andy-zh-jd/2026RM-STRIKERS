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

/* ===================== 模式选择 =====================
 * CHASSIS_TEST_MODE = 1 → **逐轮验收模式**（2026-10-23 起）：
 *     SC(右三段) + SB(左三段) 组合选轮子，右摇杆上下给转速（±30 rpm 输出轴）。
 *     | SC | SB   | 测哪个轮 | 电调 ID |
 *     |----|------|----------|---------|
 *     | 上 | 上   | 0 左前   | 1 |
 *     | 中 | 上   | 1 右前   | 2 |
 *     | 上 | 中/下| 2 左后   | 3 |
 *     | 中 | 中/下| 3 右后   | 4 |
 *     | 下 | 任意 | **急停**（优先于上面所有组合）|
 *   为什么要这样：换轮子不用重烧固件、也不会因为手碰左摇杆误改下标
 *   （之前用左摇杆选轮，实测推右摇杆时下标从 2 跳到 0，目标跑到没接电机的 ID1 上）。
 *
 * CHASSIS_TEST_MODE = 0 → 正常驾驶模式：SB 上档 = 底盘听摇杆，中档 = M2006。
 * 四轮验收完、底盘装好之后，把这里改成 0。 */
#define CHASSIS_TEST_MODE 0   /* 2026-10-23：四轮方向已验收完，切回正常驾驶模式 */

/* ===== 台架模式：没有可用遥控器时，自动驱动单个轮子验证接线/ID/方向 =====
 * ⚠️ 2026-10-23 起默认**关闭**：遥控已能正常工作，而且现在四个轮子逐渐装上车，
 * 无人操作时自动转轮子是危险行为。需要它时再改成 1（轮子必须悬空）。 */
#define DBG_BENCH_WHEEL  0
#define BENCH_WHEEL_IDX  2      /* 台架默认测左后轮 = 2 */

#if CHASSIS_TEST_MODE
/* SC/SB → 轮子下标：0左前 1右前 2左后 3右后（真值表见上） */
static uint8_t wheelFromSwitches(void)
{
    uint8_t scUp = (RemoteControl::rcInfo.sRight == UP_POS) ? 1u : 0u;
    uint8_t sbUp = (RemoteControl::rcInfo.sLeft  == UP_POS) ? 1u : 0u;
    return (uint8_t)((sbUp ? 0u : 2u) + (scUp ? 0u : 1u));
}
#endif

void CtrlHandle(){
    /* ===== 2026-10-21 安全修复 =====
     * 没有接收机时 rcInfo 是零初始化 → sLeft 默认 0 = MID_POS → 会误判"用户选了中档"
     * → UserSetSpeed(0) 把 stopFlag 解开。所以先判断遥控是否在线：
     * 100ms 内没收到帧、或收到的数据不像人给的 → 强制进安全态，直接返回。 */
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

    /* 急停优先级最高：SC 下档。请勿更改。 */
    if (RemoteControl::rcInfo.sRight == DOWN_POS){
        ChassisStop();
        UserStop();
        return;
    }

#if CHASSIS_TEST_MODE
    /* ---------- 逐轮验收：一次只让一个轮子转 ---------- */
    {
        uint8_t idx = wheelFromSwitches();
        float c2 = RemoteControl::rcInfo.right_col;      /* 右摇杆上下 */
        if (c2 >  1.0f) c2 =  1.0f;
        if (c2 < -1.0f) c2 = -1.0f;
        if (c2 > -0.08f && c2 < 0.08f) c2 = 0.0f;        /* 中位死区，防零漂 */
        ChassisSetWheelSpeed(idx, c2 * 30.0f);           /* 满杆 = 30 rpm 输出轴 */
        UserStop();                                      /* 机构保持断电 */
    }
    return;
#else
    /* ---------- 正常驾驶 ---------- */
    switch (RemoteControl::rcInfo.sLeft)
    {
    case UP_POS: //左侧一档：底盘听摇杆
        // 2026-10-20：满杆系数由 4.2 改成 3.0。
        // 原因：M3508 输出轴空载只有 482 rpm ≈ 3.85 m/s，4.2 会让 PID 长期顶在 16384 饱和
        //（表现是"推满杆反而发死"）。3.0 m/s 留了余量，底盘稳了再往上加。
        // 第三个参数 60 = 自转满杆 60 rpm（≈306.9 rpm 输出轴），不用改。
        ChassisSetVelocity(RemoteControl::rcInfo.right_col*3.0,
                           RemoteControl::rcInfo.right_rol*3.0,
                           RemoteControl::rcInfo.left_rol*60);
        UserStop();      /* 2026-10-23：驾驶档位时机构必须断电，否则从中档切上来会保持上次的目标 */
        break;

    case MID_POS: //左侧二档：用右摇杆控制 M2006（机构电机）
        // 台架验证用。满杆 = ±30 rpm（输出轴，36:1 减速后很慢，安全）。
        /* ⚠️ 2026-10-23 修 bug：这里必须停底盘。
         * 否则 manualWheelMode 会一直保持 1，ChassisHandle 继续按"上次单轮调试的目标"
         * 驱动轮子 → 现象是"没有任何操作，前轮却在快速滚动"，而且 t1..t4 出现
         * ++-- 这种逆解不可能产生的组合（陈旧目标拼在一起）。 */
        ChassisStop();
        {
            float c = RemoteControl::rcInfo.right_col;
            if (c > -0.08f && c < 0.08f) c = 0.0f;
            UserSetSpeed(c * 30.0f);
        }
        break;

    case DOWN_POS: //左侧三档：底盘与机构都断电
        ChassisStop();
        UserStop();
        break;

    default:
        ChassisStop();
        UserStop();
        break;
    }
#endif
}
