/**************************************************************************
 * rgb_led.c —— CH592X 键盘 RGB 灯效驱动（WS2812 类灯珠，PWM9 输出）
 *
 * 原理：
 *   WS2812 的"位"用高电平宽度区分（0.4us=0，0.8us=1），位周期 1.25us。
 *   这里用 PWM9(PB7) 的 **16 位模式**产生波形：
 *       PWM 基准时钟 = 1*Tsys（60MHz 时 1 计数 = 16.67ns）
 *       周期 = 75 计数 = 1.25us
 *       占空比 = 24 计数(0.4us) 表示 '0'，48 计数(0.8us) 表示 '1'
 *   每发一位前更新一次占空比寄存器（R16_PWM9_DATA），并等这一位的 PWM
 *   周期结束（轮询周期结束标志），这样高低电平边沿完全由硬件产生，
 *   不受软件抖动影响，比纯 GPIO 翻转稳得多。
 *
 * 一帧 = 9 颗 × 24 位（G-R-B 顺序）× 1.25us ≈ 270us，之后保持低电平 >50us 复位。
 **************************************************************************/

#include "rgb_led.h"

/*==========================================================================
 * 内部数据
 *========================================================================*/
static uint8_t  s_buf[RGB_LED_COUNT * 3];   /* 每颗灯 3 字节：R,G,B（物理序号排列） */
static uint16_t s_key_mask;                 /* 哪些灯要显示"按键白"（bit0~bit8，逻辑键位） */
static uint8_t  s_effect;                   /* 当前灯效 */
static uint8_t  s_on;                       /* 灯效总开关 */
static uint16_t s_phase;                    /* 动画相位（每 20ms +1） */

/*==========================================================================
 * 引脚
 *========================================================================*/
static void rgb_en_write(uint8_t level)
{
    if (RGB_EN_PORTB)
    {
        if (level) { GPIOB_SetBits(RGB_EN_PIN); } else { GPIOB_ResetBits(RGB_EN_PIN); }
    }
    else
    {
        if (level) { GPIOA_SetBits(RGB_EN_PIN); } else { GPIOA_ResetBits(RGB_EN_PIN); }
    }
}

/*==========================================================================
 * 逻辑(行,列) -> 物理序号（蛇形：第 1 行左->右，第 2 行右->左，第 3 行左->右…）
 *========================================================================*/
uint8_t RGB_PhysIndex(uint8_t row, uint8_t col)
{
    uint8_t c;

    if (row >= RGB_LED_ROWS) { row = (uint8_t)(RGB_LED_ROWS - 1u); }
    if (col >= RGB_LED_COLS) { col = (uint8_t)(RGB_LED_COLS - 1u); }

    c = ((row & 1u) != 0u) ? (uint8_t)(RGB_LED_COLS - 1u - col) : col;
    return (uint8_t)(row * RGB_LED_COLS + c);
}

/*==========================================================================
 * 缓冲区操作
 *========================================================================*/
void RGB_SetPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= RGB_LED_COUNT)
    {
        return;
    }
    s_buf[index * 3u + 0u] = r;
    s_buf[index * 3u + 1u] = g;
    s_buf[index * 3u + 2u] = b;
}

void RGB_SetAll(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t i;
    for (i = 0; i < RGB_LED_COUNT; i++)
    {
        RGB_SetPixel(i, r, g, b);
    }
}

void RGB_Clear(void)
{
    RGB_SetAll(0u, 0u, 0u);
}

uint8_t *RGB_GetBuffer(void)
{
    return s_buf;
}

/*==========================================================================
 * HSV -> RGB（彩虹用，h/s/v 都是 0~255）
 *========================================================================*/
static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = (uint8_t)(h / 43u);                 /* 0~5 */
    uint8_t rem    = (uint8_t)((uint8_t)(h - region * 43u) * 6u);   /* 0~255 */
    uint8_t p      = (uint8_t)(((uint16_t)v * (255u - s)) >> 8);
    uint8_t q      = (uint8_t)(((uint16_t)v * (255u - (uint8_t)(((uint16_t)s * rem) >> 8))) >> 8);
    uint8_t t      = (uint8_t)(((uint16_t)v * (255u - (uint8_t)(((uint16_t)s * (255u - rem)) >> 8))) >> 8);

    switch (region)
    {
        case 0u:  *r = v; *g = t; *b = p; break;
        case 1u:  *r = q; *g = v; *b = p; break;
        case 2u:  *r = p; *g = v; *b = t; break;
        case 3u:  *r = p; *g = q; *b = v; break;
        case 4u:  *r = t; *g = p; *b = v; break;
        default:  *r = v; *g = p; *b = q; break;
    }
}

/*==========================================================================
 * PWM9(PB7) 初始化
 *========================================================================*/
/*==========================================================================
 * 亮度调节（等比缩放 RGB）
 *   0~100，发送时把每个字节按 值*亮度/100 缩放 —— 三种颜色比例不变，
 *   所以颜色不会偏，只是整体变亮/变暗；0 = 全灭，100 = 原样。
 *   为什么放在发送这一层：效果算出来的颜色不用改，调亮度不用重算灯效 ?
 *========================================================================*/
static uint8_t s_bright = RGB_BRIGHT_MAX;      /* 默认 100% */

static uint8_t rgb_scale(uint8_t v)
{
    if (s_bright >= (uint8_t)RGB_BRIGHT_MAX) { return v; }        /* 100% 直接原样 */
    return (uint8_t)(((uint16_t)v * (uint16_t)s_bright + 50u) / (uint16_t)RGB_BRIGHT_MAX);
}

void RGB_SetBrightness(uint8_t level)
{
    if (level > (uint8_t)RGB_BRIGHT_MAX) { level = (uint8_t)RGB_BRIGHT_MAX; }
    s_bright = level;
    RGB_Show();                      /* 立刻刷新一帧，马上能看到变化 */
}

uint8_t RGB_GetBrightness(void) { return s_bright; }

void RGB_BrightnessUp(uint8_t step)
{
    if (step == 0u) { step = 10u; }
    RGB_SetBrightness((uint8_t)((s_bright + step > (uint8_t)RGB_BRIGHT_MAX)
                                    ? (uint8_t)RGB_BRIGHT_MAX : (uint8_t)(s_bright + step)));
}

void RGB_BrightnessDown(uint8_t step)
{
    if (step == 0u) { step = 10u; }
    RGB_SetBrightness((step > s_bright) ? 0u : (uint8_t)(s_bright - step));
}

/*==========================================================================
 * DIN 输出：GPIO 位翻转（不再借用 PWM9）
 *
 * 【为什么从 PWM 改回 GPIO】
 *   原来用 PWM9 的 16 位模式产生波形，靠轮询 R8_PWM_INT_CTRL 的周期结束标志
 *   RB_PWM_IF_CYC 来做"一位"的同步。但那个标志的比较点是按**模式位宽**固定的：
 *       SFR 原文：0 = after count 0xFE (0x7E for 7 bits mode...)
 *                 1 = after count 0xF0 (0x70 for 7 bits mode...)
 *   8 位模式比 254、7 位模式比 126 —— 也就是"满量程 - 2"。
 *   而 16 位模式下这个点落在 0xFFFE，位周期却只有 75 计数（1.25us），
 *   计数器永远到不了 0xFFFE ——**标志一次都不会置位**。
 *   结果每发一位都把 rgb_wait_bit() 里的 guard 自旋跑满（十万圈 ≈ 十几毫秒），
 *   相邻两位之间隔了十几毫秒的低电平；WS2812B 把 >280us 的低电平当作复位，
 *   于是每一位都被当成"一帧的结束" —— 数据永远进不了锁存器，
 *   **灯珠一颗都不亮**。这就是"键盘灯完全不亮"的根因。
 *
 *   另外 PB4(BLK/PWM7) 和 PB7(DIN/PWM9) 在**同一个 PWM 组**里，共用
 *   R32_PWM_REG_CYCLE 与 R8_PWM_CONFIG —— 5kHz 的屏幕 PWM 和 1.25us 的
 *   灯珠 PWM 本来就无法共存，所以 DIN 必须让出 PWM。
 *
 * 【现在的做法】纯 GPIO 翻转 + 关中断的定长延时，严格照数据手册取值：
 *       T0H = 0.40us ±150ns     24 计数 @60MHz
 *       T1H = 0.80us ±150ns     48 计数
 *       T0L = 0.85us ±150ns
 *       T1L = 0.45us ±150ns
 *       位周期 1.25us ±600ns    75 计数
 *       复位   >280us
 *   位内用 R32_PB_OUT / R32_PB_CLR 直写寄存器（各 1~3 条指令，不经过
 *   GPIOB_SetBits() 那样的函数调用），再用定长 nop 凑够周期数。
 *   一帧 216 位 × 1.25us ≈ 270us，期间关中断。
 *========================================================================*/

/* DIN = PB7。直写寄存器别名：
 *   R32_PB_OUT  RW  置 1 是读-改-写（3 条指令）
 *   R32_PB_CLR  WZ  写 1 清 0（1 条 store） */
#define RGB_DIN_MASK    ((uint32_t)RGB_DIN_PIN)

static void rgb_din_hi(void) { R32_PB_OUT |=  RGB_DIN_MASK; }
static void rgb_din_lo(void) { R32_PB_CLR  =  RGB_DIN_MASK; }

/* 定长延时：n 是"要消耗的 CPU 周期数"。
 * 循环体在 -Os 下是 addi + blt + nop ≈ 3 周期一圈，所以圈数 = 周期数/3。
 * 这几个数就是 WS2812B 的时序预算，实测偏了只需在这里微调
 * （容差 ±150ns = ±9 个周期，调整余量很大）。 */
/*==========================================================================
 * 【2026-10-14 关键回退：彻底不再依赖 SysTick】
 *
 * 前一版这里用 SysTick->CNT 当精确时基，结果引出两个大问题：
 *   1) SysTick 在【进键盘页之前根本没被使能】（只在 CH59x_BLEInit() 里
 *      才 SysTick_Config 一次），开机后它冻在 0 —— 当时靠 RGB_Init()
 *      自己调 SysTick_Config() 补救，但那个函数**会打开 SysTick 中断**，
 *      于是留下一个永远没人清的挂起标志；等 CH59x_BLEInit() 里再次
 *      PFIC_EnableIRQ 时它立刻被投递 —— 而全工程没有 SysTick 处理函数，
 *      **直接跑飞，屏幕停在 BT step 1**。
 *   2) 更要命的是：一旦 CNT 读数不正常，rgb_wait_until() 里的兜底
 *      guard(4000) 会被每次都跑满 —— 216 位 × 2 段 × 4000 圈 ≈ 1.7M 次
 *      循环，而且**整段是关中断做的**，一帧就能阻塞上百毫秒。
 *      主循环被拖死，表现同样是"卡死"。
 *
 * 结论：WS2812 的位时序**不要**去借任何外设当时基。
 * 这里换回纯 nop 循环 —— 不读任何寄存器、不依赖任何初始化顺序，
 * 60MHz 下 1 个 nop 就是 1 个周期，行为完全可预测。
 *
 * 代价是精度靠"数指令"，比硬件差；但灯珠本来就要走 TMR1+DMA 方案
 * （需要把行1 挪到 PB7、DIN 挪到 PA10），到那时这套软件延时会被整体换掉。
 * 现在首要目标是**别再让灯珠驱动拖死系统**。
 *========================================================================*/
static void rgb_delay_cycles(uint32_t cycles)
{
    /* 循环体：sub + bne ≈ 2 周期，加上进入时的开销按 3 周期估 */
    uint32_t n = (cycles + 2u) / 3u;

    while (n != 0u)
    {
        __nop();
        n--;
    }
}
static void rgb_din_gpio_init(void)
{
    GPIOB_ModeCfg(RGB_DIN_PIN, GPIO_ModeOut_PP_5mA);
    rgb_din_lo();                       /* 空闲低电平 */
}

/* 发一位。high_cycles = T0H 或 T1H；整位固定 75 周期。 */
static void rgb_send_bit(uint8_t one, uint8_t first)
{
    (void)first;

    rgb_din_hi();

    /* 高电平：T0H=26 / T1H=52 计数（文档 §12 的 CCR 值）。
     * 扣掉 store 与调用开销约 3 周期 */
    rgb_delay_cycles((one != 0u) ? (uint32_t)(RGB_T1H_TICKS - 3u)
                                 : (uint32_t)(RGB_T0H_TICKS - 3u));

    rgb_din_lo();

    /* 低电平：把整位补满 75 计数 */
    rgb_delay_cycles((one != 0u) ? (uint32_t)(RGB_BIT_TICKS - RGB_T1H_TICKS - 3u)
                                 : (uint32_t)(RGB_BIT_TICKS - RGB_T0H_TICKS - 3u));
}
/* 发一个字节（MSB 先行） */
static void rgb_send_byte(uint8_t dat)
{
    uint8_t i;

    for (i = 0u; i < 8u; i++)
    {
        rgb_send_bit((uint8_t)((dat & 0x80u) != 0u), 0u);
        dat = (uint8_t)(dat << 1);
    }
}

/*==========================================================================
 * 把缓冲区发出去（WS2812 顺序 = GRB）
 *========================================================================*/
void RGB_Show(void)
{
    uint8_t  i;
    uint8_t *p;

    if (s_on == 0u)
    {
        return;                         /* 关灯状态下不发数据 */
    }

    /*==========================================================================
     * 【2026-10-14 修复：蓝牙疯狂连接断开 —— 真凶就是这里】
     *
     * 原来整帧（27 字节 ≈ 270us）都在**关中断**里发，而本函数每 20ms 跑一次
     * （LED_TASK_MS）—— 也就是每 20ms 就有 300us 中断全屏蔽。
     *
     * 对 BLE 是致命的：连接间隔只有 7.5~11.25ms，连接事件窗口约 150us。
     * 300us 的屏蔽窗口足以**整个错过一次连接事件**；连续错过到
     * 监督超时（BTWCH_CONN_TIMEOUT = 300 x 10ms = **3 秒**）就断链，
     * 主机立刻重连、再断 …… 现场就是【疯狂地连接/断开】，
     * 而且是**3 秒一个循环**。
     *
     * 注意：这个毛病从最早的 PWM9 版本就存在（那时也是整帧关中断），
     * 所以它一直是蓝牙不稳的真正原因，只是每次都被当成别的问题在查。
     *
     * 修法：**按字节关中断**。WS2812 的位周期是 1.25us，一个字节只有 10us，
     * 关中断窗口从 270us 降到 **10us（27 倍改善）**；字节之间放开一下，
     * 那点间隙（几微秒）远小于 WS2812 允许的 50us 帧内空洞，协议上安全。
     *
     * 残余风险：如果两个字节之间正好被一个**很长的**中断（比如 BLE 的 LLE）
     * 插进来，那一次间隙可能超过 50us，**这一帧灯珠数据会错位**。
     * 但灯珠的重置只有视觉影响，**蓝牙不能断** —— 这个取舍是对的。
     * 彻底解决要等 TMR1+DMA 方案（需把矩阵行1 挪到 PB7、灯珠 DIN 挪到 PA10）。
     *=======================================================================*/
    rgb_din_lo();                       /* 拉低起帧（保证帧前有低电平） */

    for (i = 0u; i < RGB_LED_COUNT; i++)
    {
        p = &s_buf[i * 3u];

#if (RGB_TX_IRQ_LOCK)
        {
            /* 一个字节 = 8 bit x 1.25us = 10us 不许被打断 */
            uint32_t intlev = __risc_v_disable_irq();
            rgb_send_byte(rgb_scale(p[1]));     /* G（等比缩放） */
            __risc_v_enable_irq(intlev);
        }
        {
            uint32_t intlev = __risc_v_disable_irq();
            rgb_send_byte(rgb_scale(p[0]));     /* R */
            __risc_v_enable_irq(intlev);
        }
        {
            uint32_t intlev = __risc_v_disable_irq();
            rgb_send_byte(rgb_scale(p[2]));     /* B */
            __risc_v_enable_irq(intlev);
        }
#else
        rgb_send_byte(rgb_scale(p[1]));
        rgb_send_byte(rgb_scale(p[0]));
        rgb_send_byte(rgb_scale(p[2]));
#endif
    }

    rgb_din_lo();                       /* 之后线一直保持低电平 */

    DelayUs(RGB_RESET_US);              /* 帧间复位（>50us 低电平） */
}

/*==========================================================================
 * 渲染当前灯效到缓冲区（并把"按键白"覆盖上去）
 *========================================================================*/
static void rgb_render_white_overlay(void)
{
    uint8_t  i;
    uint8_t  w = RGB_MAX_BRIGHT;
    uint8_t  row;
    uint8_t  col;

    /* s_key_mask 的 bit0~bit8 是 K1~K9（从上到下、每排从左到右） */
    for (i = 0u; i < RGB_LED_COUNT; i++)
    {
        if ((s_key_mask & (uint16_t)(1u << i)) != 0u)
        {
            row = (uint8_t)(i / RGB_LED_COLS);
            col = (uint8_t)(i % RGB_LED_COLS);
            RGB_SetPixel(RGB_PhysIndex(row, col), w, w, w);      /* 白色 */
        }
    }
}

static void rgb_render(void)
{
    uint8_t  i;
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  bright;
    uint8_t  head;
    uint16_t p;

    switch (s_effect)
    {
        case RGB_EFF_WHITE:                                     /* 全白常亮 */
            RGB_SetAll(RGB_MAX_BRIGHT, RGB_MAX_BRIGHT, RGB_MAX_BRIGHT);
            break;

        case RGB_EFF_RED_BREATH:                                /* 红色呼吸 */
            p = (uint16_t)(s_phase & 0x7Fu);                    /* 0~127，约 1.28s 半个周期 */
            bright = (uint8_t)((p < 64u) ? (p * 2u) : ((127u - p) * 2u));
            bright = (uint8_t)(((uint16_t)bright * RGB_MAX_BRIGHT) / 128u);
            RGB_SetAll(bright, 0u, 0u);
            break;

        case RGB_EFF_RAINBOW:                                   /* 彩虹流动 */
            for (i = 0u; i < RGB_LED_COUNT; i++)
            {
                hsv_to_rgb((uint8_t)(s_phase * 3u + i * 24u), 255u, RGB_MAX_BRIGHT, &r, &g, &b);
                RGB_SetPixel(i, r, g, b);
            }
            break;

        case RGB_EFF_RUNNING:                                   /* 跑马灯（按蛇形顺序） */
            RGB_SetAll(0u, 0u, 0u);
            i = (uint8_t)((s_phase / 4u) % RGB_LED_COUNT);      /* 每 80ms 走一颗 */
            RGB_SetPixel(i, RGB_MAX_BRIGHT, RGB_MAX_BRIGHT, RGB_MAX_BRIGHT);
            break;

        case RGB_EFF_BLINK:                                     /* 整屏闪烁（约 1Hz） */
            bright = (((s_phase / 25u) & 1u) != 0u) ? RGB_MAX_BRIGHT : 0u;
            RGB_SetAll(bright, bright, bright);
            break;

        case RGB_EFF_KEY_WHITE_RAIN:                            /* 按键盘 + 彩虹底 */
            for (i = 0u; i < RGB_LED_COUNT; i++)
            {
                uint8_t dim = (uint8_t)(RGB_MAX_BRIGHT / 4u);
                hsv_to_rgb((uint8_t)(s_phase * 3u + i * 24u), 255u, dim, &r, &g, &b);
                RGB_SetPixel(i, r, g, b);
            }
            break;

        /* ---- 3x3 预置灯效 ---- */

        case RGB_EFF_FX_BREATH:                                 /* 呼吸：整盘青蓝渐变 */
            p = (uint16_t)(s_phase & 0x7Fu);                    /* 0..127，约 2.56s 一周期 */
            bright = (uint8_t)((p < 64u) ? (p * 2u) : ((127u - p) * 2u));
            bright = (uint8_t)(((uint16_t)bright * RGB_MAX_BRIGHT) / 128u);
            RGB_SetAll((uint8_t)(bright / 4u), bright, bright);
            break;

        case RGB_EFF_FX_CHASE:                                  /* 跑马：3 颗拖尾沿蛇形跑 */
            head = (uint8_t)((s_phase / 3u) % RGB_LED_COUNT);   /* 每 60ms 走一颗 */
            for (i = 0u; i < RGB_LED_COUNT; i++)
            {
                uint8_t d  = (uint8_t)((head + RGB_LED_COUNT - i) % RGB_LED_COUNT);
                uint8_t lv = (uint8_t)((d < 3u) ? (((3u - d) * RGB_MAX_BRIGHT) / 3u) : 0u);
                RGB_SetPixel(i, lv, lv, lv);
            }
            break;

        case RGB_EFF_FX_RIPPLE:                                 /* 涟漪：从中心往外扩 */
            bright = (uint8_t)((s_phase / 5u) % 3u);            /* 每 100ms 换一圈 */
            for (i = 0u; i < RGB_LED_COUNT; i++)
            {
                uint8_t rr = (uint8_t)(i / RGB_LED_COLS);
                uint8_t cc = (uint8_t)(i % RGB_LED_COLS);
                /* 到中心 (1,1) 的曼哈顿距离：0=中心 1=上下左右 2=四角 */
                uint8_t dd = (uint8_t)(((rr > 1u) ? (rr - 1u) : (1u - rr)) +
                                       ((cc > 1u) ? (cc - 1u) : (1u - cc)));
                uint8_t lv;
                if (dd == bright)
                {
                    lv = RGB_MAX_BRIGHT;
                }
                else if (dd == (uint8_t)((bright + 1u) % 3u))
                {
                    lv = (uint8_t)(RGB_MAX_BRIGHT / 6u);        /* 余晖 */
                }
                else
                {
                    lv = 0u;
                }
                RGB_SetPixel(RGB_PhysIndex(rr, cc), 0u, lv, lv);
            }
            break;

        case RGB_EFF_FX_WAVE:                                   /* 波浪：蓝色波峰沿灯带扫过 */
            for (i = 0u; i < RGB_LED_COUNT; i++)
            {
                uint16_t ph = (uint16_t)((s_phase + (uint16_t)(i * 6u)) & 0x1Fu);
                uint8_t  lv = (uint8_t)((ph < 16u) ? (ph * 8u) : ((31u - ph) * 8u));
                lv = (uint8_t)(((uint16_t)lv * RGB_MAX_BRIGHT) / 128u);
                RGB_SetPixel(i, 0u, (uint8_t)(lv / 2u), lv);
            }
            break;

        case RGB_EFF_FX_RAINBOW:                                /* 彩虹：整盘流动 */
            for (i = 0u; i < RGB_LED_COUNT; i++)
            {
                hsv_to_rgb((uint8_t)(s_phase * 3u + (uint16_t)(i * 28u)),
                           255u, RGB_MAX_BRIGHT, &r, &g, &b);
                RGB_SetPixel(i, r, g, b);
            }
            break;

        case RGB_EFF_KEY_WHITE:                                 /* 只有按下的键亮白 */
        case RGB_EFF_OFF:
        default:
            RGB_SetAll(0u, 0u, 0u);
            break;
    }

    /* 按下的键始终覆盖成白色（这样任何灯效下按键盘都能看到白灯） */
    rgb_render_white_overlay();
}

/*==========================================================================
 * 功能 1：灯效总开关
 *========================================================================*/
void RGB_Switch(uint8_t on)
{
    if (on)
    {
        s_on = 1u;
        rgb_en_write(1);                /* RGB_EN 输出高（使能灯珠） */
        DelayMs(2);                     /* 等灯珠上电稳定 */
        rgb_render();
        RGB_Show();
    }
    else
    {
        RGB_Clear();
        s_on = 1u;                      /* 临时允许发送，把灯刷灭 */
        RGB_Show();
        s_on = 0u;
        rgb_en_write(0);                /* 再断电，避免残留微亮 */
    }
}

uint8_t RGB_IsOn(void)
{
    return s_on;
}

/*==========================================================================
 * 功能 2：灯效驱动输出
 *========================================================================*/
void RGB_ShowEffect(uint8_t effect)
{
    if (effect > RGB_EFF_MAX)
    {
        effect = RGB_EFF_MAX;
    }
    s_effect = effect;
    s_phase  = 0u;

    rgb_render();
    if (s_on != 0u)
    {
        RGB_Show();                     /* 立即出一帧，切灯效立刻可见 */
    }
}

uint8_t RGB_GetEffect(void)
{
    return s_effect;
}

/* 预置下标 -> 实际灯效编号（设置页和 Main.c 都用这一个入口，避免两边各写一份） */
static const uint8_t s_fx_preset[RGB_FX_PRESET_COUNT] =
{
    RGB_EFF_FX_BREATH,
    RGB_EFF_FX_CHASE,
    RGB_EFF_FX_RIPPLE,
    RGB_EFF_FX_WAVE,
    RGB_EFF_FX_RAINBOW
};

uint8_t RGB_EffectOfPreset(uint8_t idx)
{
    if (idx >= (uint8_t)RGB_FX_PRESET_COUNT)
    {
        idx = 0u;
    }
    return s_fx_preset[idx];
}

/*********************************************************************
 * @fn      rgb_effect_is_static
 * @brief   这个灯效会不会随 s_phase 变化？
 *
 * 【2026-10-18 功耗优化】
 *   RGB_EffectTask() 每 20ms 被调一次，原来无论什么灯效都会 rgb_render()
 *   + RGB_Show() 走一遍。而 RGB_Show() 要按字节关中断发 9 颗灯珠 × 3 字节
 *   = 27 次临界区（每次约 10us），一帧下来 ~270us 中断被切成 27 段。
 *
 *   像 RGB_EFF_WHITE（全白常亮）、RGB_EFF_KEY_WHITE（只跟按键走）这类
 *   **完全不依赖 s_phase** 的灯效，每 20ms 重算重发一遍纯属白烧：
 *   灯珠内容一个字都没变，却每秒制造 50 次 SPI/临界区开销，
 *   还把 BLE 的连接事件窗口切碎（连接间隔只有 7.5~11.25ms）。
 *
 *   静态灯效的内容变化由 RGB_UpdateByKeyMap() / RGB_Switch() 负责刷新，
 *   所以这里直接跳过是安全的。
 *********************************************************************/
static uint8_t rgb_effect_is_static(uint8_t eff)
{
    switch (eff)
    {
        case RGB_EFF_WHITE:
        case RGB_EFF_KEY_WHITE:
            return 1u;

        default:
            return 0u;
    }
}

/*********************************************************************
 * @fn      RGB_EffectTask
 * @brief   动画推进：每 20ms 调用一次（内部按 20ms 累加计时）
 *********************************************************************/
void RGB_EffectTask(void)
{
    if (s_on == 0u)
    {
        return;
    }
    if (s_effect == RGB_EFF_OFF)
    {
        return;
    }

    /* 静态灯效：内容不会随相位变，重发等于白烧中断（见 rgb_effect_is_static） */
    if (rgb_effect_is_static(s_effect) != 0u)
    {
        return;
    }

    s_phase++;
    rgb_render();
    RGB_Show();
}

/*==========================================================================
 * 功能 3：按键 → 对应灯变白（键位从上到下、每排从左到右）
 *========================================================================*/
void RGB_KeyWhite(uint8_t row, uint8_t col, uint8_t on)
{
    uint8_t idx;

    if ((row >= RGB_LED_ROWS) || (col >= RGB_LED_COLS))
    {
        return;
    }

    idx = (uint8_t)(row * RGB_LED_COLS + col);      /* 逻辑序号 = 键盘 K1~K9 的编号 */

    if (on)
    {
        s_key_mask |= (uint16_t)(1u << idx);
    }
    else
    {
        s_key_mask &= (uint16_t)~(uint16_t)(1u << idx);
    }

    rgb_render();
    if (s_on != 0u)
    {
        RGB_Show();
    }
}

/*********************************************************************
 * @fn      RGB_UpdateByKeyMap
 * @brief   用键盘位图批量刷新（bit0~bit8 = K1~K9）
 *          直接配合 key_matrix 模块：KEY_GetMap() / ev.map
 *********************************************************************/
void RGB_UpdateByKeyMap(uint16_t keymap)
{
    s_key_mask = keymap;

    rgb_render();
    if (s_on != 0u)
    {
        RGB_Show();
    }
}

/*==========================================================================
 * 初始化
 *========================================================================*/
void RGB_Init(void)
{
    /* 1) RGB_EN 先拉低：上电时灯不亮、也不耗电 */
    if (RGB_EN_PORTB) { GPIOB_ModeCfg(RGB_EN_PIN, GPIO_ModeOut_PP_5mA); }
    else              { GPIOA_ModeCfg(RGB_EN_PIN, GPIO_ModeOut_PP_5mA); }
    rgb_en_write(0);

    /* 2) DIN：纯 GPIO（PWM9 让给 BLK 的屏幕亮度 PWM） */
    rgb_din_gpio_init();

        /* 【2026-10-14】这里**完全不碰 SysTick**。
     *
     * 曾经在这里调 SysTick_Config() 想给 WS2812 的软件延时找个精确时基，
     * 结果它**会打开 SysTick 中断**并留下一个永远没人清的挂起标志；
     * 等进键盘页、CH59x_BLEInit() 里再次 PFIC_EnableIRQ(SysTick_IRQn) 时，
     * 那个挂起中断立刻被投递 —— 而全工程没有 SysTick 处理函数，
     * **直接跑飞，屏幕停在 BT step 1**。
     *
     * SysTick 现在只由 BLE 协议栈自己管（CH59x_BLEInit 里那一次），
     * 别人一律不许动。WS2812 的位时序改用纯 nop 循环，见 rgb_delay_cycles()。 */

    /* 3) 状态复位 */
    s_effect   = RGB_EFF_KEY_WHITE;     /* 默认：按键跟随（按下哪个键，哪个灯白） */
    s_phase    = 0u;
    s_on       = 0u;
    s_key_mask = 0u;
    RGB_Clear();
}

/******************* (C) CH592X 键盘 RGB 灯效 *****************/
