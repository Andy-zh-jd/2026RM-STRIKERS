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

/* ===== 2026-10-23：摇杆死区 =====
 * 实测：中位时通道仍有偏置（q0=39、q3=47 → 0.058 / 0.070），乘上满杆系数后
 * 会把轮子驱动到 40 rpm 左右，表现为"没有操作，前轮却在快速滚动"。
 * 所以四个摇杆轴都要过一遍死区：|v| < RC_DEADZONE 直接当 0。
 * 发射机里也可以做中立点微调，但代码里兜一层更可靠（换遥控器也不会翻车）。 */
#define RC_DEADZONE 0.10f

static float rcDeadzone(float v)
{
    if (v > -RC_DEADZONE && v < RC_DEADZONE) return 0.0f;
    return v;
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
#define CHASSIS_TEST_MODE 0   /* 驾驶模式 */

/* ===== 台架模式：没有可用遥控器时，自动驱动单个轮子验证接线/ID/方向 =====
 * ⚠️ 2026-10-23 起默认**关闭**：遥控已能正常工作，而且现在四个轮子逐渐装上车，
 * 无人操作时自动转轮子是危险行为。需要它时再改成 1（轮子必须悬空）。 */
#define DBG_BENCH_WHEEL  0
#define BENCH_WHEEL_IDX  2      /* 台架默认测左后轮 = 2 */

/* ===== 2026-10-23：自动动作循环（验证麦轮运动学用）=====
 * =1 时，SB 上档 + SC 上档 就会**自动**按 3 秒一拍循环这五个动作，不用推摇杆：
 *   0 前进 / 1 右移 / 2 左移 / 3 顺时针 / 4 逆时针
 * 速度都压到轮子输出 ~25-30 rpm（很慢，肉眼能看清），方便判断"车往哪走"。
 * 验证完运动学后改回 0。 */
#define DBG_SEQ_MODE  0

#if DBG_SEQ_MODE
/* 返回当前阶段，并给出该阶段的三轴速度 */
static uint8_t seqPhase(float *fb, float *lr, float *rt)
{
    static uint32_t t0 = 0;
    if (t0 == 0) t0 = HAL_GetTick();
    uint8_t ph = (uint8_t)(((HAL_GetTick() - t0) / 3000u) % 7u);
    *fb = 0.0f; *lr = 0.0f; *rt = 0.0f;
    switch (ph) {
    case 0: *fb = 0.20f;  break;   /* 前进   */
    case 1: *lr = -0.20f; break;   /* 右移   */
    case 2: *lr = +0.20f; break;   /* 左移   */
    case 3: *rt = -6.0f;  break;   /* 顺时针 */
    case 4: *rt = +6.0f;  break;   /* 逆时针 */
    case 5: ChassisSetWheelPattern4(+30.0f, 0.0f, 0.0f, +30.0f); break; /* 左前+右后 正转 */
    case 6: ChassisSetWheelPattern4(0.0f, +30.0f, +30.0f, 0.0f); break; /* 右前+左后 正转 */
    }
    return ph;
}
#endif

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
        /* 2026-10-23 实测标定（三个摇杆轴的方向）：
         *   右摇杆上推 → 前进       ：+，公式里 fb 正 = 前进，一致
         *   右摇杆右推 → 车右移     ：**要取负**，因为公式里 lr 正 = 向左
         *   左摇杆右推 → 顺时针自转 ：**要取负**，因为公式里 ω 正 = 俯视逆时针
         * 实测依据：右推时四个轮子呈"左前后、右前前"的组合，按运动学那正是"向左平移"；
         *           左摇杆右推时左侧后、右侧前，那是"逆时针"。两个都反了，所以这两项加负号。 */
        {
#if DBG_SEQ_MODE
            /* 自动循环演示（不用推摇杆）：看车往哪边走 */
            float sfb, slr, srt;
            uint8_t ph = seqPhase(&sfb, &slr, &srt);
            g_selWheel = ph;              /* 借 wi 字段把当前阶段号发到串口 */
            if (ph >= 5u) {
                /* 对角对照拍：seqPhase 里已经设好四个轮子的目标，别覆盖 */
            } else if (sfb == 0.0f && slr == 0.0f && srt == 0.0f) {
                ChassisStop();
            } else {
                ChassisSetVelocity(sfb, slr, srt);
            }
#else
            float v_fb = rcDeadzone(RemoteControl::rcInfo.right_col) * 3.0f;
            float v_lr = -rcDeadzone(RemoteControl::rcInfo.right_rol) * 3.0f;
            float v_rt = -rcDeadzone(RemoteControl::rcInfo.left_rol) * 60.0f;

            /* 2026-10-23：**摇杆全在中位 → 直接断电，而不是"0 速保持"**。
             * 之前用 SetTargetSpeed(0) 会让四个电机的 stopFlag 解开 = 通电保持在 0 速；
             * 电调零点偏移/编码器噪声/地面微斜都会让它慢慢爬（实测"没操作车也在动"就是它）。 */
            if (v_fb == 0.0f && v_lr == 0.0f && v_rt == 0.0f) {
                ChassisStop();          /* stopFlag=1：电机彻底不输出 */
            } else {
                ChassisSetVelocity(v_fb, v_lr, v_rt);
            }
#endif
        }
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
            float c = rcDeadzone(RemoteControl::rcInfo.right_col);
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




