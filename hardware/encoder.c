/**************************************************************************
 * encoder.c —— CH592X 带按键旋转编码器驱动（正交状态机，抗抖动）
 *
 * 核心：正交状态机
 *   A/B 电平组成 2 位状态 (A<<1)|B，状态之间合法跳变按下表累加：
 *
 *        prev\cur   00   01   11   10
 *          00       0   +1   X   -1        X = 非法跳变（跳过了中间状态）算 0，
 *          01      -1    0   +1   X          抖动产生的来回跳变会自己抵消，
 *          11       X   -1    0   +1         所以不需要额外延时消抖
 *          10      +1    X   -1    0
 *
 *   累加满 ENC_DETENT_COUNT（EC11 通常 4）就是一个"格"：
 *       顺时针 -> +1，逆时针 -> -1
 *   这样既不会"一格报 4 次"，也不怕机械触点抖动。
 **************************************************************************/

#include "encoder.h"

/*==========================================================================
 * 正交跳变表：index = (prev << 2) | cur
 *========================================================================*/
static const int8_t s_quad_table[16] = {
     0,  1, -1,  0,
    -1,  0,  0,  1,
     1,  0,  0, -1,
     0, -1,  1,  0
};

/*==========================================================================
 * 内部状态
 *========================================================================*/
/* 【注意】ENC_Scan() 只在 TMR0 的 1ms 中断里调用（见 Main.c），
 * 所以下面这些"中断改、主循环读"的变量必须 volatile，否则编译器可能
 * 把主循环里的读缓存起来（一直读到老值）。 */
static uint8_t  s_prev_ab;              /* 上一次的 (A<<1)|B 状态（只在中断里用） */
static int8_t   s_acc;                  /* 不足一格的零头（只在中断里用） */
static volatile int16_t  s_steps;       /* 还没被取走的格数 */
static volatile int16_t  s_position;    /* 累计位置 */

static volatile uint8_t  s_btn_stable;  /* 消抖后的按键状态：1 = 按下 */
static uint8_t  s_btn_cnt;              /* 消抖计数（只在中断里用） */
static volatile uint8_t  s_btn_event;   /* 1 = 刚按下，2 = 刚松开 */

/*==========================================================================
 * 引脚读写
 *========================================================================*/
static uint8_t enc_pin_read(uint8_t portb, uint32_t mask)
{
    uint32_t v = portb ? GPIOB_ReadPortPin(mask) : GPIOA_ReadPortPin(mask);
    return (v != 0u) ? 1u : 0u;
}

/* A/B 相：接成"闭合到地"时读到低电平，这里换算成逻辑 1 */
static uint8_t enc_read_ab(void)
{
    uint8_t a = enc_pin_read(ENC_A_PORTB, ENC_A_PIN);
    uint8_t b = enc_pin_read(ENC_B_PORTB, ENC_B_PIN);

#if (ENC_AB_ACTIVE_LOW)
    a = (uint8_t)((a != 0u) ? 0u : 1u);
    b = (uint8_t)((b != 0u) ? 0u : 1u);
#endif

    return (uint8_t)((a << 1) | b);
}

/* 按键：换算成"逻辑 1 = 按下" */
static uint8_t enc_read_btn(void)
{
    uint8_t v = enc_pin_read(ENC_BTN_PORTB, ENC_BTN_PIN);

#if (ENC_BTN_ACTIVE_LOW)
    v = (uint8_t)((v != 0u) ? 0u : 1u);
#endif

    return v;
}

/*==========================================================================
 * 正交状态机（ENC_Scan 和 ENC_HandleIRQ 共用）
 *========================================================================*/
static void enc_update_ab(void)
{
    uint8_t ab = enc_read_ab();
    int8_t  d;

    if (ab == s_prev_ab)
    {
        return;
    }

    d = s_quad_table[(uint8_t)((s_prev_ab << 2) | ab)];
    s_prev_ab = ab;

#if (ENC_DIR_INVERT)
    d = (int8_t)(-d);
#endif

    if (d == 0)
    {
        return;                     /* 非法跳变（抖动/干扰），忽略 */
    }

    s_acc = (int8_t)(s_acc + d);

    if (s_acc >= ENC_DETENT_COUNT)
    {
        s_acc = (int8_t)(s_acc - ENC_DETENT_COUNT);
        s_steps++;
        s_position++;
    }
    else if (s_acc <= -ENC_DETENT_COUNT)
    {
        s_acc = (int8_t)(s_acc + ENC_DETENT_COUNT);
        s_steps--;
        s_position--;
    }
}

/*==========================================================================
 * 初始化
 *========================================================================*/
void ENC_Init(void)
{
    /* A/B 相：机械触点式编码器一般接 GND，所以用上拉输入；
     * 若你的板子是"接 VCC"的接法，把 ENC_AB_ACTIVE_LOW 改成 0（用下拉输入） */
#if (ENC_AB_ACTIVE_LOW)
    if (ENC_A_PORTB) { GPIOB_ModeCfg(ENC_A_PIN, GPIO_ModeIN_PU); }
    else             { GPIOA_ModeCfg(ENC_A_PIN, GPIO_ModeIN_PU); }
    if (ENC_B_PORTB) { GPIOB_ModeCfg(ENC_B_PIN, GPIO_ModeIN_PU); }
    else             { GPIOA_ModeCfg(ENC_B_PIN, GPIO_ModeIN_PU); }
#else
    if (ENC_A_PORTB) { GPIOB_ModeCfg(ENC_A_PIN, GPIO_ModeIN_PD); }
    else             { GPIOA_ModeCfg(ENC_A_PIN, GPIO_ModeIN_PD); }
    if (ENC_B_PORTB) { GPIOB_ModeCfg(ENC_B_PIN, GPIO_ModeIN_PD); }
    else             { GPIOA_ModeCfg(ENC_B_PIN, GPIO_ModeIN_PD); }
#endif

    /* 按键 */
#if (ENC_BTN_ACTIVE_LOW)
    if (ENC_BTN_PORTB) { GPIOB_ModeCfg(ENC_BTN_PIN, GPIO_ModeIN_PU); }
    else               { GPIOA_ModeCfg(ENC_BTN_PIN, GPIO_ModeIN_PU); }
#else
    if (ENC_BTN_PORTB) { GPIOB_ModeCfg(ENC_BTN_PIN, GPIO_ModeIN_PD); }
    else               { GPIOA_ModeCfg(ENC_BTN_PIN, GPIO_ModeIN_PD); }
#endif

    /* 状态复位：把当前 A/B 当作起点，避免上电瞬间被当成转动 */
    s_prev_ab  = enc_read_ab();
    s_acc      = 0;
    s_steps    = 0;
    s_position = 0;

    s_btn_stable = enc_read_btn();
    s_btn_cnt    = 0;
    s_btn_event  = 0;
}

/*==========================================================================
 * 扫描：每 ENC_SCAN_MS（默认 1ms）调用一次
 *========================================================================*/
/*********************************************************************
 * @fn      ENC_Scan
 * @brief   扫一次：正交状态机 + 按键消抖
 * @note    **只在 TMR0 的 1ms 中断里调用**（Main.c 的 TMR0_IRQHandler）。
 *          主循环再调一次会重复计步，而且和中断并发读写没有保护。
 *********************************************************************/
void ENC_Scan(void)
{
    uint8_t raw;

    /* ---- 1. A/B 正交状态机 ---- */
    enc_update_ab();

    /* ---- 2. 按键消抖 ---- */
    raw = enc_read_btn();

    if (raw == s_btn_stable)
    {
        s_btn_cnt = 0;
    }
    else
    {
        s_btn_cnt++;
        if (s_btn_cnt >= (ENC_BTN_DEBOUNCE_MS / ENC_SCAN_MS))
        {
            s_btn_stable = raw;
            s_btn_cnt    = 0;
            s_btn_event  = (raw != 0u) ? 1u : 2u;   /* 1 = 刚按下，2 = 刚松开 */
        }
    }
}

/*==========================================================================
 * 如果你自己配了 A/B 引脚中断，可以在中断里调用它（代替 ENC_Scan 的 A/B 部分）
 *========================================================================*/
void ENC_HandleIRQ(void)
{
    enc_update_ab();
}

/*==========================================================================
 * 读取接口
 *========================================================================*/
/*********************************************************************
 * @fn      ENC_GetStep
 * @brief   【要求 2】取走累计格数：正转 +1，反转 -1，没转动 0
 *********************************************************************/
int8_t ENC_GetStep(void)
{
    int16_t v;

    if (s_steps == 0)
    {
        return 0;
    }

    v = s_steps;

    /* 先夹到 int8_t 范围，再按"夹过的量"消费：
     *   * 转得再快，多出来的格子留到下次取，不会丢
     *   * 只减掉本次取走的量（而不是写 0），1ms 中断期间新加的格子也保得住 */
    if (v > 127)
    {
        v = 127;
    }
    if (v < -128)
    {
        v = -128;
    }

    s_steps = (int16_t)(s_steps - v);

    return (int8_t)v;
}

/*********************************************************************
 * @fn      ENC_ButtonPressed
 * @brief   【要求 1】按键是否按下：按下 1，松开 0（已消抖）
 *********************************************************************/
uint8_t ENC_ButtonPressed(void)
{
    return s_btn_stable;
}

uint8_t ENC_GetButtonEvent(void)
{
    uint8_t e = s_btn_event;
    s_btn_event = 0;
    return e;
}

void ENC_ReadEvent(EncEvent_t *ev)
{
    if (ev == 0)
    {
        return;
    }
    ev->step      = ENC_GetStep();
    ev->btn       = s_btn_stable;
    ev->btn_event = ENC_GetButtonEvent();
}

int16_t ENC_GetPosition(void)
{
    return s_position;
}

void ENC_SetPosition(int16_t pos)
{
    s_position = pos;
}

uint8_t ENC_GetRawAB(void)
{
    return enc_read_ab();
}

uint8_t ENC_GetRawButton(void)
{
    return enc_pin_read(ENC_BTN_PORTB, ENC_BTN_PIN);
}

/******************* (C) CH592X 旋转编码器 *****************/
