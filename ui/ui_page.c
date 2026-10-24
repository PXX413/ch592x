/**************************************************************************
 * ui_page.c —— 页面栈 + 路由 + 编码器事件分发
 **************************************************************************/

#include "ui_page.h"
#include "key_matrix.h"
#include "encoder.h"
#include <string.h>

/* 一帧多久重画一次（屏刷新是局部重绘，20ms 足够） */
#ifndef UI_FRAME_MS
#define UI_FRAME_MS         20u
#endif

typedef struct
{
    const ui_page_t *page;
    void            *arg;
} ui_frame_t;

static ui_frame_t s_stack[UI_PAGE_DEPTH];
static uint8_t    s_top;            /* 栈顶下标，184 表示栈空 */
static uint8_t    s_dirty;
static uint8_t    s_enc_down;       /* 编码器原始电平（由 Game 事件维护） */
static ui_enc_t   s_enc;
static uint32_t   s_last_frame_ms;
static uint32_t   s_epoch;          /* 页面切换代号，见 UI_Epoch() */
static uint8_t    s_last_key;       /* 【2026-10-14】最近投给页面的键，切页时用来吞键 */

/*==========================================================================
 * 内部
 *========================================================================*/
static void ui_call_enter(const ui_page_t *p, void *arg)
{
    if ((p != 0) && (p->enter != 0))
    {
        p->enter(arg);
    }
}

static void ui_call_leave(const ui_page_t *p)
{
    if ((p != 0) && (p->leave != 0))
    {
        p->leave();
    }
}

static void ui_redraw_all(void)
{
    s_dirty = 1u;
}

/*==========================================================================
 * 栈操作
 *========================================================================*/
void UI_Init(void)
{
    memset(s_stack, 0, sizeof(s_stack));
    s_top      = 0u;
    s_stack[0].page = &UI_PAGE_HOME;
    s_stack[0].arg  = 0;
    s_dirty    = 1u;
    s_enc_down = 0u;
    UiEnc_Reset(&s_enc);
    s_last_frame_ms = Game_Millis();

    ui_call_enter(&UI_PAGE_HOME, 0);
}

void UI_Push(const ui_page_t *p, void *arg)
{
    if ((p == 0) || (s_top + 1u >= (uint8_t)UI_PAGE_DEPTH))
    {
        return;
    }

    /* 【2026-10-13】防重入：目标页已经在栈顶就忽略。
     * 长按产生的 GAME_EV_REPEAT（每 90ms 一条）会让页面处理函数反复执行，
     * 从而反复 UI_Push 同一个页 —— 结果是 enter 反复跑（kbd_enter 里的
     * KEY_BlockInput 被无限续期，9 键永久锁死）、导航栈被撑满、
     * 每 90ms 还来一次整屏重画。加了这道闸，重复 push 直接丢掉。 */
    if (s_stack[s_top].page == p)
    {
        return;
    }

    s_top++;
    s_stack[s_top].page = p;
    s_stack[s_top].arg  = arg;
    ui_call_enter(p, arg);
    s_epoch++;
    ui_redraw_all();

    /* 【2026-10-14 修复：KEY5 进新页后被再识别一次】
     *
     * 现象：用 KEY5 进键盘页，键盘页立刻显示 5；用 KEY5 进某些设置页，
     *       进去后立刻被判成一次确认又弹回来。
     *
     * 原因：UI_Task() 的排空循环是
     *     while (Game_PollEvent(&ev)) { UI_PostKey(ev.key, ev.type); }
     * 而 UI_PostKey() 每次都取【当前的栈顶页】。于是页面处理函数里一旦
     * UI_Push/UI_Pop 切了页，【队列里还没排完的事件就全部投给了新页面】：
     *     按下 K5  -> 旧页收到 PRESS 并切页
     *     按住不放 -> 450ms 后 key_track 每 90ms 一个 GAME_EV_REPEAT
     *              -> 这些 REPEAT 全灌进新页面 -> 新页面当成又按了一次
     *
     * 解法：切页的这一刻把队列清空。RELEASE / REPEAT 本来就只对旧页面
     * 有意义，丢掉不会让任何状态机卡住 —— key_track 的按下状态是每圈
     * 直接读 KEY_IsPressed() 来的，不依赖队列。
     *
     * UI_Pop / UI_GoHome 里同样加了这一句。
     *
     * 这一条同时替掉了键盘页原来那句 KEY_BlockInput(0.5s)：那个屏蔽是
     * 为了让 KEY_Scan() 整段跳过 GPIO 扫描 0.5 秒，一旦标志退不出来就把
     * 9 个键全锁死。病根既然是事件跨页投递，在这里堵掉就不用屏蔽了。 */
    /* 吞掉触发这次切页的那个键：它要是一直按着，键层会在 450ms 后
     * 继续产生 GAME_EV_REPEAT，那些新事件会投给新页面 -> 新页面当成
     * 又按了一次。FlushEvents 只能清"已经排队的"，清不了"还没产生的"。 */
    Game_SwallowKey(s_last_key);
    Game_FlushEvents();
}

void UI_Pop(void)
{
    if (s_top == 0u)
    {
        return;                     /* 已经在主页面，回不去了 */
    }

    ui_call_leave(s_stack[s_top].page);
    s_stack[s_top].page = 0;
    s_stack[s_top].arg  = 0;
    s_top--;
    s_epoch++;
    ui_redraw_all();

    /* 【2026-10-14 修复：KEY5 进新页后被再识别一次】
     *
     * 现象：用 KEY5 进键盘页，键盘页立刻显示 5；用 KEY5 进某些设置页，
     *       进去后立刻被判成一次确认又弹回来。
     *
     * 原因：UI_Task() 的排空循环是这样的 ——
     *     while (Game_PollEvent(&ev)) { UI_PostKey(ev.key, ev.type); }
     * 而 UI_PostKey() 每次都取【当前的栈顶页】。
     * 于是页面处理函数里一旦 UI_Push/UI_Pop 切了页，
     * 【队列里还没排完的事件就全部投给了新页面】：
     *     按下 K5   -> 旧页收到 PRESS 并切页
     *     按住不放  -> 450ms 后 key_track 每 90ms 一个 GAME_EV_REPEAT
     *               -> 这些 REPEAT 全灌进新页面 -> 新页面当成又按了一次
     *
     * 解法：切页的这一刻把队列清空。RELEASE / REPEAT 本来就只对旧页面
     * 有意义，丢掉不会让任何状态机卡住 —— key_track 的按下状态是每圈
     * 直接读 KEY_IsPressed() 来的，不依赖队列。
     *
     * 这一条同时替掉了键盘页原来那句 KEY_BlockInput(0.5s)：
     * 那个屏蔽是为了挡住【进页那一瞬间残留的键】，但它把整段 GPIO 扫描
     * 停掉 0.5 秒，一旦标志退不出来就把 9 个键全锁死。
     * 真正的病根是事件跨页投递，在这里堵掉就不用屏蔽了。 */
    /* 吞掉触发这次切页的那个键：它要是一直按着，键层会在 450ms 后
     * 继续产生 GAME_EV_REPEAT，那些新事件会投给新页面 -> 新页面当成
     * 又按了一次。FlushEvents 只能清"已经排队的"，清不了"还没产生的"。 */
    Game_SwallowKey(s_last_key);
    Game_FlushEvents();
}

void UI_Replace(const ui_page_t *p, void *arg)
{
    if (p == 0)
    {
        return;
    }

    ui_call_leave(s_stack[s_top].page);
    s_stack[s_top].page = p;
    s_stack[s_top].arg  = arg;
    ui_call_enter(p, arg);
    s_epoch++;
    ui_redraw_all();
}

void UI_GoHome(void)
{
    while (s_top > 0u)
    {
        ui_call_leave(s_stack[s_top].page);
        s_stack[s_top].page = 0;
        s_stack[s_top].arg  = 0;
        s_top--;
    }

    /* 回主页时把手势状态清干净，免得半截双击跨页面带过去 */
    UiEnc_Reset(&s_enc);
    s_enc_down = 0u;
    s_epoch++;
    ui_redraw_all();

    /* 【2026-10-14 修复：KEY5 进新页后被再识别一次】
     *
     * 现象：用 KEY5 进键盘页，键盘页立刻显示 5；用 KEY5 进某些设置页，
     *       进去后立刻被判成一次确认又弹回来。
     *
     * 原因：UI_Task() 的排空循环是这样的 ——
     *     while (Game_PollEvent(&ev)) { UI_PostKey(ev.key, ev.type); }
     * 而 UI_PostKey() 每次都取【当前的栈顶页】。
     * 于是页面处理函数里一旦 UI_Push/UI_Pop 切了页，
     * 【队列里还没排完的事件就全部投给了新页面】：
     *     按下 K5   -> 旧页收到 PRESS 并切页
     *     按住不放  -> 450ms 后 key_track 每 90ms 一个 GAME_EV_REPEAT
     *               -> 这些 REPEAT 全灌进新页面 -> 新页面当成又按了一次
     *
     * 解法：切页的这一刻把队列清空。RELEASE / REPEAT 本来就只对旧页面
     * 有意义，丢掉不会让任何状态机卡住 —— key_track 的按下状态是每圈
     * 直接读 KEY_IsPressed() 来的，不依赖队列。
     *
     * 这一条同时替掉了键盘页原来那句 KEY_BlockInput(0.5s)：
     * 那个屏蔽是为了挡住【进页那一瞬间残留的键】，但它把整段 GPIO 扫描
     * 停掉 0.5 秒，一旦标志退不出来就把 9 个键全锁死。
     * 真正的病根是事件跨页投递，在这里堵掉就不用屏蔽了。 */
    /* 吞掉触发这次切页的那个键：它要是一直按着，键层会在 450ms 后
     * 继续产生 GAME_EV_REPEAT，那些新事件会投给新页面 -> 新页面当成
     * 又按了一次。FlushEvents 只能清"已经排队的"，清不了"还没产生的"。 */
    Game_SwallowKey(s_last_key);
    Game_FlushEvents();
}

uint32_t UI_Epoch(void)
{
    return s_epoch;
}

/* 【强制整页重画】比 UI_Dirty() 多走一步：把页面代号 +1。
 *
 * 为什么要这个：carousel / cardlist 这类"只重画变化区域"的控件，
 * 是靠 UI_Epoch() 变没变来决定要不要整页重画的。电量这种**不属于任何
 * 控件内部状态**的数据变了，光调 UI_Dirty() 只会让页面回调跑一遍，
 * 控件走的是局部刷新分支 —— 状态栏里的电量条根本不会被重画。
 * 需要"整页重画"的场合（如电量刷新）就用这个。 */
void UI_DirtyFull(void)
{
    s_epoch++;
    s_dirty = 1u;
}

void UI_Dirty(void)
{
    s_dirty = 1u;
}

const ui_page_t *UI_Current(void)
{
    return s_stack[s_top].page;
}

uint8_t UI_Depth(void)
{
    return (uint8_t)(s_top + 1u);
}


ui_nav_t UI_PostKey(uint8_t key, uint8_t type)
{
    GameKeyEvent_t ev;
    ui_nav_t       nav;

    s_last_key = key;

    if (s_stack[s_top].page == 0)
    {
        return UI_NAV_STAY;
    }

    /* ---- 全局规则优先（文档 (六).3 / (六).4）---- */
    if (key == (uint8_t)GKEY_ENC_HOLD3)
    {
        UI_GoHome();
        return UI_NAV_HOME;
    }

    if (key == (uint8_t)GKEY_ENC_DBL)
    {
        /* 【修复 §8-4】先给当前页面一次处理双击的机会。
         *
         * 游戏页里双击的语义是"退出这一局、回到选游戏列表"（需求 (三)），
         * 而框架的全局规则是"双击回上一级"—— 那会直接把整个游戏页弹回
         * 主页面，跳过了"选游戏"这一级。页面返回 UI_NAV_CONSUMED 就表示
         * "我自己处理了，别再弹"。其它页面照旧走下面的全局规则。 */
        if (s_stack[s_top].page->on_key != 0)
        {
            ev.key     = (GameKey_t)key;
            ev.type    = type;
            ev.held_ms = 0u;

            if (s_stack[s_top].page->on_key(&ev) == UI_NAV_CONSUMED)
            {
                return UI_NAV_STAY;
            }
        }

        if (s_top > 0u)
        {
            UI_Pop();
            return UI_NAV_POP;
        }
        return UI_NAV_STAY;         /* 主页面双击没地方回 */
    }

    if (s_stack[s_top].page->on_key == 0)
    {
        return UI_NAV_STAY;
    }

    ev.key     = (GameKey_t)key;
    ev.type    = type;
    ev.held_ms = 0u;

    nav = s_stack[s_top].page->on_key(&ev);

    if (nav == UI_NAV_POP)
    {
        if (s_top > 0u)
        {
            UI_Pop();
        }
        else
        {
            nav = UI_NAV_STAY;
        }
    }
    else if (nav == UI_NAV_HOME)
    {
        UI_GoHome();
    }

    return nav;
}

/*==========================================================================
 * 主循环
 *========================================================================*/
/*==========================================================================
 * 【诊断 2026-10-13】屏幕底部叠加一行按键状态，专查"9 键间歇性锁死"
 *
 * 排掉之后把 UI_KEY_DIAG 改回 0 即可（不改任何行为，只是少画一行）。
 *
 * 屏幕上的格式：
 *   R <raw_read>  L <raw_last>  S <stable>  D <debounce>  B <blocked> G <ghost>
 *
 * 复现时这样看：
 *   D 一直是 0/1 涨不到 15  -> 去抖被反复打断（raw 在抖）—— 已加关中断读列
 *   R 每次刷新都在变        -> key_read_raw() 读出来就是抖的（硬件/干扰）
 *   S 冻在非 0 值           -> 真卡在某个状态
 *   B = 1                   -> KEY_BlockInput 没退出，整段扫描被跳过
 *========================================================================*/
#ifndef UI_KEY_DIAG
#define UI_KEY_DIAG     0       /* 1 = 叠加一行按键诊断（插问题时改成 1） */
#endif

#if (UI_KEY_DIAG)
static void ui_diag_hex4(char *p, uint16_t v)
{
    uint8_t i;
    for (i = 0u; i < 4u; i++)
    {
        uint8_t n = (uint8_t)((v >> (12u - (i * 4u))) & 0x0Fu);
        p[i] = (char)((n < 10u) ? ('0' + n) : ('A' + (n - 10u)));
    }
}

static void ui_diag_draw(void)
{
    KeyDbg_t d;
    char     b[44];

    KEY_DbgGet(&d);

    b[0] = 'R'; b[1] = ' ';
    ui_diag_hex4(&b[2], d.raw_read);
    b[6] = ' '; b[7] = 'L'; b[8] = ' ';
    ui_diag_hex4(&b[9], d.raw_last);
    b[13] = ' '; b[14] = 'S'; b[15] = ' ';
    ui_diag_hex4(&b[16], d.stable);
    b[20] = ' '; b[21] = 'D'; b[22] = ' ';
    ui_diag_hex4(&b[23], d.debounce);
    b[27] = ' '; b[28] = 'B'; b[29] = (char)('0' + d.blocked);
    b[30] = ' '; b[31] = 'G'; b[32] = (char)('0' + (uint8_t)(d.ghost != 0u ? 1u : 0u));
    b[33] = 0;

    TFT_FillRect(0, 124, (int16_t)UI_SCREEN_W, 11, 0x0000u);
    Game_DrawAscii(2, 125, b, 1u, 0x07E0u, 0x0000u);
}
#endif

uint8_t UI_Task(void)
{
    GameKeyEvent_t ev;
    ui_enc_evt_t   ee;
    uint32_t       now;

    now = Game_Millis();

    /* ---- 1. 按键事件（编码器按键只用来喂分类器，不直接给页面）---- */
    while (Game_PollEvent(&ev))
    {
        if (ev.key == GKEY_ENC_BTN)
        {
            if (ev.type == (uint8_t)GAME_EV_PRESS)
            {
                s_enc_down = 1u;
            }
            else if (ev.type == (uint8_t)GAME_EV_RELEASE)
            {
                s_enc_down = 0u;
            }
            continue;
        }

        (void)UI_PostKey((uint8_t)ev.key, ev.type);
    }

    /* ---- 2. 编码器点击分类（每 1ms 一步）---- */
    ee = UiEnc_Step(&s_enc, s_enc_down, now);

    if (ee != UI_ENC_NONE)
    {
        (void)UI_PostKey(UiEnc_ToGameKey(ee), (uint8_t)GAME_EV_PRESS);
    }

    /* ---- 3. 固定帧率：update + draw ---- */
    if ((uint32_t)(now - s_last_frame_ms) < (uint32_t)UI_FRAME_MS)
    {
#if (UI_KEY_DIAG)
        /* 诊断行不受帧率门限制，保证一直刷新（它是最底下 11 行） */
        {
            static uint32_t s_dg_ms;
            if ((uint32_t)(now - s_dg_ms) >= 200u)
            {
                s_dg_ms = now;
                ui_diag_draw();
            }
        }
#endif
        return 0u;
    }
    s_last_frame_ms = now;

    if (s_stack[s_top].page == 0)
    {
        return 0u;
    }

    if (s_stack[s_top].page->update != 0)
    {
        s_stack[s_top].page->update((uint16_t)UI_FRAME_MS);
    }

    if ((s_dirty != 0u) && (s_stack[s_top].page->draw != 0))
    {
        /* FW38：量一次整页重画到底花多少 ms（显示在屏底诊断行 U 段）*/
        extern volatile uint32_t g_draw_ms_max;
        uint32_t _d0 = Game_Millis();
        s_dirty = 0u;
        s_stack[s_top].page->draw();
        {
            uint32_t _d1 = (uint32_t)(Game_Millis() - _d0);
            if (_d1 > g_draw_ms_max) { g_draw_ms_max = _d1; }
        }

#if (UI_KEY_DIAG)
        {
            static uint32_t s_dg_ms;
            if ((uint32_t)(now - s_dg_ms) >= 200u)
            {
                s_dg_ms = now;
                ui_diag_draw();
            }
        }
#endif
        return 1u;                  /* 这一帧真的画完了 */
    }

#if (UI_KEY_DIAG)
    {
        static uint32_t s_dg_ms;
        if ((uint32_t)(now - s_dg_ms) >= 200u)
        {
            s_dg_ms = now;
            ui_diag_draw();
        }
    }
#endif
    return 0u;                      /* 没有需要重画的内容 */
}
