/**************************************************************************
 * ui_pages.c —— 前 7 个页面（卡片风格）
 *
 * 做实：游戏页 / 设置页 / 亮度页 / 休眠时间页 / 恢复默认页
 *
 * 版面已从"2px 黄框 + 23px 行"改为**卡片式列表**（ui_cardlist）：
 *   * 每项一张圆角卡片，带左侧图标
 *   * 选中卡片：绿色描边 + 绿色左竖条 + 右侧绿箭头
 *   * 卡片高 23、缝隙 3、圆角 5，选中项固定在屏幕垂直中央
 *
 * 顶部标题保留（用主题强调色），其余沿用原交互。
 **************************************************************************/

#include "ui_page.h"
#include "bt_wch.h"
#include "ui_cardlist.h"
#include "ui_carousel.h"
#include "icon_game_60x60.h"
#include "ui_widget.h"
#include "ui_store.h"
#include "ui_transport.h"
#include "key_matrix.h"
#include "rgb_led.h"
#include "tft_gfx.h"
#include "ui_kbd.h"
#include "ui_pages2.h"
#include "game_state.h"
#include "kmap_store.h"

/* 游戏模块（游戏页要用） */
#include "game_2048.h"
#include "game_runner.h"
#include "game_thunder.h"
#include "ui_theme.h"
#include "ui_icon.h"
#include "ui_card.h"

/*==========================================================================
 * 公共小工具
 *========================================================================*/

/* 页面背景：只有底色 */
static void page_bg(void)
{
    TFT_FillRect(0, 0, UI_SCREEN_W, UI_SCREEN_H, C_BG);
}

/* 顶部标题条（统一风格：分隔线用主题暗青） */
static void page_title(GameTextId_t id)
{
    Game_DrawTextCenter(1, id, C_ACCENT, C_BG);
    TFT_FillRect(0, (int16_t)17, UI_SCREEN_W, 1, C_ACCENT_DIM);
}

/* 编码器旋转 + K4/K6 都能上下选（沿用工程里的 WASD 手感） */
/* 【修复 §8-8】列表页的"上移"不含 GKEY_UP。
 *
 * GKEY_UP 在 game_port.h 里就是 GKEY_K5，而 GKEY_OK 也是 K5 —— 于是
 * is_up() 和 is_ok() 会同时命中 K5，列表页按 K5 只会"上移"，选中项永远
 * 确认不了（需求 (六).2 要求矩阵键在列表页是"确认"）。
 * 现在把 K5 从方向键里摘出来只留给 is_ok()，K4（GKEY_LEFT）继续当上移。 */
static bool is_up(const GameKeyEvent_t *ev)
{
    return (ev->key == GKEY_ENC_CCW) || (ev->key == GKEY_LEFT);
}
static bool is_down(const GameKeyEvent_t *ev)
{
    /* 【2026-10-13 需求】翻页只认 K6（右翻）+ 编码器顺时针。
     * 原来还把 K8（下）也算进来，现在 K8 在主页/列表页属于"无操作"按键。 */
    return (ev->key == GKEY_ENC_CW) || (ev->key == GKEY_RIGHT);
}
static bool is_ok(const GameKeyEvent_t *ev)
{
    return (ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK) || (ev->key == GKEY_ENC_BTN);
}
static bool is_back(const GameKeyEvent_t *ev)
{
    /* 【2026-10-13 需求】主页/列表页只保留"编码器逻辑"：
     * 返回上一级 = 编码器双击（框架全局规则）。
     * 原来的 K9 不再当返回键用（K9 现在属于"无操作"按键）。
     * 要把 K9 加回来，这里把它或回去即可：|| (ev->key == GKEY_BACK)。 */
    return (ev->key == GKEY_ENC_DBL);
}

/*==========================================================================
 * 通用：GTXT 文字表 + 图标表驱动的卡片内容回调
 *========================================================================*/
/* 右侧"当前值"绘制回调：调用方把值【右对齐】画到 x_right 为止。
 * x_right 已经预留了和右边选中箭头之间的间距。 */
typedef void (*clist_value_fn)(int16_t x_right, int16_t cy,
                               uint8_t idx, uint8_t selected,
                               uint16_t fg, uint16_t bg, void *user);

typedef struct
{
    const uint8_t *texts;       /* GTXT_xxx 数组 */
    const uint8_t *icons;       /* icon_t 数组（可为 0，用列表默认图标） */
    clist_value_fn vfn;         /* 右侧当前值；0 = 该项不显示值 */
    void          *vuser;       /* 传给 vfn 的用户数据 */
} clist_text_src_t;

/* 右侧值和"选中箭头"之间留的间距（px） */
#define CLIST_VALUE_GAP     4

static void clist_text_draw(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                            uint8_t idx, uint8_t selected, void *user)
{
    const clist_text_src_t *src = (const clist_text_src_t *)user;
    uint16_t   fg;
    uint16_t   bg;

    (void)ch;

    if ((src == 0) || (src->texts == 0)) { return; }

    fg = (selected != 0u) ? C_TEXT : C_TEXT_DIM;
    bg = (selected != 0u) ? Theme_Blend(C_BG_CARD_HI, C_GREEN, 40) : C_BG_CARD;

    Game_DrawText(cx, cy, (GameTextId_t)src->texts[idx], fg, bg);

    /* 【2026-10-12】右侧当前值画在【内容回调里】，而不是在外面事后补画。
     *
     * ui_cardlist 的 clist_render() 有两条刷新路径：
     *   text_only = 0 -> 整行重画（卡片底 + 图标 + 内容）
     *   text_only = 1 -> 先擦掉整块内容区 (cx,cy,cw,16) 再重画内容
     * 值画在内容区范围内，两条路径都会带上它 —— 所以滚动列表时
     * 每一行的值都会跟着重画，既不会丢，也不会留下没擦干净的重影。 */
    if (src->vfn != 0)
    {
        src->vfn((int16_t)(cx + cw - CLIST_VALUE_GAP), cy,
                 idx, selected, fg, bg, src->vuser);
    }
}

/*==========================================================================
 * 1. 游戏页（选游戏 -> 进游戏 -> 结束回列表）
 *========================================================================*/
#define GP_NONE     0xFFu
#define GP_2048     0u
#define GP_RUNNER   1u
#define GP_THUNDER  2u

/* 三个游戏：图标 / 名字 / 磁贴本色。
 * 2048 没有中文字模（title = GTXT_NONE），底栏和图标旁边用数字画。 */
static const uicar_item_t s_games[3] =
{
    { game_2048_60x60,    GTXT_NONE,     0xFD20u, 2048u },   /* 2048：橙 */
    { game_parkour_60x60, GTXT_RUNNER,   0x2FE6u, 0u },      /* 跑酷：绿 */
    { game_jet_60x60,     GTXT_THUNDER,  0x2DFFu, 0u }       /* 战机：蓝 */
};

static uicar_t   s_car;                 /* 选游戏用的磁贴轮播 */
static uint8_t   s_play = GP_NONE;      /* GP_NONE = 在选游戏 */

static void game_enter(void *arg)
{
    (void)arg;
    s_play = GP_NONE;
    UiCar_Init(&s_car, s_games, 3u, GTXT_SELECT_GAME);
    UiCar_Enter(&s_car);
}

static void game_start(uint8_t which)
{
    /* 状态池是"用时才申请"的：进游戏这一刻才向堆要（最大的一份，
     * 见 game_state.h）。要不到就干脆别进（列表页照旧），
     * 免得后面拿空指针乱画。
     *
     * 【共享堆的所有权】这块池和遥控模式的主机结构体（User/Main.c 的 s_rch）
     * 是**同一块堆**，同一时刻只能有一个所有者。正常路径上不会有冲突：
     * 页面一变成 game，radio_demand_of() 就返回 RADIO_NONE，主循环下一圈
     * radio_task() 会走 radio_enter_none() -> rc_stop() 把遥控那块还掉，
     * 而按键事件是在 radio_task() **之后**的 UI_Task() 里才处理的 ——
     * 也就是说用户按"进游戏"那一刻，遥控池早就还回去了。
     * 所以这里不需要（也没有接口）主动去动遥控模式。 */
    if (!Game_PoolAcquire())
    {
        return;
    }

    s_play = which;
    Game_FlushEvents();
    switch (which)
    {
        case GP_2048:    G2048_Enter(); break;
        case GP_RUNNER:  RUN_Enter();   break;
        default:         THD_Enter();   break;
    }
    UI_Dirty();
}

static uint8_t s_game_dirty;    /* 【2026-10-13】游戏页有按键 -> 下一帧重画 */
static uint8_t s_game_anim;     /* 【2026-10-15】上一帧游戏是否还在动（1=在动） */

static ui_nav_t game_key(const GameKeyEvent_t *ev)
{
    if ((s_play == GP_NONE) || (!Game_PoolReady()))
    {
        if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }
        if (is_up(ev))   { UiCar_Step(&s_car, -1); return UI_NAV_STAY; }
        if (is_down(ev)) { UiCar_Step(&s_car, +1); return UI_NAV_STAY; }
        if (is_ok(ev) || (ev->key == GKEY_ENC_HOLD))
        {
            /* 需求 (六).4：长按 0.5~2.5s 执行"当前可执行的操作"，这里就是进游戏 */
            UiCar_Tap(&s_car);
            game_start(UiCar_Sel(&s_car));
            return UI_NAV_STAY;
        }
        if (is_back(ev)) { return UI_NAV_POP; }
        if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }
        return UI_NAV_STAY;
    }

    /* 游戏里：先把按键喂给游戏自己的处理器 */
    {
        bool leave = false;

        if (ev->type != (uint8_t)GAME_EV_RELEASE)
        {
            s_game_dirty = 1u;      /* 有按键 -> 下一帧重画 */
        }

        switch (s_play)
        {
            case GP_2048:    leave = G2048_HandleKey(ev); break;
            case GP_RUNNER:  leave = RUN_HandleKey(ev);   break;
            default:         leave = THD_HandleKey(ev);   break;
        }

        if (leave)                  /* 游戏自己要求退出 -> 回选游戏 */
        {
            Game_PoolYield();       /* 把状态池还给堆（选游戏页不需要它） */
            s_play = GP_NONE;
            UiCar_Enter(&s_car);    /* 回到"选游戏"：整页重画一次 */

            /* 【修复 §8-4】如果是双击触发的退出，这一下已经被页面用掉了：
             * 告诉框架别再执行"双击回上一级"，否则会连游戏页一起弹掉。 */
            if (ev->key == (uint8_t)GKEY_ENC_DBL)
            {
                return UI_NAV_CONSUMED;
            }
        }
    }
    return UI_NAV_STAY;
}

/* 【2026-10-13】游戏页按键后是否需要重画。
 * 2048 是纯事件驱动、没有动画，原来 game_update() 里那句无条件的
 * UI_Dirty() 让它【每帧都重画整屏】，把主循环拖慢、变抖 ——
 * 而主循环越抖，KEY_Scan() 的去抖越容易被干扰打断（详见 key_matrix.c
 * 里 2026-10-13 那段关于"9 键锁死"的说明）。
 * 现在 2048 只在真有按键时才重画；跑酷/雷霆有动画时每帧刷，
 * 但一旦暂停/结束（画面不再变化）就停刷，只在有操作时刷一次。 */
static void game_update(uint16_t dt_ms)
{
    uint8_t anim;

    if ((s_play == GP_NONE) || (!Game_PoolReady()))
    {
        UiCar_Update(&s_car, dt_ms);        /* 选游戏页：推进轮播动画 */
        return;
    }

    switch (s_play)
    {
        case GP_2048:                       /* 纯事件驱动，没有动画 */
            G2048_Update(dt_ms);
            anim = 0u;
            break;

        case GP_RUNNER:
            RUN_Update(dt_ms);
            anim = (uint8_t)(((!RUN_IsPaused()) && (!RUN_IsOver())) ? 1u : 0u);
            break;

        default:                            /* 雷霆战机 */
            THD_Update(dt_ms);
            anim = (uint8_t)((THD_GetState() == (uint8_t)THD_ST_PLAY) ? 1u : 0u);
            break;
    }

    /* 【2026-10-15 暂停不刷屏】游戏停住（暂停 / 结束 / 标题 / 菜单）以后画面
     * 不会再变，只在"刚停下来"那一帧和真有按键（s_game_dirty）时才重画。
     * 原来跑酷和雷霆无条件每帧 UI_Dirty()，暂停以后照样把地面虚线、障碍、
     * 玩家一帧一帧重刷一遍，白占主循环 —— 就是"暂停了还在刷屏"。
     * 2048 一直是 anim=0，行为和以前一样（只在有按键时重画）。 */
    if (anim != 0u)
    {
        s_game_anim  = 1u;
        s_game_dirty = 0u;
        UI_Dirty();
        return;
    }

    if ((s_game_anim != 0u) || (s_game_dirty != 0u))
    {
        s_game_dirty = 0u;                  /* 停下来的那一帧 / 有按键 -> 刷一次 */
        UI_Dirty();
    }
    s_game_anim = 0u;
}

static void game_draw(void)
{
    if ((s_play == GP_NONE) || (!Game_PoolReady()))
    {
        UiCar_Draw(&s_car);                 /* 选游戏页：磁贴轮播 */
        return;
    }

    switch (s_play)
    {
        case GP_2048:    G2048_Draw(); break;
        case GP_RUNNER:  RUN_Draw();   break;
        default:         THD_Draw();   break;
    }
}

/* 离开游戏页时一定要把状态池还给堆（框架全局处理双击/长按，
 * 页面被弹出时只调 leave，不会调 game_key）。 */
static void game_leave(void)
{
    Game_PoolYield();
    s_play = GP_NONE;
}

const ui_page_t UI_PAGE_GAME = { "game", game_enter, game_leave, game_key, game_update, game_draw };

/*==========================================================================
 * 2. 灯效选择页（5 种适合 3x3 的预置灯效）
 *
 *  下标 0..4 与 rgb_led.c 的 s_fx_preset[] 一一对应，
 *  编译期断言盯着 UI_LEDFX_COUNT == RGB_FX_PRESET_COUNT。
 *========================================================================*/
static const uint8_t s_fx_name[UI_LEDFX_COUNT] =
{
    GTXT_FX_BREATH,         /* 呼吸 */
    GTXT_FX_CHASE,          /* 跑马 */
    GTXT_FX_RIPPLE,         /* 涟漪 */
    GTXT_FX_WAVE,           /* 波浪 */
    GTXT_FX_RAINBOW         /* 彩虹 */
};

/* 数量对不上就编不过（防止以后只改一边） */
typedef char ui_ledfx_count_must_match
    [((int)UI_LEDFX_COUNT == (int)RGB_FX_PRESET_COUNT) ? 1 : -1];

static const clist_text_src_t s_fx_src = { s_fx_name, 0 , 0, 0 };
static ui_cardlist_t s_fx_list;

static void fx_enter(void *arg)
{
    ui_cfg_t *c = UiStore_Cfg();

    (void)arg;
    UiCList_Init(&s_fx_list, ICON_LED, clist_text_draw, (void *)&s_fx_src);
    UiCList_SetCount(&s_fx_list, (uint8_t)UI_LEDFX_COUNT);
    UiCList_SetSel(&s_fx_list, (c != 0) ? c->led_fx : 0u);
    UI_Dirty();
}

static ui_nav_t fx_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }

    if (is_up(ev))   { UiCList_Move(&s_fx_list, -1); UI_Dirty(); return UI_NAV_STAY; }
    if (is_down(ev)) { UiCList_Move(&s_fx_list, +1); UI_Dirty(); return UI_NAV_STAY; }
    if (is_back(ev)) { return UI_NAV_POP; }
    if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }

    if (is_ok(ev) || (ev->key == GKEY_ENC_HOLD))
    {
        ui_cfg_t *c = UiStore_Cfg();

        if (c != 0)
        {
            c->led_fx = UiCList_Selected(&s_fx_list);
            (void)UiStore_Save();
            /* 立刻切过去，不用等主循环那一帧 */
            RGB_ShowEffect(RGB_EffectOfPreset(c->led_fx));
        }
        return UI_NAV_POP;              /* 选完自动回设置页 */
    }
    return UI_NAV_STAY;
}

static void fx_draw(void)
{
    if (UiCList_TakeBg(&s_fx_list) != 0u)
    {
        page_bg();
        page_title(GTXT_LEDFX);
        UiCList_Draw(&s_fx_list);
    }
    else
    {
        /* 换项：卡片底板/描边/图标都不变，只重画 4 行文字 */
        UiCList_DrawText(&s_fx_list);
    }
}

const ui_page_t UI_PAGE_LEDFX = { "ledfx", fx_enter, 0, fx_key, 0, fx_draw };

/*==========================================================================
 * 2. 设置页（键盘灯开关 / 灯效 / 按键提示 / 键盘灯亮度 / 休眠时间 / 恢复默认）
 *========================================================================*/
/*--------------------------------------------------------------------------
 * 设置页：每一项右侧的"当前状态值"（右对齐画到 x_right）
 *
 * 【为什么放在这里而不是 set_draw() 里】见 clist_text_draw() 的注释 ——
 * 放进内容回调后，列表滚动/换项时走的任何一条刷新路径都会带上它。
 *------------------------------------------------------------------------*/
static void set_value_draw(int16_t xr, int16_t cy, uint8_t idx,
                           uint8_t selected, uint16_t fg, uint16_t bg, void *user)
{
    ui_cfg_t  *c = UiStore_Cfg();
    GameTextId_t id;

    (void)selected; (void)user;
    if (c == 0) { return; }

    switch (idx)
    {
        case 0:     /* 键盘灯开关：开 / 关 */
            id = (c->led_on != 0u) ? GTXT_ON : GTXT_OFF;
            Game_DrawText((int16_t)(xr - Game_TextWidth(id)), cy, id, fg, bg);
            break;

        case 1:     /* 灯效：呼吸 / 跑马 / 涟漪 / 波浪 / 彩虹 */
            id = (GameTextId_t)s_fx_name[c->led_fx];
            Game_DrawText((int16_t)(xr - Game_TextWidth(id)), cy, id, fg, bg);
            break;

        case 2:     /* 按键提示：开 / 关 */
            id = (c->key_hint != 0u) ? GTXT_ON : GTXT_OFF;
            Game_DrawText((int16_t)(xr - Game_TextWidth(id)), cy, id, fg, bg);
            break;

        case 3:     /* 键盘灯亮度：0~100 数字，右对齐 */
            Game_DrawNumberRight(xr, cy, (uint32_t)c->led_bright, 2u, fg, bg);
            break;

        case 4:     /* 屏幕亮度：0~100 数字，右对齐 */
            Game_DrawNumberRight(xr, cy, (uint32_t)c->scr_bright, 2u, fg, bg);
            break;

        case 5:     /* 休眠时间："N 分钟" 或 "永不" */
            if (c->sleep_idx == (uint8_t)UI_SLEEP_NEVER)
            {
                id = GTXT_NEVER;
                Game_DrawText((int16_t)(xr - Game_TextWidth(id)), cy, id, fg, bg);
            }
            else
            {
                uint32_t mins = (uint32_t)UiStore_SleepMinutes(c->sleep_idx);
                int16_t  wmin = Game_TextWidth(GTXT_MIN);
                int16_t  wn   = Game_NumberWidth(mins, 2u);
                int16_t  x    = (int16_t)(xr - wmin - wn);

                Game_DrawNumber(x, cy, mins, 2u, fg, bg);
                Game_DrawText((int16_t)(x + wn), cy, GTXT_MIN, fg, bg);
            }
            break;

        default:    /* 恢复默认：不显示值 */
            break;
    }
}

static const uint8_t s_set_texts[7] =
{
    GTXT_KBD_LED,       /* 0 键盘灯开关 */
    GTXT_LEDFX,         /* 1 灯效     */
    GTXT_KEY_HINT,      /* 2 按键提示 */
    GTXT_BRIGHT,        /* 3 键盘灯亮度 */
    GTXT_SCRBRIGHT,     /* 4 屏幕亮度 */
    GTXT_SLEEP,         /* 5 休眠时间 */
    GTXT_RESET          /* 6 恢复默认 */
};
static const clist_text_src_t s_set_src = { s_set_texts, 0, set_value_draw, 0 };
static ui_cardlist_t s_set;

static void set_enter(void *arg)
{
    (void)arg;
    UiCList_Init(&s_set, ICON_SETTINGS, clist_text_draw, (void *)&s_set_src);
    UiCList_SetCount(&s_set, 7u);
    UI_Dirty();
}

/* 选中行右侧显示当前值：键盘灯/按键提示 = 开/关，灯效 = 灯效名 */
static void set_draw(void)
{
    /* 每项右侧的"当前值"由内容回调 set_value_draw() 负责画
     * （这样滚动/换项的两条刷新路径都会带上它），这里只管列表本身。 */
    if (UiCList_TakeBg(&s_set) != 0u)
    {
        page_bg();
        page_title(GTXT_SETTINGS);
        UiCList_Draw(&s_set);
    }
    else
    {
        /* 换项：只重画 4 行内容（卡片底和图标没变） */
        UiCList_DrawText(&s_set);
    }
}

static ui_nav_t set_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }

    if (is_up(ev))   { UiCList_Move(&s_set, -1); UI_Dirty(); return UI_NAV_STAY; }
    if (is_down(ev)) { UiCList_Move(&s_set, +1); UI_Dirty(); return UI_NAV_STAY; }
    if (is_back(ev)) { return UI_NAV_POP; }
    if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }

    if (is_ok(ev) || (ev->key == GKEY_ENC_HOLD))
    {
        ui_cfg_t *c = UiStore_Cfg();

        switch (UiCList_Selected(&s_set))
        {
            case 0u:                            /* 键盘灯开关 */
                if (c != 0)
                {
                    c->led_on = (c->led_on != 0u) ? 0u : 1u;
                    RGB_Switch(c->led_on);
                    (void)UiStore_Save();
                }
                UI_Dirty();
                break;

            case 1u: UI_Push(&UI_PAGE_LEDFX,  0);   break;   /* 灯效 */

            case 2u:                                        /* 按键提示开关 */
                if (c != 0)
                {
                    c->key_hint = (c->key_hint != 0u) ? 0u : 1u;
                    (void)UiStore_Save();
                }
                UI_Dirty();
                break;

            case 3u: UI_Push(&UI_PAGE_BRIGHT,   0); break;
            case 4u: UI_Push(&UI_PAGE_SCRBRIGHT, 0); break;   /* 屏幕亮度 */
            case 5u: UI_Push(&UI_PAGE_SLEEP,    0); break;
            default: UI_Push(&UI_PAGE_RESET,    0); break;
        }
    }
    return UI_NAV_STAY;
}

const ui_page_t UI_PAGE_SETTING = { "setting", set_enter, 0, set_key, 0, set_draw };

/*==========================================================================
 * 3. 键盘灯亮度页（中心显示数值，步进 5，范围 0~100，全灯白光）
 *========================================================================*/
/*--------------------------------------------------------------------------
 * 亮度页的局部刷新状态
 *
 * 【为什么】原来 bright_draw() 每次都先 TFT_FillRect 整个 240x135 屏（25.9ms @20MHz），
 * 可这一页里**只有数值和分段条会变** —— 标题和太阳图标是静态的。
 * 转一格编码器就白刷一遍全屏，一次 26ms，转起来一卡一卡的。
 *
 * 现在按 ui_carousel 那套做法分开：
 *   * 静态部分（标题 + 太阳图标）只在"页面被切走过"时重画一次
 *     —— 用 UI_Epoch() 判断，UI_Push/UI_Pop 都会让它自增；
 *   * 会变的部分（数值 + 分段条）只在**值真的变了**的时候重画，
 *     面积 166x16 + 166x9 = 4150 像素 = 3.3ms，比全屏省 8 倍。
 *------------------------------------------------------------------------*/
static uint8_t  s_br_shown;         /* 上一次画出来的亮度值（0xFF = 无效，强制重画） */
static uint8_t  s_br_full;          /* 需要重画静态部分 */
static uint32_t s_br_epoch;         /* 上一次绘制时的 UI_Epoch */
static uint8_t  s_br_dirty;         /* 亮度改过、但还没写进 Flash */

/* 数值 + 分段条（会变的部分） */
static void bright_draw_value(uint8_t v)
{
    int16_t digits = (int16_t)((v >= 100u) ? 3 : ((v >= 10u) ? 2 : 1));
    int16_t nw     = (int16_t)(12 * digits);
    int16_t px     = (int16_t)(62 + (166 - nw) / 2);

    /* 数值区：按"最多 3 位"的大小整块清掉再画。
     * 必须清，否则 100 -> 95 时第 3 位会残留在屏上。 */
    TFT_FillRect(62, 56, 166, 16, C_BG);
    Game_DrawNumber(px, 56, v, 2u, C_TEXT, C_BG);

    /* 分段条：UiCard_SegBar 会把 20 段（亮的+灭的）全画一遍，
     * 本身就覆盖整块，所以不用先清。 */
    UiCard_SegBar(62, 96, 166, 9, 20u, (uint8_t)(v / 5u), C_AMBER, C_BG_CARD);
}

/* 静态部分（标题 + 太阳图标） */
static void bright_draw_static(void)
{
    page_title(GTXT_BRIGHT);
    Icon_Draw(12, 50, ICON_BRIGHT, C_AMBER, 30);
}

static void bright_enter(void *arg)
{
    (void)arg;
    RGB_KeyWhite(1u, 1u, 1u);               /* 进页面就全灯白光（文档 (五).2.a） */
    TFT_FillRect(0, 0, UI_SCREEN_W, UI_SCREEN_H, C_BG);   /* 整屏清底：只在进页面时做一次 */
    s_br_full  = 1u;                        /* 静态部分交给 draw 画（此时屏已是底色） */
    s_br_shown = 0xFFu;                     /* 强制重画数值区 */
    UI_Dirty();
}

static ui_nav_t bright_key(const GameKeyEvent_t *ev)
{
    ui_cfg_t *c = UiStore_Cfg();

    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }
    if (is_back(ev)) { return UI_NAV_POP; }
    if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }
    if (c == 0) { return UI_NAV_STAY; }

    /* 【2026-10-14 需求】单击 K5 或单击编码器 -> 回设置列表
     * （存盘在 bright_leave() 里做，这里只管返回） */
    if (is_ok(ev)) { return UI_NAV_POP; }

    if (is_up(ev) || is_down(ev))
    {
        c->led_bright = UiStore_BrightStep(c->led_bright, is_up(ev) ? -1 : +1);

        RGB_SetBrightness(c->led_bright);   /* 灯效立刻生效，不等着存盘 */

        /* 【2026-10-12】不再每格编码器都写 Flash。
         * 原来这里调 UiStore_Save()，而它是"关中断 + 扇区擦除 + 写入"，
         * 一次几十毫秒，期间连 USB 中断都进不来（Code 43 那一类风险）；
         * 转 20 格就是 20 次扇区擦除。改成标脏，离开页面时统一存一次。 */
        s_br_dirty = 1u;

        UI_Dirty();
    }
    return UI_NAV_STAY;
}

/* 离开亮度页：把改过的亮度落盘一次
 *
 * UI_Pop() 和 UI_GoHome() 都会调 ui_call_leave()，所以
 *   * 双击编码器 -> 上一级          （is_back => GKEY_ENC_DBL => UI_NAV_POP）
 *   * 长按 3 秒   -> 直接回主页      （GKEY_ENC_HOLD3 => UI_NAV_HOME => UI_GoHome）
 * 两条退出路径都覆盖到了。
 *
 * 代价：如果停在亮度页上直接断电，最后这次改动会丢（RAM 里是对的，只是没落盘）。 */
static void bright_leave(void)
{
    if (s_br_dirty != 0u)
    {
        s_br_dirty = 0u;
        (void)UiStore_Save();
    }
}

static void bright_draw(void)
{
    ui_cfg_t *c  = UiStore_Cfg();
    uint8_t   v  = (uint8_t)((c != 0) ? c->led_bright : 0u);
    uint32_t  ep = UI_Epoch();

    /* 页面被切走过（UI_Push/UI_Pop 都会让 epoch 自增）-> 原来的屏幕内容不可信，
     * 静态部分必须重画一遍。 */
    if (s_br_epoch != ep)
    {
        s_br_epoch = ep;
        s_br_full  = 1u;
    }

    if (s_br_full != 0u)
    {
        s_br_full  = 0u;
        s_br_shown = 0xFFu;                 /* 静态重画时数值区也一起重画 */
        bright_draw_static();
    }

    /* 值没变就什么都不用画（比如已经在 0 或 100 还继续转编码器） */
    if (s_br_shown == v)
    {
        return;
    }
    s_br_shown = v;

    bright_draw_value(v);
}

const ui_page_t UI_PAGE_BRIGHT = { "bright", bright_enter, bright_leave, bright_key, 0, bright_draw };

/*==========================================================================
 * 3b. 屏幕亮度页（界面与键盘灯亮度页一致）
 *
 * 唯一区别：
 *   * 标题写 GTXT_SCRBRIGHT，图标沿用同一个；
 *   * 进的页面做的是 TPORT_BlSetBright()（BLK = PB4 = PWM7，5kHz），
 *     而不是给灯珠调亮度；
 *   * 落盘字段是 cfg->scr_bright。
 * 刷新策略和亮度页一模一样：静态层只在页面被切走过时重画一次，
 * 每次只刷"数值 + 分段条"那一小块。
 *========================================================================*/
static uint8_t  s_sb_shown;
static uint8_t  s_sb_full;
static uint32_t s_sb_epoch;
static uint8_t  s_sb_dirty;

static void sbright_draw_value(uint8_t v)
{
    int16_t digits = (int16_t)((v >= 100u) ? 3 : ((v >= 10u) ? 2 : 1));
    int16_t nw     = (int16_t)(12 * digits);
    int16_t px     = (int16_t)(62 + (166 - nw) / 2);

    TFT_FillRect(62, 56, 166, 16, C_BG);
    Game_DrawNumber(px, 56, v, 2u, C_TEXT, C_BG);
    UiCard_SegBar(62, 96, 166, 9, 20u, (uint8_t)(v / 5u), C_AMBER, C_BG_CARD);
}

static void sbright_draw_static(void)
{
    page_title(GTXT_SCRBRIGHT);
    Icon_Draw(12, 50, ICON_BRIGHT, C_AMBER, 30);
}

static void sbright_enter(void *arg)
{
    (void)arg;
    TFT_FillRect(0, 0, UI_SCREEN_W, UI_SCREEN_H, C_BG);
    s_sb_full  = 1u;
    s_sb_shown = 0xFFu;
    UI_Dirty();
}

static ui_nav_t sbright_key(const GameKeyEvent_t *ev)
{
    ui_cfg_t *c = UiStore_Cfg();

    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }
    if (is_back(ev)) { return UI_NAV_POP; }
    if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }
    if (c == 0) { return UI_NAV_STAY; }

    /* 【2026-10-14 需求】单击 K5 或单击编码器 -> 回设置列表
     * （存盘在 sbright_leave() 里做，这里只管返回） */
    if (is_ok(ev)) { return UI_NAV_POP; }

    if (is_up(ev) || is_down(ev))
    {
        c->scr_bright = UiStore_ScrBrightStep(c->scr_bright, is_up(ev) ? -1 : +1);

        TPORT_BlSetBright(c->scr_bright);   /* 立刻生效，不等着存盘 */
        s_sb_dirty = 1u;
        UI_Dirty();
    }
    return UI_NAV_STAY;
}

static void sbright_leave(void)
{
    if (s_sb_dirty != 0u)
    {
        s_sb_dirty = 0u;
        (void)UiStore_Save();
    }
}

static void sbright_draw(void)
{
    ui_cfg_t *c  = UiStore_Cfg();
    uint8_t   v  = (uint8_t)((c != 0) ? c->scr_bright : 100u);
    uint32_t  ep = UI_Epoch();

    if (s_sb_epoch != ep)
    {
        s_sb_epoch = ep;
        s_sb_full  = 1u;
    }

    if (s_sb_full != 0u)
    {
        s_sb_full  = 0u;
        s_sb_shown = 0xFFu;
        sbright_draw_static();
    }

    if (s_sb_shown == v)
    {
        return;
    }
    s_sb_shown = v;

    sbright_draw_value(v);
}

const ui_page_t UI_PAGE_SCRBRIGHT = { "scrbr", sbright_enter, sbright_leave,
                                      sbright_key, 0, sbright_draw };


/*==========================================================================
 * 4. 休眠时间页（5/10/20/60min / 永不）
 *========================================================================*/
static void sleep_draw_item(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                            uint8_t idx, uint8_t selected, void *user);

static ui_cardlist_t s_sleep;

static void sleep_enter(void *arg)
{
    (void)arg;
    UiCList_Init(&s_sleep, ICON_SLEEP, sleep_draw_item, 0);
    UiCList_SetCount(&s_sleep, (uint8_t)UI_SLEEP_COUNT);

    {
        ui_cfg_t *c = UiStore_Cfg();
        if (c != 0) { UiCList_SetSel(&s_sleep, c->sleep_idx); }
    }
    UI_Dirty();
}

static ui_nav_t sleep_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }
    if (is_back(ev)) { return UI_NAV_POP; }
    if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }

    if (is_up(ev) || is_down(ev))
    {
        UiCList_Move(&s_sleep, is_up(ev) ? -1 : +1);
        UI_Dirty();
    }
    else if (is_ok(ev) || (ev->key == GKEY_ENC_HOLD))
    {
        ui_cfg_t *c = UiStore_Cfg();
        if (c != 0)
        {
            c->sleep_idx = UiCList_Selected(&s_sleep);   /* 存 Flash，断电不丢 */
            (void)UiStore_Save();
        }
        return UI_NAV_POP;
    }
    return UI_NAV_STAY;
}

/* 列表项画成 "N 分钟" / "永不" */
static void sleep_draw_item(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                            uint8_t idx, uint8_t selected, void *user)
{
    uint16_t fg = (selected != 0u) ? C_TEXT  : C_TEXT_DIM;
    uint16_t bg = (selected != 0u) ? Theme_Blend(C_BG_CARD_HI, C_GREEN, 40) : C_BG_CARD;

    (void)cw; (void)ch; (void)user;
    if (idx == (uint8_t)UI_SLEEP_NEVER)
    {
        Game_DrawText(cx, cy, GTXT_NEVER, fg, bg);
    }
    else
    {
        static const uint16_t mins[4] = { 5u, 10u, 20u, 60u };
        Game_DrawNumber(cx, cy, mins[idx], 1u, fg, bg);
        Game_DrawText((int16_t)(cx + 20), cy, GTXT_MIN, fg, bg);
    }
}

static void sleep_draw(void)
{
    if (UiCList_TakeBg(&s_sleep) != 0u)
    {
        page_bg();
        page_title(GTXT_SLEEP);
        UiCList_Draw(&s_sleep);
    }
    else
    {
        UiCList_DrawText(&s_sleep);
    }
}

const ui_page_t UI_PAGE_SLEEP = { "sleep", sleep_enter, 0, sleep_key, 0, sleep_draw };

/*==========================================================================
 * 5. 恢复默认确认页（是 / 否）
 *========================================================================*/
static const uint8_t s_reset_texts[2] = { GTXT_YES, GTXT_NO };
static const clist_text_src_t s_reset_src = { s_reset_texts, 0 , 0, 0 };
static ui_cardlist_t s_reset;

static void reset_enter(void *arg)
{
    (void)arg;
    UiCList_Init(&s_reset, ICON_REFRESH, clist_text_draw, (void *)&s_reset_src);
    UiCList_SetCount(&s_reset, 2u);
    UI_Dirty();
}

static ui_nav_t reset_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }
    if (is_back(ev)) { return UI_NAV_POP; }
    if (ev->key == GKEY_ENC_HOLD3) { return UI_NAV_HOME; }

    /* 【修复 §8-9】确认优先，且三档改成"各自 return"的串行判断。
     *
     * 原来是 is_up / is_down / is_ok 三个并列的 if，而 GKEY_UP 与 GKEY_OK
     * 同为 K5 —— 停在"否"上按一下 K5，会先执行"上移到是"，紧接着又落进
     * 确认分支，等于一键恢复出厂设置。方向键也已不再包含 K5（见 is_up）。 */
    if (is_ok(ev) || (ev->key == GKEY_ENC_HOLD))
    {
        if (UiCList_Selected(&s_reset) == 0u)    /* 是：一切回默认 */
        {
            UiStore_Default();
            (void)UiStore_Save();

            /* 【修复 §8-5】需求 (五).4.b：恢复默认要连**自定义键值映射**一起复位。
             * 原来只切回第 0 组 —— 网页改过的映射还留在 RAM 和 Flash 0x0400 里，
             * 断电重启又回来了。KmapStore_ResetDefault() 把映射表 + 组名 + 当前组
             * 一起复位并落盘，等价于网页上那条"恢复出厂默认"。 */
            (void)KmapStore_ResetDefault();

            /* 【2026-10-14】恢复默认也要清掉 BLE 绑定信息。
             * 原来只复位设置 + 键值映射，绑定一直留着 —— 设备端和主机端
             * 密钥一旦不一致（反复烧录整片擦除后就是这样），就会陷入
             * 『回连 -> 加密失败 -> 拆链 -> 重播 -> 回连』的死循环。
             * 用户按『恢复默认』就是要一个干净状态，绑定也该算在里面。 */
            BT_WCH_EraseAllBonds();

            {
                ui_cfg_t *c = UiStore_Cfg();
                if (c != 0)
                {
                    RGB_Switch(c->led_on);
                    RGB_SetBrightness(c->led_bright);
                }
            }
        }
        return UI_NAV_POP;
    }

    if (is_up(ev))   { UiCList_Move(&s_reset, -1); UI_Dirty(); return UI_NAV_STAY; }
    if (is_down(ev)) { UiCList_Move(&s_reset, +1); UI_Dirty(); return UI_NAV_STAY; }

    return UI_NAV_STAY;
}

static void reset_draw(void)
{
    if (UiCList_TakeBg(&s_reset) != 0u)
    {
        page_bg();
        page_title(GTXT_RESET);
        UiCList_Draw(&s_reset);
    }
    else
    {
        UiCList_DrawText(&s_reset);
    }
}

const ui_page_t UI_PAGE_RESET = { "reset", reset_enter, 0, reset_key, 0, reset_draw };

/******************* (C) CH592X UI 页面 *****************/
