/**************************************************************************
 * game_port.c —— CH592X 小游戏公共层实现
 *
 * 依赖 key_matrix.c / encoder.c / tft_*.c，详见 game_port.h
 **************************************************************************/

#include "game_port.h"
#include "key_matrix.h"
#include "encoder.h"

/*==========================================================================
 * 内部状态
 *========================================================================*/
static int16_t      s_w      = GAME_PANEL_W;   /* 当前方向下的宽 */
static int16_t      s_h      = GAME_PANEL_H;   /* 当前方向下的高 */
static GameOrient_t s_orient = GAME_PORTRAIT;  /* 当前方向 */

static uint32_t     s_ms;                      /* 毫秒计数（= g_ms_tick - s_base_ms，真实毫秒） */
static uint32_t     s_base_ms;                 /* Game_Init() 那一刻的系统时基 */
static uint32_t     s_rand = 0x1F123BB5u;      /* 随机数状态 */

static uint8_t      s_down[GKEY_COUNT];        /* 消抖后的按下状态 */
static uint16_t     s_hold[GKEY_COUNT];        /* 已按住毫秒数 */
static uint16_t     s_next[GKEY_COUNT];        /* 下一次连发的时间点 */
static uint32_t     s_press_ms[GKEY_COUNT];   /* 每个键按下那一刻的 s_ms（真实毫秒时基） */

static GameKeyEvent_t s_q[GAME_EVENT_QUEUE];   /* 事件环形队列 */
static uint8_t        s_q_head;
static uint8_t        s_q_tail;
static uint8_t        s_q_cnt;
static uint32_t       s_q_drop;                /* 因为队列满而丢掉的事件数 */

/*==========================================================================
 * 事件队列
 *========================================================================*/
static void q_push(GameKey_t key, uint8_t type, uint16_t held_ms)
{
    if (s_q_cnt >= GAME_EVENT_QUEUE)
    {
        s_q_drop++;                     /* 队列满：丢弃新事件（游戏不会因此卡死） */
        return;
    }

    s_q[s_q_tail].key     = key;
    s_q[s_q_tail].type    = type;
    s_q[s_q_tail].held_ms = held_ms;

    s_q_tail = (uint8_t)((s_q_tail + 1u) % GAME_EVENT_QUEUE);
    s_q_cnt++;
}

bool Game_PollEvent(GameKeyEvent_t *ev)
{
    if (ev == 0 || s_q_cnt == 0u)
    {
        return false;
    }

    *ev = s_q[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1u) % GAME_EVENT_QUEUE);
    s_q_cnt--;
    return true;
}

void Game_FlushEvents(void)
{
    s_q_head = 0;
    s_q_tail = 0;
    s_q_cnt  = 0;
}

/*==========================================================================
 * 读取一个"键"的物理状态（9 个矩阵键 + 编码器按键）
 *========================================================================*/
/*==========================================================================
 * 【2026-10-14】切页瞬间的"吞键"集合
 *
 * 背景：UI_Push/UI_Pop/UI_GoHome 里已经用 Game_FlushEvents() 清掉了队列里
 * 剩下的事件，但那只解决了"已经排在队列里"的那部分。**按键还按着**的话，
 * key_track() 会在 450ms 后继续每 90ms 产生一个 GAME_EV_REPEAT，
 * 这些**新产生**的事件照样会投给新页面 —— 于是：
 *     按住 K5 进键盘页  -> 键盘页还是显示 5
 *     按住 K5 进设置页  -> 设置页还是立刻被判成一次确认弹回来
 *
 * 解法：切页时把"触发这次切页的那个键"记下来，在它**松开之前**一律不产生
 * 任何事件（PRESS/REPEAT/RELEASE 都不发）。松开后自动解除。
 *
 * 这比原来键盘页那句 KEY_BlockInput(0.5s) 精确得多：只吞一个键、
 * 而且是事件级而不是"停掉整段 GPIO 扫描"，绝不会把 9 个键一起锁死。
 *========================================================================*/
static uint8_t s_swallow_mask;      /* 位 k-1 = 该键在释放前一律不产生事件 */

void Game_SwallowKey(uint8_t key)
{
    if ((key >= (uint8_t)GKEY_K1) && (key <= (uint8_t)GKEY_K9))
    {
        s_swallow_mask |= (uint8_t)(1u << (key - 1u));
    }
}

/* 某键是否处于吞键状态 */
static uint8_t key_swallowed(uint8_t k)
{
    return (uint8_t)((s_swallow_mask >> (k - 1u)) & 1u);
}

static uint8_t key_raw(GameKey_t key)
{
    /* 【2026-10-13】按键屏蔽（进键盘页前 0.5 秒）挪到这一层生效。
     * 矩阵本身照常扫描（s_raw_stable 一直是活的），只是这里按【没按】处理：
     *   * 不产生任何新的按下事件；
     *   * 之前按着的键会在这一拍上报【松开】，不会卡住主机的键。
     * 这样屏蔽标志即使卡住，也只影响【按键层收不到按键】，
     * 而不会再像以前那样把整段 GPIO 扫描停掉。 */
    if (KEY_IsBlocked())
    {
        return 0u;
    }

    if (key >= GKEY_K1 && key <= GKEY_K9)
    {
        /* 矩阵键盘的键号就是 1~9，和 KEY_IsPressed(n) 一一对应 */
        return (KEY_IsPressed((uint8_t)key)) ? 1u : 0u;
    }

    if (key == GKEY_ENC_BTN)
    {
        return (ENC_ButtonPressed() != 0u) ? 1u : 0u;
    }

    return 0u;                          /* 编码器旋转不是"按住"型，走另一条路 */
}

/* 单个键的边沿/连发状态机（每次主循环调一次；按住时长按【真实毫秒】算） */
static void key_track(uint8_t k)
{
    uint8_t now = key_raw((GameKey_t)k);

    /* 【2026-10-14】吞键：切页那一刻按着的键，在它松开之前一律不产生事件。
     * 光靠 Game_FlushEvents() 只能清掉"已经排在队列里"的事件；
     * 键还按着的话，这里 450ms 后照样每 90ms 生成一个 REPEAT，
     * 那些新事件会投给新页面 -> 新页面当成又按了一次。
     * 松开即自动解除。 */
    if (key_swallowed(k) != 0u)
    {
        if (now == 0u)
        {
            s_swallow_mask &= (uint8_t)(~(uint8_t)(1u << (k - 1u)));   /* 松开了 */
            s_down[k] = 0u;
            s_hold[k] = 0u;
            s_next[k] = 0u;
        }
        return;
    }

    if (now != 0u)
    {
        if (s_down[k] == 0u)
        {
            /* 刚按下 */
            s_down[k] = 1u;
            s_hold[k] = 0u;
            s_press_ms[k] = s_ms;                    /* 记下按下那一刻，后面算真实按住时长 */
            s_next[k] = (uint16_t)GAME_REPEAT_DELAY_MS;
            q_push((GameKey_t)k, GAME_EV_PRESS, 0u);
        }
        else
        {
            /* 一直按着：够了 GAME_REPEAT_DELAY_MS 就开始连发
             *
             * 【修复 2026-10-12】按住时长改用 s_ms（TMR0 1ms 中断累加的真实毫秒）。
             *   原来写的是 s_hold[k] += GAME_SCAN_MS，也就是"主循环跑一圈当 1ms"。
             *   可一圈实际是 1~几十 ms 不等（UI 刷屏 / RF 收发 / USB 中断都会拖它），
             *   于是 450ms 的连发延迟实测会变成 0.7s ~ 好几秒，而且随负载漂移 ——
             *   刷一次屏，按键的连发节奏就变一次。
             *   交接文档 §8-12 修的就是这一类"按圈数当毫秒"的问题，这里当时漏了。
             *   注：编码器按键的长按/双击在 ui/ui_enc.c 里独立计时，那边本来就是
             *   真实毫秒，不受这里影响。 */
            uint32_t held = (uint32_t)(s_ms - s_press_ms[k]);

            if (held > 60000u) { held = 60000u; }   /* 和原来一样封顶，长期按住也不会溢出 */
            s_hold[k] = (uint16_t)held;

            if (s_hold[k] >= s_next[k])
            {
                s_next[k] = (uint16_t)(s_next[k] + GAME_REPEAT_PERIOD_MS);
                q_push((GameKey_t)k, GAME_EV_REPEAT, s_hold[k]);
            }
        }
    }
    else if (s_down[k] != 0u)
    {
        /* 松开 */
        s_down[k] = 0u;
        q_push((GameKey_t)k, GAME_EV_RELEASE, s_hold[k]);
        s_hold[k] = 0u;
        s_press_ms[k] = 0u;
        s_next[k] = 0u;
    }
    else
    {
        /* 一直没按：什么也不做 */
    }
}

/*==========================================================================
 * 一次扫描（每次主循环调一次；时间一律读真实毫秒 s_ms，不要按圈数累加）
 *========================================================================*/
static void game_scan(void)
{
    uint8_t k;
    int8_t  step;

    KEY_Scan();                         /* 键盘（内部按 5ms 计时） */

    /* 编码器**不在这里扫**：ENC_Scan() 只在 TMR0 的 1ms 中断里跑（见 Main.c）。
     * 两处都扫会重复累加正交状态，而且中断和主循环并发读写同一组变量。 */

    /* 毫秒数直接读系统时基。主循环一圈不止 1ms（刷屏、RF 收发都算在内），
     * 按圈数累加的话编码器"长按 3 秒"实际要十几秒，双击窗口也会缩水。 */
    s_ms = (uint32_t)(g_ms_tick - s_base_ms);

    /* ---- 1. 9 个矩阵键 ---- */
    for (k = (uint8_t)GKEY_K1; k <= (uint8_t)GKEY_K9; k++)
    {
        key_track(k);
    }

    /* ---- 2. 编码器按键 ---- */
    key_track((uint8_t)GKEY_ENC_BTN);

    /* ---- 3. 编码器旋转：每格一个事件 ---- */
    step = ENC_GetStep();

    while (step > 0)
    {
        q_push(GKEY_ENC_CW, GAME_EV_PRESS, 0u);
        step--;
    }

    while (step < 0)
    {
        q_push(GKEY_ENC_CCW, GAME_EV_PRESS, 0u);
        step++;
    }
}

void Game_Task(void)
{
    game_scan();

    /* 【P1 修复】这里原来有一句 DelayMs(GAME_SCAN_MS)（1ms 忙等），现已删除。
     *
     * 它的危害不只是"白等 1ms"：
     *   1) 忙等期间主循环被占住 —— USB 的 EP0 没人喂，主机会收 NAK 超时重发；
     *      BLE 的 TMOS_SystemProcess() 与 TMOS 定时器也跟着变慢
     *      （见 usb_kbd_port_ch592.c 顶部的警告）。
     *   2) 而且**游戏自己的节拍并不靠它**：本文件的 s_ms 读的是 g_ms_tick
     *      （TMR0 1ms 中断累加），连发延迟、双击窗口、编码器长按全是真实毫秒。
     *      所以删掉它不影响任何计时精度，只把主循环从"每圈强制 1ms"里解放出来。
     *
     * 现在的节拍来源：TMR0 1ms 中断（Main.c 的 enc_timer_init）提供时基，
     * 主循环能跑多快跑多快；刷屏由 UI_Task() 自己按 20ms 分频，不在这里限速。 */
}

/*==========================================================================
 * 初始化
 *========================================================================*/
void Game_Init(GameOrient_t orient)
{
    uint8_t k;

    /* --- 状态清零 --- */
    s_base_ms = g_ms_tick;              /* 时基对齐：Game_Millis() 从 0 开始算 */
    s_ms = 0u;
    s_q_head = 0;
    s_q_tail = 0;
    s_q_cnt  = 0;
    s_q_drop = 0u;

    for (k = 0; k < (uint8_t)GKEY_COUNT; k++)
    {
        s_down[k] = 0u;
        s_hold[k] = 0u;
        s_next[k] = 0u;
    }

    /* --- 外设 --- */
    KEY_Init();
    /* 矩阵键的去抖/组合键窗口走真实毫秒（TMR0 1ms 时基），
     * 不再拿"调用次数 × 5ms"当时间 —— 详见 key_matrix.h 的说明。 */
    KEY_SetTimeSource(Game_Millis);
    ENC_Init();
    ST7789_Init(GAME_PANEL_W, GAME_PANEL_H);

    Game_SetOrientation(orient);
    Game_Clear(ST77XX_BLACK);

    /* --- 随机数种子：用系统 tick，每次上电都不一样 --- */
    s_rand = SYS_GetSysTickCnt() ^ 0x5A5A5A5Au;
    if (s_rand == 0u)
    {
        s_rand = 0x1F123BB5u;
    }
}

/*==========================================================================
 * 屏幕方向 / 尺寸
 *========================================================================*/
void Game_SetOrientation(GameOrient_t o)
{
    s_orient = o;

    if (o == GAME_LANDSCAPE)
    {
        TFT_SetRotation(1);             /* 旋转 90 度：240 宽 x 135 高 */
        s_w = GAME_PANEL_H;
        s_h = GAME_PANEL_W;
    }
    else
    {
        TFT_SetRotation(0);             /* 竖屏：135 宽 x 240 高 */
        s_w = GAME_PANEL_W;
        s_h = GAME_PANEL_H;
    }
}

GameOrient_t Game_GetOrientation(void)
{
    return s_orient;
}

int16_t Game_Width(void)
{
    return s_w;
}

int16_t Game_Height(void)
{
    return s_h;
}

/*==========================================================================
 * 按键查询
 *========================================================================*/
bool Game_KeyDown(GameKey_t key)
{
    if (key <= GKEY_NONE || key >= GKEY_COUNT)
    {
        return false;
    }
    return (s_down[key] != 0u) ? true : false;
}

uint16_t Game_KeyHeldMs(GameKey_t key)
{
    if (key <= GKEY_NONE || key >= GKEY_COUNT)
    {
        return 0u;
    }
    return s_hold[key];
}

uint32_t Game_Millis(void)
{
    return s_ms;
}

/*==========================================================================
 * 随机数：xorshift32
 *========================================================================*/
void Game_Srand(uint32_t seed)
{
    s_rand = (seed == 0u) ? 0x1F123BB5u : seed;
}

uint32_t Game_Rand(void)
{
    uint32_t x = s_rand;

    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);

    s_rand = (x == 0u) ? 0x1F123BB5u : x;
    return s_rand;
}

uint32_t Game_RandRange(uint32_t n)
{
    if (n == 0u)
    {
        return 0u;
    }
    return (Game_Rand() % n);
}

/*==========================================================================
 * 绘图：中文
 *========================================================================*/
void Game_Clear(uint16_t color)
{
    TFT_FillScreen(color);
}

void Game_DrawGlyph(int16_t x, int16_t y, GameGlyph_t g, uint16_t fg, uint16_t bg)
{
    if ((int)g < 0 || (int)g >= (int)GF_COUNT)
    {
        return;                         /* 字库里没有这个字：不画（宁可空白也不乱码） */
    }

    if (fg != bg)
    {
        TFT_FillRect(x, y, GAME_GLYPH_W, GAME_GLYPH_H, bg);
    }
    TFT_DrawBitmap(x, y, game_font16_data[g], GAME_GLYPH_W, GAME_GLYPH_H, fg);
}

void Game_DrawText(int16_t x, int16_t y, GameTextId_t id, uint16_t fg, uint16_t bg)
{
    const uint8_t *p;
    uint8_t        n;

    if ((int)id <= (int)GTXT_NONE || (int)id >= (int)GTXT_COUNT)
    {
        return;
    }

    p = game_text_ptr[id];
    n = game_text_len[id];

    if (p == 0)
    {
        return;
    }

    while (n > 0u)
    {
        Game_DrawGlyph(x, y, (GameGlyph_t)(*p), fg, bg);
        x = (int16_t)(x + GAME_GLYPH_W);
        p++;
        n--;
    }
}

int16_t Game_TextWidth(GameTextId_t id)
{
    if ((int)id <= (int)GTXT_NONE || (int)id >= (int)GTXT_COUNT)
    {
        return 0;
    }
    return (int16_t)(game_text_len[id] * GAME_GLYPH_W);
}

void Game_DrawTextCenter(int16_t y, GameTextId_t id, uint16_t fg, uint16_t bg)
{
    int16_t x = (int16_t)((s_w - Game_TextWidth(id)) / 2);

    if (x < 0)
    {
        x = 0;
    }
    Game_DrawText(x, y, id, fg, bg);
}

/*==========================================================================
 * 绘图：数字（5x7 字库按 scale 放大）
 *========================================================================*/
int16_t Game_NumberWidth(uint32_t num, uint8_t scale)
{
    uint8_t  digits = 1u;
    uint32_t v      = num;

    while (v >= 10u)
    {
        v /= 10u;
        digits++;
    }

    if (scale == 0u)
    {
        scale = 1u;
    }

    /* 5x7 字库每个字符占 6 列（5 列字 + 1 列间隔），放大 scale 倍 */
    return (int16_t)(6u * scale * digits);
}

void Game_DrawNumber(int16_t x, int16_t y, uint32_t num, uint8_t scale,
                     uint16_t fg, uint16_t bg)
{
    if (scale == 0u)
    {
        scale = 1u;
    }

    TFT_SetTextSize(scale);
    TFT_SetTextColorBg(fg, bg);
    TFT_SetCursor(x, y);
    TFT_Printf("%u", (unsigned int)num);
}

void Game_DrawNumberRight(int16_t x_right, int16_t y, uint32_t num, uint8_t scale,
                          uint16_t fg, uint16_t bg)
{
    Game_DrawNumber((int16_t)(x_right - Game_NumberWidth(num, scale)), y, num, scale, fg, bg);
}

/*==========================================================================
 * 绘图：ASCII 字符串
 *========================================================================*/
int16_t Game_AsciiWidth(const char *s, uint8_t scale)
{
    uint16_t n = 0u;

    if (s == 0)
    {
        return 0;
    }
    if (scale == 0u)
    {
        scale = 1u;
    }
    while (s[n] != '\0')
    {
        n++;
    }
    return (int16_t)(6u * (uint16_t)scale * n);
}

void Game_DrawAscii(int16_t x, int16_t y, const char *s, uint8_t scale,
                    uint16_t fg, uint16_t bg)
{
    if (s == 0)
    {
        return;
    }
    if (scale == 0u)
    {
        scale = 1u;
    }

    TFT_SetTextSize(scale);
    TFT_SetTextColorBg(fg, bg);
    TFT_SetCursor(x, y);
    (void)TFT_Printf("%s", s);
}
/*==========================================================================
 * 绘图：面板 / 提示条
 *========================================================================*/
void Game_DrawPanel(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t fill, uint16_t border)
{
    TFT_FillRect(x, y, w, h, fill);

    if (border != fill)
    {
        TFT_DrawRect(x, y, w, h, border);
    }
}

void Game_DrawHintBar(GameTextId_t id, uint16_t fg, uint16_t bg)
{
    int16_t y = (int16_t)(s_h - GAME_HINT_BAR_H);

    TFT_FillRect(0, y, s_w, GAME_HINT_BAR_H, bg);

    if ((int)id > (int)GTXT_NONE && (int)id < (int)GTXT_COUNT)
    {
        Game_DrawTextCenter((int16_t)(y + 1), id, fg, bg);
    }
}

/*==========================================================================
 * 音效（预留）
 *========================================================================*/
void Game_Sound(uint16_t freq_hz, uint16_t ms)
{
    (void)freq_hz;
    (void)ms;
    /* 现在什么都不做。
     * 以后接无源蜂鸣器：在这里用 PWMX 输出 freq_hz，延时 ms 后停掉即可，
     * 所有游戏代码都不用改。 */
}

/******************* (C) CH592X 小游戏公共层 *****************/
