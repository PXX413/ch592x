/**************************************************************************
 * key_matrix.c —— CH592X 3x3 矩阵键盘驱动（支持组合按键）
 *
 * 扫描原理：
 *   列 = 上拉输入；行 = 平时高阻输入，扫描时"逐行拉低"。
 *   某行拉低时，该行上按下的键会把对应列拉低 → 读回来就是按下。
 *   注意：**同一时刻只允许一行为低电平**，其余行必须是高阻输入，
 *         否则两个按键（不同行同一列）会把两个输出直接对拉，烧端口。
 *
 * 组合按键：
 *   1) 整幅位图去抖 —— 同时按下的多个键在同一幅位图里，天然一起上报；
 *   2) 组合键窗口 —— 先按下 K1、再按 K2 时，如果两次间隔 < KEY_COMBO_WINDOW_MS，
 *      就合并成一次 "K1+K2" 事件，不会先报 K1 再报 K1+K2；
 *   3) 组合键查表 —— KEY_COMBO_TABLE 里把位图映射成编号，交给应用层用。
 *
 * 幽灵键（ghosting）：无二极管矩阵的老问题。同时按下 3 个键且构成 L 形时，
 *   第 4 个角会被"看成"按下（例：按 2/3/5 会多出 6）。
 *   【2026-10-13 修复】本驱动现在**主动抑制**幽灵键（见文件后半 key_deghost()）：
 *   按扫描顺序"先到先得"，行列都已被占用的格子判为幽灵丢弃，正常三键组合
 *   （2/3/5）正确上报，不再多出一个幽灵键。ev.ghost / KEY_GhostDropped()
 *   会置 1 表示"本次确实抑制掉了幽灵键"。
 **************************************************************************/

#include "key_matrix.h"

/*==========================================================================
 * 行 / 列 引脚表
 *========================================================================*/
static const uint8_t  s_row_b[3] = {KEY_R1_PORTB, KEY_R2_PORTB, KEY_R3_PORTB};
static const uint32_t s_row_m[3] = {KEY_R1_PIN,   KEY_R2_PIN,   KEY_R3_PIN};
static const uint8_t  s_col_b[3] = {KEY_C1_PORTB, KEY_C2_PORTB, KEY_C3_PORTB};
static const uint32_t s_col_m[3] = {KEY_C1_PIN,   KEY_C2_PIN,   KEY_C3_PIN};

/*==========================================================================
 * GPIO 底层（portb: 0 = PA，1 = PB；mask 用 EVT 的 GPIO_Pin_x）
 *========================================================================*/
static void pin_as_input_pu(uint8_t portb, uint32_t mask)
{
    if (portb) { GPIOB_ModeCfg(mask, GPIO_ModeIN_PU); }
    else       { GPIOA_ModeCfg(mask, GPIO_ModeIN_PU); }
}

static void pin_as_output(uint8_t portb, uint32_t mask)
{
    if (portb) { GPIOB_ModeCfg(mask, GPIO_ModeOut_PP_5mA); }
    else       { GPIOA_ModeCfg(mask, GPIO_ModeOut_PP_5mA); }
}

static void pin_write(uint8_t portb, uint32_t mask, uint8_t level)
{
    if (level)
    {
        if (portb) { GPIOB_SetBits(mask); } else { GPIOA_SetBits(mask); }
    }
    else
    {
        if (portb) { GPIOB_ResetBits(mask); } else { GPIOA_ResetBits(mask); }
    }
}

static uint8_t pin_read(uint8_t portb, uint32_t mask)
{
    uint32_t v = portb ? GPIOB_ReadPortPin(mask) : GPIOA_ReadPortPin(mask);
    return (v != 0u) ? 1u : 0u;
}

/*==========================================================================
 * 状态
 *========================================================================*/
static uint16_t s_raw_last;         /* 上一次原始位图 */
static uint16_t s_raw_stable;       /* 去抖后的位图 */
static uint16_t s_reported;         /* 已经上报过的位图 */
static uint16_t s_debounce_ms;      /* 当前位图已经稳定了多少毫秒 */
static uint16_t s_tick_ms;          /* 模块内部毫秒里程（16 位，回绕无所谓） */
static uint16_t s_change_tick;      /* 位图最后一次变化的时刻 */
static uint16_t s_dbg_raw_read;     /* 【诊断】最近一次 key_read_raw() 的结果 */
static KeyMsFn_t s_ms_fn;           /* 外部时间源；0 = 用内置近似 */
static uint16_t s_fallback_ms;      /* 没有时间源时的自累加里程 */
static uint16_t s_sample_tick;      /* 【性能】上一次真正做 GPIO 扫描的时刻 */

/* 取一次"现在几毫秒"。有外部时间源就用它，没有就按扫描节拍自累加
 * （等价于老行为：每次 KEY_Scan() 前进 KEY_SCAN_PERIOD_MS 毫秒）。 */
static uint16_t key_now_ms(void)
{
    if (s_ms_fn != 0)
    {
        return (uint16_t)(s_ms_fn() & 0xFFFFu);
    }

    s_fallback_ms = (uint16_t)(s_fallback_ms + KEY_SCAN_PERIOD_MS);
    return s_fallback_ms;
}

void KEY_SetTimeSource(KeyMsFn_t fn)
{
    s_ms_fn       = fn;
    s_fallback_ms = 0u;
    s_tick_ms     = 0u;             /* 里程清零，下一次扫描从 0 起算 */
    s_change_tick = 0u;
}
static KeyEvent_t s_event;          /* 待取事件 */
static bool     s_event_pending;

/*==========================================================================
 * 小工具：统计位图里按下了几个键
 *========================================================================*/
static uint8_t key_popcount(uint16_t v)
{
    uint8_t n = 0u;

    while (v != 0u)
    {
        n = (uint8_t)(n + (uint8_t)(v & 1u));
        v = (uint16_t)(v >> 1);
    }
    return n;
}

/*==========================================================================
 * 初始化
 *========================================================================*/
void KEY_Init(void)
{
    uint8_t i;

#if ((KEY_R1_PORTB == 1) && (KEY_R1_PIN == GPIO_Pin_14)) || \
    ((KEY_R1_PORTB == 1) && (KEY_R1_PIN == GPIO_Pin_15)) || \
    ((KEY_R2_PORTB == 1) && (KEY_R2_PIN == GPIO_Pin_14)) || \
    ((KEY_R2_PORTB == 1) && (KEY_R2_PIN == GPIO_Pin_15)) || \
    ((KEY_R3_PORTB == 1) && (KEY_R3_PIN == GPIO_Pin_14)) || \
    ((KEY_R3_PORTB == 1) && (KEY_R3_PIN == GPIO_Pin_15)) || \
    ((KEY_C1_PORTB == 1) && (KEY_C1_PIN == GPIO_Pin_14)) || \
    ((KEY_C1_PORTB == 1) && (KEY_C1_PIN == GPIO_Pin_15)) || \
    ((KEY_C2_PORTB == 1) && (KEY_C2_PIN == GPIO_Pin_14)) || \
    ((KEY_C2_PORTB == 1) && (KEY_C2_PIN == GPIO_Pin_15)) || \
    ((KEY_C3_PORTB == 1) && (KEY_C3_PIN == GPIO_Pin_14)) || \
    ((KEY_C3_PORTB == 1) && (KEY_C3_PIN == GPIO_Pin_15))
    /* 用到 PB14/PB15 时必须关掉仿真调试口，否则这两个脚不受 GPIO 控制。
     * 关掉后 WCH-Link 可能无法再直接下载，需要用 ISP 方式恢复！ */
    R16_PIN_ALTERNATE |= RB_DEBUG_EN;
#endif

    /* 列：上拉输入 */
    for (i = 0; i < 3u; i++)
    {
        pin_as_input_pu(s_col_b[i], s_col_m[i]);
    }

    /* 行：先全部高阻输入（不驱动），扫描时才逐行拉低 */
    for (i = 0; i < 3u; i++)
    {
        pin_as_input_pu(s_row_b[i], s_row_m[i]);
    }

    s_raw_last      = 0u;
    s_raw_stable    = 0u;
    s_reported      = 0u;
    s_debounce_ms   = 0u;
    s_tick_ms       = 0u;
    s_change_tick   = 0u;
    s_sample_tick   = 0u;
    s_event_pending = false;
}

/*==========================================================================
 * 读一幅原始位图（扫描一遍 3 行）
 *========================================================================*/
/*==========================================================================
 * 物理格 -> 逻辑键号 映射表（★ 换接线只需改这张表）
 *   下标 = 扫描时的物理格顺序：r*3 + c
 *           r = 驱动的第几条"行线"（KEY_R1/R2/R3 的顺序）
 *           c = 第几条"列线"（KEY_C1/C2/C3 的顺序）
 *   值   = 这一格应该报成哪个键（1~9）
 *
 *   行/列引脚已按《CH592X引脚定义》对齐：
 *     行1=PA10  行2=PB6  行3=PB0      列1=PB15  列2=PA9  列3=PA8
 *   引脚顺序与上面的键位图一致，所以这里是**全等表**。
 *   只有当实际接线顺序和键位图不一致时，才需要在这张表里换位。
 *==========================================================================*/
static const uint8_t s_scan_key[9] =
{
    1u, 2u, 3u,      /* 物理第 1 行：K1 / K2 / K3 */
    4u, 5u, 6u,      /* 物理第 2 行：K4 / K5 / K6 */
    7u, 8u, 9u       /* 物理第 3 行：K7 / K8 / K9 */
};
static uint16_t key_read_raw(void)
{
    uint16_t map = 0u;
    uint8_t  r;
    uint8_t  c;

    /* 1) 先把所有行放成高阻输入，避免两个输出对拉 */
    for (r = 0; r < 3u; r++)
    {
        pin_as_input_pu(s_row_b[r], s_row_m[r]);
    }

    /* 2) 逐行拉低，读列
     *
     * 【2026-10-13 修复：按键间歇性锁死】
     *   原来这 3 次 pin_read 是【开着中断】做的。只要中断正好在读列那一瞬间插进来
     *   （TMR0 的 ENC_Scan、RF、USB 都会），读回的列状态就可能和上一轮不同 ——
     *   于是 raw 每隔几毫秒就抖一次。
     *   而 KEY_Scan() 的去抖是【一变就清零】，抖动比 15ms 还密时 s_debounce_ms
     *   永远累积不到阈值，s_raw_stable 冻住 —— 9 个键全死；
     *   编码器走 ENC_ButtonPressed()，不经过这里，所以它照常好用。
     *   全部松开后 raw 变回稳定的 0，去抖能累积，于是【等一会儿自己恢复】。
     *
     *   修法：只在【真正读那 3 个列脚】的几微秒里关中断，不是整段扫描。
     *   每行约 2 微秒、整轮约 6 微秒，对 USB/RF 的影响可忽略。
     *   行拉低和 DelayUs 等待都留在关中断之外，不影响总线时序。 */
    for (r = 0; r < 3u; r++)
    {
        uint32_t intlev;

        pin_as_output(s_row_b[r], s_row_m[r]);
        pin_write(s_row_b[r], s_row_m[r], 0u);      /* 该行拉低 */

        DelayUs(KEY_ROW_SETTLE_US);                 /* 等上拉 RC 稳定 */

        intlev = __risc_v_disable_irq();            /* <<< 只护住这 3 次读 */
        for (c = 0; c < 3u; c++)
        {
            if (pin_read(s_col_b[c], s_col_m[c]) == 0u)
            {
                uint8_t k = s_scan_key[r * 3u + c];      /* 物理格 -> 逻辑键号 */
                map |= (uint16_t)(1u << (k - 1u));      /* 低电平 = 按下 */
            }
        }
        __risc_v_enable_irq(intlev);

        pin_as_input_pu(s_row_b[r], s_row_m[r]);    /* 立刻恢复高阻 */
    }

    s_dbg_raw_read = map;                           /* 诊断探针 */
    return map;
}

/*==========================================================================
 * 扫描 + 去抖 + 组合键窗口
 *========================================================================*/
/*==========================================================================
 * 【新增】幽灵键抑制（无二极管矩阵的 L 形三键）
 *
 * 现象：同时按 2 / 3 / 5，整幅位图会读出 2 / 3 / 5 / 6。
 * 原理：扫第 2 行拉低时，电流走 5→2→R1→3 这条"两触点串联"的旁路，
 *       把第 3 列（K6 那一列）也拉低，于是 K6 被误判为按下。
 *       这是没有二极管时矩阵的**固有限制**，硬件层面无法区分。
 *
 * 处理办法（按扫描顺序"先到先得"）：
 *   逐格按扫描顺序（行 1→3、列 1→3）检查，接受一个键的条件是
 *   **它所在的行、列里至少有一边还没有被接受的键**；
 *   行、列**都**已经有键的格子，判为幽灵键丢弃。
 *
 *   例：读出 {2,3,5,6}
 *     2 (行1,列2)：行空、列空 → 收 √（真键）
 *     3 (行1,列3)：行已有 2、列空 → 收 √（真键）
 *     5 (行2,列2)：行空、列已有 2 → 收 √（真键）
 *     6 (行2,列3)：行已有 5、列已有 3 → **丢弃** ×（幽灵）
 *   结果 = {2,3,5}，正是用户按下的三个键。
 *
 * 副作用（必须在文档里写明）：四个真键正好构成矩形时（例如同时按
 *   2/3/7/8），按扫描顺序最后一个会被当成幽灵丢掉 —— 要彻底解决
 *   只能给每个按键串一个二极管。本规则保证"少报"而不是"误报"，
 *   且只要松开任意一个键，剩余键会在去抖后恢复上报。
 *========================================================================*/
static uint8_t s_ghost_dropped;         /* 最近一次扫描是否丢了幽灵键（诊断用） */

static uint16_t key_deghost(uint16_t raw)
{
    uint16_t clean = 0u;
    uint8_t  row_has[3] = {0u, 0u, 0u};
    uint8_t  col_has[3] = {0u, 0u, 0u};
    uint8_t  r, c;

    s_ghost_dropped = 0u;

    for (r = 0u; r < 3u; r++)
    {
        for (c = 0u; c < 3u; c++)
        {
            uint8_t k = s_scan_key[r * 3u + c];

            if ((raw & (uint16_t)(1u << (k - 1u))) == 0u)
            {
                continue;                   /* 这一格没按下 */
            }

            if ((row_has[r] != 0u) && (col_has[c] != 0u))
            {
                s_ghost_dropped = 1u;       /* 行列都被占：幽灵键，丢弃 */
                continue;
            }

            clean |= (uint16_t)(1u << (k - 1u));
            row_has[r] = 1u;
            col_has[c] = 1u;
        }
    }

    return clean;
}


/*==========================================================================
 * 【诊断 2026-10-13】按键间歇性锁死的只读探针
 *
 * 症状：9 个矩阵键间歇性全死，但编码器（按键+旋转）和 UI 都正常。
 * 因为 9 键走 KEY_Scan() 的 s_raw_stable，编码器走 ENC_ButtonPressed()，
 * 两条来源完全独立 —— 问题一定在 s_raw_stable 被冻住。
 *
 * 复现时看 KeyDbg_t 里这几个值就能定位：
 *   debounce 一直是 0/1 涨不上去 -> 去抖被反复打断（raw 在抖）
 *   raw_read 每次都在变          -> key_read_raw() 读出来就是抖的
 *   stable 冻在某个非 0 值       -> 真的卡住了（而不是没扫）
 *   blocked = 1                  -> KEY_BlockInput 没退出，整段扫描被跳过
 *========================================================================*/
void KEY_DbgGet(KeyDbg_t *out)
{
    if (out == 0) { return; }

    out->raw_read = s_dbg_raw_read;
    out->raw_last = s_raw_last;
    out->stable   = s_raw_stable;
    out->reported = s_reported;
    out->debounce = s_debounce_ms;
    out->blocked  = KEY_IsBlocked() ? 1u : 0u;
    out->ghost    = s_ghost_dropped;
}

/* 最近一次扫描是否丢弃过幽灵键（1 = 丢过）。给上屏诊断/日志用。 */
uint8_t KEY_GhostDropped(void)
{
    return s_ghost_dropped;
}

/*==========================================================================
 * 【新增】按键输入屏蔽（进键盘页前 0.5 秒不读按键）
 *
 * 用途：切页的那一瞬间手上往往还按着键（或触点还在抖），这些状态
 * 不该被当成"用户刚按下"灌进 HID 报文。
 * 屏蔽期间 RAW 位图直接按 0 处理 —— 于是：
 *   * 不会产生任何新的按下事件；
 *   * 之前按着的键会在去抖窗后自然上报"松开"，不会卡住主机的键。
 *========================================================================*/
static uint16_t s_block_until;          /* 屏蔽到这一刻（16 位毫秒里程） */
static uint32_t s_block_at;             /* 【2026-10-13】arm 时刻的 32 位毫秒 */
static uint8_t  s_block_armed;          /* 【2026-10-13】当前是否处于屏蔽中 */

/*==========================================================================
 * 【2026-10-13 修复：9 键间歇性锁死】
 *
 * 现场实测（诊断行）：卡住时 R/L/S 全 0、D 已经饱和到 15，**只有 B=1**。
 * 也就是：没有任何键按下、去抖早就稳定，纯粹是屏蔽标志退不出来，
 * KEY_Scan() 于是整段跳过，9 个矩阵键全死（编码器不经 KEY_Scan，照常好用）。
 *
 * 原来的实现只有一个 16 位终点时刻：
 *     s_block_until = now16 + ms;
 *     blocked = (int16_t)(s_block_until - now16) > 0;
 * 只要 kbd_enter() 被反复调用（长按连发 -> 页面处理函数反复执行 -> 反复进页），
 * 终点就会被无限往后推。另外 16 位只有 65.5 秒一圈，
 * 一旦越过半个量程，有符号比较的语义就会翻 —— 屏蔽可能永远退不出去。
 *
 * 现在加三道保险：
 *   1) **已经屏蔽中就忽略新的 KEY_BlockInput()**（不续期）；
 *   2) 用 32 位绝对时刻记 arm 时间，**总时长硬上限 KEY_BLOCK_MAX_MS**，
 *      到点无论 16 位比较算出什么，一律强制解除；
 *   3) 超时解除是**带副作用的**（会把 s_block_armed 清掉），
 *      所以哪怕外部的调用逻辑再出花，键盘也不可能被永久封死。
 *========================================================================*/
#ifndef KEY_BLOCK_MAX_MS
#define KEY_BLOCK_MAX_MS     1500u       /* 屏蔽的绝对上限（正常只用 500us~500ms） */
#endif

/* 32 位毫秒（屏蔽的寿终判定用；key_now_ms() 是 16 位的，不能拿来算超时） */
static uint32_t key_now_ms32(void)
{
    if (s_ms_fn != 0)
    {
        return s_ms_fn();
    }
    s_fallback_ms = (uint16_t)(s_fallback_ms + KEY_SCAN_PERIOD_MS);
    return (uint32_t)s_fallback_ms;
}

void KEY_BlockInput(uint16_t ms)
{
    uint32_t now32 = key_now_ms32();

    if ((ms == 0u) || (ms > (uint16_t)KEY_BLOCK_MAX_MS))
    {
        ms = (uint16_t)KEY_BLOCK_MAX_MS;
    }

    /* 保险 1：已经在屏蔽里就不续期。页面 enter 被反复调用时，
     * 这里保证总时长不会被无限往后推。 */
    if ((s_block_armed != 0u) &&
        ((uint32_t)(now32 - s_block_at) < (uint32_t)KEY_BLOCK_MAX_MS))
    {
        return;
    }

    s_block_armed = 1u;
    s_block_at    = now32;
    s_block_until = (uint16_t)((uint16_t)now32 + ms);
}

bool KEY_IsBlocked(void)
{
    uint32_t now32;

    if (s_block_armed == 0u)
    {
        return false;
    }

    now32 = key_now_ms32();

    /* 保险 2/3：超过硬上限一律强制解除（并清标志），
     * 不管 16 位比较算出什么结果。 */
    if ((uint32_t)(now32 - s_block_at) >= (uint32_t)KEY_BLOCK_MAX_MS)
    {
        s_block_armed = 0u;
        return false;
    }

    return ((int16_t)(s_block_until - (uint16_t)now32) > 0) ? true : false;
}

/*==========================================================================
 * 【2026-10-18 性能优化】GPIO 采样节流
 *
 * 背景：Game_Task() 里原来那句 DelayMs(GAME_SCAN_MS)（1ms 忙等）被删掉之后，
 *       主循环变成"能跑多快跑多快"，KEY_Scan() 于是每圈都被调一次 ——
 *       实测一圈只有几百微秒，也就是每秒要扫上千次。
 *
 * 代价：key_read_raw() 每扫一次都要做 3 行 × KEY_ROW_SETTLE_US(30us)
 *       = **90us 的 RC 稳定等待**（DelayUs 是忙等，纯烧 CPU）。
 *       一圈几百微秒里有 90us 花在等按键电平上 —— 占比接近 90%，
 *       直接拖慢刷屏、USB 枚举轮询和 BLE 的 TMOS 调度。
 *
 * 而按键去抖窗是 KEY_DEBOUNCE_MS = 15ms、组合键窗 40ms，
 * 每 2ms 采一次（比原来假设的 5ms 还快）精度绰绰有余。
 *
 * 所以：距上次真正扫 GPIO 不足 KEY_SAMPLE_MIN_MS 就直接返回，
 *       连去抖累加都不做（时间戳 s_tick_ms 不动，下次一次补齐 dt，
 *       去抖/组合键的毫秒语义完全不变）。
 *
 * 没有注册外部时间源时（s_ms_fn == 0，单元测试/裸跑）**不节流**，
 * 保持"每次调用算 KEY_SCAN_PERIOD_MS 毫秒"的老行为。
 *========================================================================*/
#ifndef KEY_SAMPLE_MIN_MS
#define KEY_SAMPLE_MIN_MS   2u      /* 两次 GPIO 采样之间的最小间隔（毫秒） */
#endif

void KEY_Scan(void)
{
    uint16_t raw;
    uint16_t now;
    uint16_t dt;

    /* 【2026-10-13 修复：9 键间歇性锁死 —— 彻底版】
     *
     * 原来这里是：屏蔽期间【连 GPIO 都不扫】，raw 直接按 0 算。
     * 问题是 s_raw_stable 也跟着冻住 —— 一旦【屏蔽标志退不出来】，
     * **所有页面的 9 个矩阵键就全死**（编码器不经 KEY_Scan，所以它还好）。
     * 现场实测卡住时正是这个特征：R/L/S 全 0、D 已饱和、只有 B=1。
     *
     * 现在改成【永远照常扫描】：s_raw_stable 一直是活的，
     * 屏蔽只作用在【喂给按键层】那一侧（见 game_port.c 的 key_raw()）。
     * 这样即使屏蔽标志卡住，最坏也只是【键盘页暂时不吃键】，
     * 菜单 / 游戏 / 设置页的 9 键照常好用 —— 故障范围从【整机】缩到【一个页面】。
     *
     * 屏蔽的原始意图（进键盘页前 0.5 秒不把手上残留的键发出去）
     * 由 game_port.c 的 key_raw() 原样保留，行为不变。 */
    /* --- 取当前毫秒（有外部时间源就是真实毫秒）--- */
    now = key_now_ms();

    /* --- 【性能】采样节流：不到 KEY_SAMPLE_MIN_MS 就不碰 GPIO ---
     * 见上面 "GPIO 采样节流" 的说明。注意这里**不更新 s_tick_ms**，
     * 让下一次真正扫描时的 dt 把跳过的这段时间一并算进去。 */
    if (s_ms_fn != 0)
    {
        if ((uint16_t)(now - s_sample_tick) < (uint16_t)KEY_SAMPLE_MIN_MS)
        {
            return;
        }
    }
    s_sample_tick = now;

    raw = key_deghost(key_read_raw());      /* 先削掉幽灵键，再进去抖/组合键 */

    /* --- 真实毫秒：距上次扫描过了多少毫秒 ---
     * 16 位无符号回绕不影响差值（每 65.5s 绕一圈，差仍正确）。 */
    dt  = (uint16_t)(now - s_tick_ms);
    s_tick_ms = now;

    /* --- 去抖：同一幅图连续稳定 KEY_DEBOUNCE_MS 毫秒才算数 --- */
    if (raw == s_raw_last)
    {
        s_debounce_ms = (uint16_t)(s_debounce_ms + dt);
        if (s_debounce_ms > KEY_DEBOUNCE_MS)
        {
            s_debounce_ms = KEY_DEBOUNCE_MS;    /* 饱和，免得一直累到回绕 */
        }
    }
    else
    {
        s_raw_last    = raw;
        s_debounce_ms = 0u;
    }

    if ((s_debounce_ms >= KEY_DEBOUNCE_MS) && (raw != s_raw_stable))
    {
        s_raw_stable  = raw;
        s_change_tick = s_tick_ms;      /* 记录变化时刻（组合键窗口从这里算） */
    }

    /* --- 组合键窗口：稳定后 KEY_COMBO_WINDOW_MS 内没有新变化，才上报事件 --- */
    if ((s_raw_stable != s_reported) &&
        ((uint16_t)(s_tick_ms - s_change_tick) >= KEY_COMBO_WINDOW_MS))
    {
        s_event.map      = s_raw_stable;
        s_event.pressed  = (uint16_t)(s_raw_stable & (uint16_t)(~s_reported));
        s_event.released = (uint16_t)(s_reported & (uint16_t)(~s_raw_stable));
        s_event.count    = key_popcount(s_raw_stable);
        s_event.combo    = KEY_ComboLookup(s_raw_stable);
        s_event.ghost    = s_ghost_dropped;

        s_event_pending = true;
        s_reported      = s_raw_stable;
    }
}

/*==========================================================================
 * 事件 / 状态读取
 *========================================================================*/
bool KEY_GetEvent(KeyEvent_t *ev)
{
    if (!s_event_pending)
    {
        return false;
    }
    if (ev != 0)
    {
        *ev = s_event;
    }
    s_event_pending = false;
    return true;
}

uint16_t KEY_GetMap(void)
{
    return s_raw_stable;
}

uint8_t KEY_GetCount(void)
{
    return key_popcount(s_raw_stable);
}

bool KEY_IsPressed(uint8_t n)
{
    if ((n < 1u) || (n > 9u))
    {
        return false;
    }
    return ((s_raw_stable & KEY_BIT(n)) != 0u) ? true : false;
}

uint8_t KEY_ComboLookup(uint16_t map)
{
    static const struct
    {
        uint16_t map;
        uint8_t  id;
    } combos[] = KEY_COMBO_TABLE;
    uint8_t i;

    for (i = 0u; i < (uint8_t)(sizeof(combos) / sizeof(combos[0])); i++)
    {
        if (combos[i].map == map)
        {
            return combos[i].id;
        }
    }
    return 0u;
}

const char *KEY_MapToString(uint16_t map)
{
    static char    s_buf[3][24];        /* 轮换 3 个缓冲区，方便一句 PRINT 里多次调用 */
    static uint8_t s_buf_idx;
    char   *buf;
    uint8_t i;
    uint8_t n = 0u;

    buf = s_buf[s_buf_idx];
    s_buf_idx = (uint8_t)((s_buf_idx + 1u) % 3u);

    buf[0] = '\0';

    for (i = 0u; i < 9u; i++)
    {
        if ((map & (uint16_t)(1u << i)) == 0u)
        {
            continue;
        }
        if (n != 0u)
        {
            buf[n++] = '+';
        }
        buf[n++] = (char)('1' + i);     /* K1 -> '1' ... K9 -> '9' */
        if (n >= 22u)
        {
            break;
        }
    }

    if (n == 0u)
    {
        buf[n++] = '-';                 /* 没有键按下 */
    }
    buf[n] = '\0';

    return buf;
}


/*==========================================================================
 * 按键映射组（用旋转编码器切换）
 *
 * 默认映射表就在下面：想改默认值直接改这张表；
 * 运行时也可以用 KEY_SetMapValue() / KEY_SetMapRow() 改；
 * 想"动态计算"（比如按当前模式/蓝牙状态给不同的值），
 * 实现文件末尾那个 KEY_MapValueHook() 就行（弱符号，不用改这个文件）。
 *
 * 表里现在填的是 BLE HID 键码，方便直接当蓝牙键盘：
 *   0x1E~0x26 = 数字 1~9      0x04~0x1D = a~z        0x28 回车   0x29 ESC
 *   0x2C 空格                 0x3A~0x42 = F1~F9      0x4F/0x50 = 右/左
 *   0x51/0x52 = 下/上         0x59~0x61 = 小键盘 1~9 0x80/0x81 = 音量 +/- 
 * 不做键盘也没关系，填你自己的动作编号，怎么解释由你决定。
 *========================================================================*/
static const uint16_t s_map_default[KEY_MAP_GROUPS][KEY_MAP_KEYS] =
{
    /* 组 0：数字 1~9（默认组） */
    { 0x1Eu, 0x1Fu, 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u },

    /* 组 1：方向键 + 回车/ESC/空格/音量（K5 上、K8 下、K4 左、K6 右） */
    { 0x29u, 0x52u, 0x2Cu, 0x50u, 0x28u, 0x4Fu, 0x80u, 0x51u, 0x81u },

    /* 组 2：F1~F9 */
    { 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu, 0x40u, 0x41u, 0x42u },

    /* 组 3：自定义 1 —— 默认给"小键盘 1~9"，用户可在键值编辑页改 */
    { 0x59u, 0x5Au, 0x5Bu, 0x5Cu, 0x5Du, 0x5Eu, 0x5Fu, 0x60u, 0x61u },

    /* 组 4：自定义 2 —— 默认给"字母 a~i" */
    { 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu }
};

/* 组名（ASCII，给屏幕显示用；不够就自己加）。
 * 前 3 个是内置默认组，后 2 个是自定义组（名字也能从网页改，见 kmap_store）。 */
static const char *const s_map_name[KEY_MAP_GROUPS] =
{
    "123", "ARROW", "F1-F9", "NUM", "ABC"
};

static uint16_t s_map[KEY_MAP_GROUPS][KEY_MAP_KEYS];
static uint8_t  s_map_group;
static bool     s_map_ready;

/*==========================================================================
 * 编码器映射槽（2026-10-13 新增，需求：编码器正反转/单击也能设键值）
 *
 * 每组映射里另开 3 个槽：1 = 顺时针、2 = 逆时针、3 = 单击。
 * 默认值特意保持旧行为：单击 = 回车（0x28，和原来 UiKbd_PulseExtra 一样），
 * 正反转 = 0（不发键）。网页改完调 SAVE_FLASH 就写进 Flash（见 kmap_store）。
 *========================================================================*/
static const uint16_t s_enc_default[KEY_MAP_GROUPS][KEY_ENC_SLOTS] =
{
    { 0u, 0u, 0x28u },      /* 组 0：1~9 数字 —— 单击=回车 */
    { 0u, 0u, 0x28u },      /* 组 1：方向键     —— 单击=回车 */
    { 0u, 0u, 0x28u },      /* 组 2：F1~F9      —— 单击=回车 */
    { 0u, 0u, 0x28u },      /* 组 3：小键盘     —— 单击=回车 */
    { 0u, 0u, 0x28u }       /* 组 4：字母       —— 单击=回车 */
};

static uint16_t s_enc[KEY_MAP_GROUPS][KEY_ENC_SLOTS];

void KEY_ResetMap(void)
{
    uint8_t g, k;

    for (g = 0; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 0; k < (uint8_t)KEY_MAP_KEYS; k++)
        {
            s_map[g][k] = s_map_default[g][k];
        }
        for (k = 0; k < (uint8_t)KEY_ENC_SLOTS; k++)
        {
            s_enc[g][k] = s_enc_default[g][k];
        }
    }
    s_map_ready = true;
}

static void key_map_ensure(void)
{
    if (!s_map_ready)
    {
        KEY_ResetMap();
    }
}

__attribute__((weak)) uint16_t KEY_MapValueHook(uint8_t group, uint8_t key, uint16_t value)
{
    (void)group;
    (void)key;
    return value;               /* 默认：直接用表里的值 */
}

/*********************************************************************
 * @fn      KEY_MapGroupNameHook
 * @brief   组名钩子。s_map_name[] 是 const，运行时改不了；
 *          想让组名可改（网页在线改映射要用），就在这里加一个弱符号，
 *          由别处的同名强符号接管。见 keyboard/kmap_store.c 的实现。
 * @param   group  组号 0..(KEY_MAP_GROUPS-1)
 * @return  组名字符串；返回 0 表示"没有接管，用内置的 s_map_name"
 * @note    默认（弱符号）实现就是返回 0 —— 行为跟以前完全一样，
 *          不定义这个钩子的时候，key_matrix 的对外表现一个字节都没变。
 *********************************************************************/
__attribute__((weak)) const char *KEY_MapGroupNameHook(uint8_t group)
{
    (void)group;
    return 0;                   /* 0 = 不接管，走 s_map_name */
}

uint8_t KEY_GetMapGroup(void)
{
    key_map_ensure();
    return s_map_group;
}

void KEY_SetMapGroup(uint8_t group)
{
    key_map_ensure();
    if (group >= (uint8_t)KEY_MAP_GROUPS)
    {
        group = 0u;
    }
    s_map_group = group;
}

void KEY_MapGroupNext(void)
{
    KEY_SetMapGroup((uint8_t)((KEY_GetMapGroup() + 1u) % (uint8_t)KEY_MAP_GROUPS));
}

void KEY_MapGroupPrev(void)
{
    KEY_SetMapGroup((uint8_t)((KEY_GetMapGroup() + (uint8_t)KEY_MAP_GROUPS - 1u) % (uint8_t)KEY_MAP_GROUPS));
}

void KEY_MapGroupByEncoder(int8_t step)
{
    if (step > 0)
    {
        KEY_MapGroupNext();
    }
    else if (step < 0)
    {
        KEY_MapGroupPrev();
    }
    else
    {
        /* step == 0：什么也不做 */
    }
}

const char *KEY_GetMapGroupName(uint8_t group)
{
    const char *p;

    key_map_ensure();
    if (group >= (uint8_t)KEY_MAP_GROUPS)
    {
        group = 0u;
    }

    /* 先问钩子：有外部实现（比如 kmap_store 的 RAM 副本）就用它的，
     * 没有（默认弱符号返回 0）再退回 const 的内置表。
     * 这样"组名可改"是可选能力，不接的时候零影响。 */
    p = KEY_MapGroupNameHook(group);
    if (p != 0)
    {
        return p;
    }

    return s_map_name[group];
}

uint16_t KEY_GetMapValue(uint8_t group, uint8_t key)
{
    key_map_ensure();
    if (group >= (uint8_t)KEY_MAP_GROUPS)
    {
        group = 0u;
    }
    if (key < 1u || key > (uint8_t)KEY_MAP_KEYS)
    {
        return 0u;
    }
    return s_map[group][key - 1u];
}

uint16_t KEY_GetMapped(uint8_t key)
{
    key_map_ensure();
    if (key < 1u || key > (uint8_t)KEY_MAP_KEYS)
    {
        return 0u;
    }
    return KEY_MapValueHook(s_map_group, key, s_map[s_map_group][key - 1u]);
}

void KEY_SetMapValue(uint8_t group, uint8_t key, uint16_t value)
{
    key_map_ensure();
    if (group >= (uint8_t)KEY_MAP_GROUPS || key < 1u || key > (uint8_t)KEY_MAP_KEYS)
    {
        return;
    }
    s_map[group][key - 1u] = value;
}

void KEY_SetMapRow(uint8_t group, const uint16_t *values)
{
    uint8_t k;

    key_map_ensure();
    if (group >= (uint8_t)KEY_MAP_GROUPS || values == 0)
    {
        return;
    }
    for (k = 0; k < (uint8_t)KEY_MAP_KEYS; k++)
    {
        s_map[group][k] = values[k];
    }
}

/*==========================================================================
 * 编码器映射（正转 / 反转 / 单击）—— 2026-10-13 新增
 *
 * which: KEY_ENC_CW(1) / KEY_ENC_CCW(2) / KEY_ENC_CLICK(3)
 * 网页协议（kmap_proto）：0x15 读一组、0x16 写一个槽，改完 0x30 存 Flash。
 *========================================================================*/
uint16_t KEY_GetEncMapValue(uint8_t group, uint8_t which)
{
    key_map_ensure();

    if ((group >= (uint8_t)KEY_MAP_GROUPS) || (which < 1u) || (which > (uint8_t)KEY_ENC_SLOTS))
    {
        return 0u;
    }
    return s_enc[group][which - 1u];
}

uint16_t KEY_GetEncMapped(uint8_t which)
{
    key_map_ensure();
    return KEY_GetEncMapValue(s_map_group, which);
}

void KEY_SetEncMapValue(uint8_t group, uint8_t which, uint16_t value)
{
    key_map_ensure();

    if ((group >= (uint8_t)KEY_MAP_GROUPS) || (which < 1u) || (which > (uint8_t)KEY_ENC_SLOTS))
    {
        return;
    }
    s_enc[group][which - 1u] = value;
}

/*==========================================================================
 * 按键 -> 行列（屏上显示用；行列按"逻辑键号"算，K1=行1列1）
 *========================================================================*/
uint8_t KEY_GetRow(uint8_t n)
{
    if (n < 1u || n > 9u) { return 0u; }
    return (uint8_t)((n - 1u) / 3u + 1u);
}

uint8_t KEY_GetCol(uint8_t n)
{
    if (n < 1u || n > 9u) { return 0u; }
    return (uint8_t)((n - 1u) % 3u + 1u);
}
