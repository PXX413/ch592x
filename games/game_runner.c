/**************************************************************************
 * game_runner.c —— CH592X 横版跑酷实现
 *
 * 物理量都用"千分之一像素"的定点数，避免用浮点（RISC-V 上没 FPU）：
 *     vy 单位 = 毫像素/毫秒，位置单位 = 毫像素
 **************************************************************************/

#include "game_runner.h"
#include "game_state.h"
#include "game_score.h"

/*==========================================================================
 * 参数
 *========================================================================*/
/* 【2026-10-15 需求】重力从 12/5（2.4）降到 10/5（2.0）：滞空时间从约 300ms
 * 拉到约 360ms，单跳最高点从 27px 抬到 32px（原来 27px 只比 26px 高的障碍
 * 高出 1 像素，几乎非二段跳不可）。想再柔和一点只改这两个宏即可。 */
#define RUN_G_NUM       10          /* 重力 = dt * 10/5 = dt * 2.0（毫像素/毫秒^2） */
#define RUN_G_DEN       5

#define RUN_JUMP_V      (-360)      /* 起跳速度（毫像素/毫秒）= 360 像素/秒 */
#define RUN_JUMP2_V     (-330)      /* 二段跳 */
#define RUN_MAX_FALL    (-520)      /* 最大下落速度（负数上限，取绝对值） */

#define RUN_SPEED_MIN   80          /* 初始速度（像素/秒） */
/* 【2026-10-15 需求】速度改成"只跟时间走"：起始 80 像素/秒，每 5 秒 +1，
 * 上限 220，不再和分数挂钩。原来的算法（ACC 时代）里分数涨得越快速度涨得
 * 越快（分数增速 = 速度/10），速度是指数式往上冲，两三秒就顶满，难度失控。
 * 冲刺（K7）再乘 1.5，用 RUN_SPEED_HARD_MAX 兜住绝对上限。 */
#define RUN_SPEED_MAX   220         /* 速度上限（像素/秒） */
#define RUN_SPEED_STEP_MS 5000u     /* 每 5 秒 +1 像素/秒 */
#define RUN_SPEED_HARD_MAX 260      /* 冲刺乘 1.5 之后的绝对上限 */

/* 【2026-10-15 需求】冲刺技能（KEY6）：按下后向前突进一个障碍物宽度，
 * 两层（可以连按两次），冷却 1 秒。规则见 run_dash_try()。
 *
 * 【实现方式】人物不做任何水平移动（sprite 一直钉在 RUN_PLAYER_X），冲刺是靠
 * "障碍物、地面虚线在这 RUN_DASH_ACT_MS 里加速左移"表现的：把 RUN_DASH_PX
 * 像素摊到动作窗口里，每帧按 dt 分一份推出去，总距离正好是 RUN_DASH_PX
 * （需要穿透时更多，见 run_dash_shift()）。冲刺推出去的这段距离不加分
 * （不动 s_dist_m）。
 * RUN_DASH_PASS = 1 时动作窗口内不判碰撞（否则"穿过去"半路就被压死了）；
 * 改成 0 = 普通加速，撞上照样算撞。 */
#define RUN_DASH_PX        12       /* 一次冲刺前进的距离（像素）：障碍宽 10/12/14 */
#define RUN_DASH_LAYERS    2u       /* 层数：可以连冲两次 */
#define RUN_DASH_CD_MS     1000u    /* 冷却：恢复满层要等的时间 */
#define RUN_DASH_ACT_MS    150u     /* 冲刺动作时长：额外距离按 dt 摊在这段时间里 */
#define RUN_DASH_PASS      1

/* 【需求 2】障碍间隔改成"按时间"算（见 run_spawn_next()）：
 * 原来固定 70~140 像素，速度 300 像素/秒时只有 0.23~0.47 秒 —— 连着来
 * 两个高障碍时玩家还在空中、落地都来不及，就是"完全无法躲避"。 */
#define RUN_GAP_MS_MIN  700         /* 两个障碍之间至少隔多少毫秒 */
#define RUN_GAP_MS_RAND 300
#define RUN_GAP_PX_MIN  96          /* 低速时的像素下限（别贴到一起） */

#define RUN_GROUNL_Y    RUN_GROUND_Y

#define RUN_COL_SKY     0xB6BEu     /* 亮色系：浅天蓝 */
#define RUN_COL_FAR     0xCE79u     /* 远山/地平线：浅灰蓝 */
#define RUN_COL_GROUND  0xC510u     /* 地面：暖沙色 */
#define RUN_COL_PLAYER  0x1AF4u     /* 玩家：深蓝（黄在亮底看不清） */
#define RUN_COL_OBS     0xC000u     /* 障碍：深红 */
#define RUN_COL_TEXT    0x4208u     /* 正文：深灰 */
#define RUN_COL_LABEL   0xBBA2u     /* 标签：琥珀 */
#define RUN_COL_NUM     0x03C0u     /* 数值：深绿 */
#define RUN_COL_DASH_ON 0x07E0u     /* 冲刺层数「还有」：亮绿 */

/* 【结束页升级】标题用深红、最高分用深蓝（和 2048 / 雷霆战机一致） */
#define RUN_COL_OVER    0xC000u     /* 结束标题：深红 */
#define RUN_COL_BEST    0x3193u     /* 最高分：深蓝 */

/*==========================================================================
 * 障碍
 *========================================================================*/
/* RunObs_t 现在在 game_runner.h 里（共享池需要看到它） */

/* 改成共享池（原来自开 RUN_MAX_OBS 个） */
static RunObs_t *s_obs;

/*==========================================================================
 * 状态
 *========================================================================*/
static int32_t  s_py_m;             /* 玩家底部 y（毫像素） */
static int32_t  s_vy;               /* 垂直速度（毫像素/毫秒） */
static bool     s_grounded;
static bool     s_jump2_used;
static int32_t  s_dx_m;             /* 本帧要移动的像素（毫像素） */
static int32_t  s_dist_m;           /* 总距离（毫像素） */
static int32_t  s_next_gap_m;       /* 下一个障碍还要跑多少毫像素才出现 */
static int32_t  s_scroll_m;         /* 地面虚线的滚动相位 */
static uint16_t s_speed;            /* 像素/秒 */
static bool     s_sprint;            /* key7 held: 1.5x speed */
static uint32_t s_score;
static uint32_t s_elapsed_ms;        /* 开局到现在的有效时间（暂停不计）；速度按它涨 */
static uint8_t  s_dash_layers;       /* 冲刺技能剩余层数（0 = 两层用完，正在冷却） */
static uint32_t s_dash_cd_ms;        /* 冷却剩余时间（到点回满两层） */
static uint32_t s_dash_ms;           /* 冲刺动作剩余时间（>0 = 正把障碍物加速左移） */
static int32_t  s_dash_extra_m;      /* 还要额外推出去多少毫像素（窗口里按 dt 分摊） */
static bool     s_sliding;
static bool     s_over;
static bool     s_paused;

static bool     s_need_full;
static bool     s_hud_dirty;
/* 【性能】HUD 增量重画用的缓存：静态底/标签只画一次，数字只画变了的那几位 */
static bool     s_hud_ready;
static uint32_t s_hud_score;
static uint16_t s_hud_speed;
static uint8_t  s_hud_layers;        /* 上一次画的冲刺层数 */
static bool     s_over_drawn;
static int16_t  s_draw_py;          /* 上一次画玩家的位置 */
static int16_t  s_draw_ph;
static bool     s_draw_valid;

/*==========================================================================
 * 小工具
 *========================================================================*/
static int16_t run_player_h(void)
{
    return s_sliding ? RUN_SLIDE_H : RUN_STAND_H;
}

static int16_t run_player_top(void)
{
    /* 玩家底部不能低于地面 */
    int32_t bottom = s_py_m / 1000;
    return (int16_t)(bottom - run_player_h());
}

/* 画到屏幕里（自动裁剪）——障碍是从右边滑进来的，不能直接画到屏幕外 */
static void run_fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) { return; }
    if (x < 0)            { w = (int16_t)(w + x); x = 0; }
    if (y < 0)            { h = (int16_t)(h + y); y = 0; }
    if (x >= RUN_W || y >= RUN_H) { return; }
    if ((int16_t)(x + w) > RUN_W) { w = (int16_t)(RUN_W - x); }
    if ((int16_t)(y + h) > RUN_H) { h = (int16_t)(RUN_H - y); }
    if (w <= 0 || h <= 0) { return; }
    TFT_FillRect(x, y, w, h, color);
}
static void run_calc_speed(void)
{
    /* 【2026-10-15 需求】速度只随时间涨：80 + 有效时间/5 秒，上限 220 */
    uint32_t sp = (uint32_t)RUN_SPEED_MIN + (s_elapsed_ms / RUN_SPEED_STEP_MS);
    uint8_t  diff = GS_GetDifficulty();

    if (sp > RUN_SPEED_MAX) { sp = RUN_SPEED_MAX; }

    if (diff == (uint8_t)GAME_DIFF_EASY) { sp = sp * 4u / 5u; }        /* 慢 20% */
    if (diff == (uint8_t)GAME_DIFF_HARD) { sp = sp * 5u / 4u; }        /* 快 25% */
    if (sp > RUN_SPEED_MAX) { sp = RUN_SPEED_MAX; }
    if (sp < 40u) { sp = 40u; }

    if (s_sprint)                                       /* sprint: 1.5x */
    {
        sp = (uint32_t)((sp * 3u) / 2u);
    }
    /* 【需求 3】冲刺乘完再兜一次绝对上限（以前没有这一下，实际能冲到 450） */
    if (sp > (uint32_t)RUN_SPEED_HARD_MAX) { sp = (uint32_t)RUN_SPEED_HARD_MAX; }
    s_speed = (uint16_t)sp;
}

static void run_spawn_next(void)
{
    uint8_t kind = (uint8_t)Game_RandRange(3u);
    int16_t x = RUN_W + 4;
    int16_t w, h, y;
    uint8_t i;

    if (kind == 0u)         { w = 10; h = 12; y = (int16_t)(RUN_GROUND_Y - 12); }     /* 矮：跳 */
    else if (kind == 1u)    { w = 12; h = 26; y = (int16_t)(RUN_GROUND_Y - 26); }     /* 高：二段跳 */
    else                    { w = 14; h = 10; y = 84; }                               /* 悬空：滑铲 */

    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        if (!s_obs[i].active)
        {
            s_obs[i].x = x;
            s_obs[i].y = y;
            s_obs[i].w = w;
            s_obs[i].h = h;
            s_obs[i].active = true;
            s_obs[i].drawn  = false;
            s_obs[i].px     = x;
            s_obs[i].py     = y;
            break;
        }
    }

    /* 【需求 2】按时间留间隔：同样的毫秒数，速度越高换算出的像素越多，
     * 留给玩家的反应/落地时间才是恒定的。像素下限兜底，免得低速时贴一起。 */
    {
        int32_t gap_px = ((int32_t)s_speed
                          * (int32_t)(RUN_GAP_MS_MIN + (uint32_t)Game_RandRange(RUN_GAP_MS_RAND)))
                         / 1000;

        if (gap_px < (int32_t)RUN_GAP_PX_MIN) { gap_px = (int32_t)RUN_GAP_PX_MIN; }
        s_next_gap_m = gap_px * 1000;
    }
}

/*==========================================================================
 * 【2026-10-15 需求】冲刺技能（KEY6）
 *
 * 按下 K6：人物向前突进一个障碍物宽度（RUN_DASH_PX = 12 像素）。
 * 两层，可以连按两次；按完开始计时 RUN_DASH_CD_MS（1 秒）：
 *   - 1 秒内再按一次 -> 用掉第二层，计时重新开始；
 *   - 1 秒内没有第二下 -> 冷却到点，回到两层；
 *   - 冷却到点 -> 同样回到两层。
 * 所以任何一个 1 秒窗口里最多连冲两次。
 *========================================================================*/
static int32_t run_dash_shift(void)
{
    int32_t shift = RUN_DASH_PX;
#if (RUN_DASH_PASS)
    int16_t py0 = run_player_top();
    int16_t py1 = (int16_t)(s_py_m / 1000 - 1);
    uint8_t i;

    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        int32_t nx;

        if (!s_obs[i].active) { continue; }

        nx = (int32_t)s_obs[i].x - shift;
        if ((nx + (int32_t)s_obs[i].w) <= (int32_t)RUN_PLAYER_X) { continue; }     /* 已经到身后 */
        if (nx > (int32_t)(RUN_PLAYER_X + RUN_PLAYER_W - 1))     { continue; }     /* 还在前面 */
        if ((py1 < s_obs[i].y) ||
            (py0 > (int16_t)(s_obs[i].y + s_obs[i].h - 1)))      { continue; }     /* 高度不重叠 */

        /* 横向重叠 -> 继续往前推，直到它整块移到玩家左边（留 1 像素余量） */
        nx = (int32_t)s_obs[i].x + (int32_t)s_obs[i].w - (int32_t)RUN_PLAYER_X + 1;
        if (nx > shift) { shift = nx; }
    }
#endif
    return shift;
}

static void run_dash_try(void)
{
    int32_t shift;

    if (s_over || s_paused) { return; }
    if (s_dash_layers == 0u) { return; }            /* 两层都用完，等冷却 */

    s_dash_layers--;
    s_dash_cd_ms = RUN_DASH_CD_MS;
    s_dash_ms    = RUN_DASH_ACT_MS;
    s_hud_dirty  = true;

    /* 【冲刺实现】不瞬移：把要额外推进的距离记下来，交给 RUN_Update() 在
     * RUN_DASH_ACT_MS 里按 dt 摊成"障碍物加速左移"，人物水平方向一动不动。 */
    shift = run_dash_shift();
    s_dash_extra_m = shift * 1000;
}

static bool run_hit(void)
{
    int16_t px0 = RUN_PLAYER_X;
    int16_t px1 = (int16_t)(RUN_PLAYER_X + RUN_PLAYER_W - 1);
    int16_t py0 = run_player_top();
    int16_t py1 = (int16_t)(s_py_m / 1000 - 1);
    uint8_t i;

#if (RUN_DASH_PASS)
    /* 【冲刺实现】人物不动、障碍物被加速推过去：动作窗口内不判碰撞，否则
     * 半路就被压在障碍里了。窗口最后一帧 s_dash_ms 已归零、障碍也正好整块
     * 挪到身后（余量由 run_dash_shift() 多留 1 像素），这一帧正常判定。 */
    if (s_dash_ms > 0u) { return false; }
#endif

    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        if (!s_obs[i].active) { continue; }

        if (px1 >= s_obs[i].x && px0 <= (int16_t)(s_obs[i].x + s_obs[i].w - 1) &&
            py1 >= s_obs[i].y && py0 <= (int16_t)(s_obs[i].y + s_obs[i].h - 1))
        {
            return true;
        }
    }
    return false;
}

/*==========================================================================
 * 画
 *========================================================================*/
/*==========================================================================
 * 【性能】HUD 增量重画
 *
 * 原来 run_draw_hud() 一被调用就把两块 118/120x18 的底 + 两个 16x16 汉字标签
 * + 分数/速度的数字全部重画一遍。而 scale=2 的数字每个字符要发 40~80 次
 * TFT_FillRect（每一次都带一整套地址窗口命令），单次 HUD 刷新约 10 ms；分数
 * 每 10 像素涨 1 分，200 像素/秒时每秒刷 20 次 => 每秒约 200 ms 花在这里。
 *
 * 现在：静态部分（底色 + 标签）只在全屏重画后画一次；数字只重画"变了的那几位"。
 *========================================================================*/
static uint8_t run_digit_count(uint32_t v)
{
    uint8_t n = 1u;

    while (v >= 10u)
    {
        v /= 10u;
        n++;
    }
    return n;
}

/* scale=2 时每个数字占 12 像素宽；x0 = 数字区左边界 */
static void run_hud_number(int16_t x0, uint32_t old_v, uint32_t new_v, uint16_t fg)
{
    uint8_t  nd_old = run_digit_count(old_v);
    uint8_t  nd_new = run_digit_count(new_v);
    uint8_t  nd = (uint8_t)((nd_old > nd_new) ? nd_old : nd_new);
    uint8_t  first = nd;                /* 从左边数第一个不一样的位 */
    uint32_t a = old_v;
    uint32_t b = new_v;
    uint8_t  i;

    for (i = 0u; i < nd; i++)
    {
        if ((a % 10u) != (b % 10u))
        {
            first = (uint8_t)(nd - 1u - i);
        }
        a /= 10u;
        b /= 10u;
    }
    if (first >= nd)
    {
        return;                         /* 数字没变，这一帧不画 */
    }

    /* 只清掉变化的那几位，再逐位重画（逐位是为了把中间的 0 也画出来） */
    run_fill((int16_t)(x0 + (int16_t)(12 * first)), 1,
             (int16_t)(12 * (nd - first)), 16, RUN_COL_SKY);

    TFT_SetTextSize(2u);
    TFT_SetTextColorBg(fg, RUN_COL_SKY);
    for (i = first; i < nd; i++)
    {
        uint32_t p = 1u;
        uint8_t  k;

        for (k = 0u; k < (uint8_t)(nd - 1u - i); k++)
        {
            p *= 10u;
        }
        TFT_SetCursor((int16_t)(x0 + (int16_t)(12 * i)), 1);
        TFT_Printf("%u", (unsigned int)((new_v / p) % 10u));
    }
}

/* 【需求】冲刺层数指示：顶栏中间两个小方块（亮 = 还能冲，暗 = 已用掉） */
static void run_draw_dash(void)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)RUN_DASH_LAYERS; i++)
    {
        run_fill((int16_t)(104 + ((int16_t)i * 12)), 6, 6, 6,
                 (i < s_dash_layers) ? RUN_COL_DASH_ON : RUN_COL_LABEL);
    }
    s_hud_layers = s_dash_layers;
}

static void run_draw_hud(void)
{
    if (!s_hud_ready)
    {
        /* 【需求 4】数字从 scale 1（6x8，半个汉字高）放大到 scale 2（12x16，
         * 正好和 16 像素高的汉字等高）。清屏矩形跟着加宽，否则位数变少时
         * 右边会留下一截旧数字。 */
        TFT_FillRect(0, 0, 118, 18, RUN_COL_SKY);
        Game_DrawText(2, 1, GTXT_SCORE, RUN_COL_LABEL, RUN_COL_SKY);
        TFT_FillRect(120, 0, 120, 18, RUN_COL_SKY);
        Game_DrawText(152, 1, GTXT_SPEED, RUN_COL_LABEL, RUN_COL_SKY);

        Game_DrawNumber(36, 1, s_score, 2u, RUN_COL_NUM, RUN_COL_SKY);
        Game_DrawNumber(186, 1, (uint32_t)s_speed, 2u, RUN_COL_TEXT, RUN_COL_SKY);

        s_hud_score = s_score;
        s_hud_speed = s_speed;
        s_hud_ready = true;
        run_draw_dash();
        return;
    }

    run_hud_number(36, s_hud_score, s_score, RUN_COL_NUM);
    run_hud_number(186, (uint32_t)s_hud_speed, (uint32_t)s_speed, RUN_COL_TEXT);
    s_hud_score = s_score;
    s_hud_speed = s_speed;
    if (s_hud_layers != s_dash_layers) { run_draw_dash(); }
}

static void run_draw_player(int16_t top, int16_t h)
{
    /* 【冲刺实现】人物不参与水平移动，永远画在 RUN_PLAYER_X */
    run_fill(RUN_PLAYER_X, top, RUN_PLAYER_W, h, RUN_COL_PLAYER);
}

static void run_erase_player(void)
{
    if (s_draw_valid)
    {
        run_fill(RUN_PLAYER_X, s_draw_py, RUN_PLAYER_W, s_draw_ph, RUN_COL_SKY);
    }
}

static void run_draw_all(void)
{
    uint8_t i;
    int16_t y;

    Game_Clear(RUN_COL_SKY);

    s_hud_ready = false;            /* 全屏重画后 HUD 的底和标签要重画一次 */

    /* 【需求 5】原来这里有一条 16 像素高的"远山/地平线"色带（y=92..107）。
     * 障碍走掉的时候是用天空色擦的（见 RUN_Draw()），每过一个障碍就在那条
     * 色带上啃掉一块、再也补不回来 —— 看上去就是"色条被障碍冲掉"。直接
     * 不要这条色带，天空留干净；RUN_COL_FAR 还给地面虚线用。 */

    /* 地面 */
    run_fill(0, RUN_GROUND_Y, RUN_W, (int16_t)(RUN_H - RUN_GROUND_Y), RUN_COL_GROUND);

    /* 障碍 */
    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        if (s_obs[i].active)
        {
            run_fill(s_obs[i].x, s_obs[i].y, s_obs[i].w, s_obs[i].h, RUN_COL_OBS);
        }
    }

    /* 玩家 */
    y = run_player_top();
    run_draw_player(y, run_player_h());
    s_draw_py = y;
    s_draw_ph = run_player_h();
    s_draw_valid = true;

    run_draw_hud();
    Game_DrawHintBar(GTXT_JUMP, RUN_COL_TEXT, 0xE71Cu);

    s_need_full = false;
    s_hud_dirty = false;
}

/* 【结束页升级】结束页：先把整屏清干净，再画标题 / 本局分数 / 最高分 /
 * 提示各一行（版式和 2048、雷霆战机一样）。以前只是在还在跑动的画面上
 * 盖一行字，看着像游戏没停下来。 */
static void run_draw_end(void)
{
    uint32_t best = GS_GetBest((uint8_t)GAME_ID_RUNNER);
    int16_t  lw;
    int16_t  nw;
    int16_t  x;

    Game_Clear(RUN_COL_SKY);

    lw = Game_TextWidth(GTXT_GAME_OVER);
    Game_DrawText((int16_t)((RUN_W - lw) / 2), 14, GTXT_GAME_OVER, RUN_COL_OVER, RUN_COL_SKY);

    lw = Game_TextWidth(GTXT_SCORE);
    nw = Game_NumberWidth(s_score, 2u);
    x  = (int16_t)((RUN_W - (lw + 8 + nw)) / 2);
    Game_DrawText(x, 46, GTXT_SCORE, RUN_COL_LABEL, RUN_COL_SKY);
    Game_DrawNumber((int16_t)(x + lw + 8), 46, s_score, 2u, RUN_COL_NUM, RUN_COL_SKY);

    lw = Game_TextWidth(GTXT_BEST);
    nw = Game_NumberWidth(best, 2u);
    x  = (int16_t)((RUN_W - (lw + 8 + nw)) / 2);
    Game_DrawText(x, 72, GTXT_BEST, RUN_COL_LABEL, RUN_COL_SKY);
    Game_DrawNumber((int16_t)(x + lw + 8), 72, best, 2u, RUN_COL_BEST, RUN_COL_SKY);

    lw = Game_TextWidth(GTXT_ANY_KEY);
    Game_DrawText((int16_t)((RUN_W - lw) / 2), 104, GTXT_ANY_KEY, RUN_COL_LABEL, RUN_COL_SKY);

    s_over_drawn = true;
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
void RUN_Enter(void)
{
    /* 共享池是按需申请的：进游戏时自己拿，
     * 不指望上层已经帮我拿过。拿不到就什么都不画。 */
    if (!Game_PoolAcquire())
    {
        return;
    }

    s_obs = Game_PoolObs();

    uint8_t i;

    Game_SetOrientation(GAME_LANDSCAPE);
    GS_GetDifficulty();

    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        s_obs[i].active = false;
        s_obs[i].drawn  = false;
        s_obs[i].x = 0;
        s_obs[i].y = 0;
        s_obs[i].w = 0;
        s_obs[i].h = 0;
    }

    s_py_m      = (int32_t)RUN_GROUND_Y * 1000;
    s_vy        = 0;
    s_grounded  = true;
    s_jump2_used = false;
    s_dx_m      = 0;
    s_dist_m    = 0;
    s_scroll_m  = 0;
    s_score     = 0u;
    s_elapsed_ms = 0u;
    s_dash_layers = (uint8_t)RUN_DASH_LAYERS;
    s_dash_cd_ms  = 0u;
    s_dash_ms     = 0u;
    s_dash_extra_m = 0;
    s_sliding   = false;
    s_over      = false;
    s_paused    = false;
    s_sprint    = false;
    s_over_drawn = false;
    s_draw_valid = false;
    s_need_full  = true;
    s_hud_dirty  = true;

    run_calc_speed();

    /* 开局先跑一段再出障碍，给玩家反应时间 */
    s_next_gap_m = 150000;

    run_draw_all();
}

bool RUN_HandleKey(const GameKeyEvent_t *ev)
{
    if (ev == 0)
    {
        return false;
    }

    /* 退出：编码器双击（文档「（三）2」） */
    if ((ev->key == GKEY_ENC_DBL) && (ev->type == GAME_EV_PRESS))
    {
        return true;
    }

    if (s_over)
    {
        /* 【结束页升级】结束页上写的是"任意键"，这里就真的任意键重开：
         * 编码器双击（退出）在上面已经拦掉了；HOLD/HOLD1/HOLD3 是框架
         * 合成的长按事件、不是"按了一下键"，排除掉。 */
        if ((ev->type == GAME_EV_PRESS) && (ev->key < (uint8_t)GKEY_ENC_HOLD))
        {
            RUN_Enter();
        }
        return false;
    }

    switch ((int)ev->key)
    {
        /* 【需求】KEY2 和 KEY5 当成同一个键：
         *   站在地上点一下（任意一个）= 起跳；
         *   还在空中再点一下（任意一个）= 二段跳（一次）。
         * "短时间"就是这个意思 —— 起跳后滞空只有约 300ms，落地后
         * s_jump2_used 复位，再点就是新的一次起跳，不需要另加计时器。 */
        case GKEY_K2:
        case GKEY_K5:
            if (ev->type == GAME_EV_PRESS)
            {
                if (s_grounded)
                {
                    s_vy = RUN_JUMP_V;
                    s_grounded = false;
                    s_jump2_used = false;
                    s_sliding = false;
                }
                else if (!s_jump2_used)
                {
                    s_vy = RUN_JUMP2_V;
                    s_jump2_used = true;
                }
            }
            break;

        case GKEY_K9:                       /* 老键位：只当二段跳用，先留着 */
            if ((ev->type == GAME_EV_PRESS) && !s_grounded && !s_jump2_used)
            {
                s_vy = RUN_JUMP2_V;
                s_jump2_used = true;
            }
            break;

        case GKEY_K8:                       /* 滑铲（按住）；空中按下 = 立刻落地 */
            if (ev->type == GAME_EV_RELEASE)
            {
                s_sliding = false;
            }
            else
            {
                if (!s_grounded)
                {
                    /* 【2026-10-15 需求】空中按下蹲键立刻回到地面：直接把位置
                     * 归位、垂直速度清零，不再等重力慢慢把人拉下来（悬在空中
                     * 那段时间正好会撞上障碍）。落地后接着滑铲。 */
                    s_py_m = (int32_t)RUN_GROUND_Y * 1000;
                    s_vy = 0;
                    s_grounded = true;
                    s_jump2_used = false;
                }
                s_sliding = true;
            }
            break;

        case GKEY_K6:                       /* 冲刺技能（两层，见 run_dash_try） */
            if (ev->type == GAME_EV_PRESS)
            {
                run_dash_try();
            }
            break;

        case GKEY_K7:                       /* 按住加速（1.5 倍速） */
            s_sprint = (ev->type != GAME_EV_RELEASE);
            break;

        case GKEY_ENC_CLICK:                /* 暂停 / 继续 */
            if (ev->type == GAME_EV_PRESS)
            {
                s_paused = !s_paused;
                s_hud_dirty = true;
                /* 【2026-10-15】暂停/继续都整屏重画一次：暂停时把"暂停"两个字
                 * 画上去，继续时把它擦掉（否则那两个字会一直留在天上）。 */
                s_need_full = true;
            }
            break;

        default:
            break;
    }

    return false;
}

void RUN_Update(uint16_t dt_ms)
{
    uint8_t i;
    int32_t dy;

    if (s_over || s_paused || dt_ms == 0u)
    {
        return;
    }

    /* 【2026-10-15 需求】冲刺技能计时 + 动作：
     * 动作窗口里把 s_dash_extra_m 按 dt 摊出去（= 障碍物加速左移，人物不动），
     * 最后一步把余量一次给完，保证总距离精确；这段距离不加分（不动 s_dist_m）。 */
    if (s_dash_ms > 0u)
    {
        uint32_t step;

        if (s_dash_ms <= (uint32_t)dt_ms)
        {
            step = (uint32_t)s_dash_extra_m;        /* 收尾：剩下的全推出去 */
        }
        else
        {
            step = (uint32_t)((uint32_t)s_dash_extra_m * (uint32_t)dt_ms / RUN_DASH_ACT_MS);
        }
        s_dx_m     += (int32_t)step;
        s_scroll_m += (int32_t)step;
        s_dash_extra_m -= (int32_t)step;
        s_dash_ms -= (s_dash_ms > (uint32_t)dt_ms) ? (uint32_t)dt_ms : s_dash_ms;
    }
    if (s_dash_cd_ms > 0u)
    {
        s_dash_cd_ms -= (s_dash_cd_ms > (uint32_t)dt_ms) ? (uint32_t)dt_ms : s_dash_cd_ms;
        if (s_dash_cd_ms == 0u)
        {
            s_dash_layers = (uint8_t)RUN_DASH_LAYERS;
            s_hud_dirty = true;
        }
    }

    /* ---- 1. 跳跃物理 ---- */
    if (!s_grounded)
    {
        s_vy += (int32_t)((int32_t)dt_ms * RUN_G_NUM / RUN_G_DEN);
        if (s_vy > -RUN_MAX_FALL) { s_vy = -RUN_MAX_FALL; }
    }
    else
    {
        /* 站在地上的时候按着 K8 就是滑铲，松开就站起来 */
        s_sliding = Game_KeyDown(GKEY_K8);
    }

    dy = s_vy * (int32_t)dt_ms;
    s_py_m += dy;

    if (s_py_m >= (int32_t)RUN_GROUND_Y * 1000)
    {
        s_py_m = (int32_t)RUN_GROUND_Y * 1000;
        s_vy = 0;
        if (!s_grounded)
        {
            s_grounded = true;
            s_jump2_used = false;
        }
    }
    else
    {
        s_grounded = false;
    }

    /* ---- 2. 世界前进 ---- */
    /* 【2026-10-15 需求】每帧累加有效时间（暂停/结束时函数开头就 return 了，
     * 不会累加），速度每 5 秒 +1 像素/秒。 */
    s_elapsed_ms += (uint32_t)dt_ms;
    run_calc_speed();
    s_dx_m += (int32_t)s_speed * (int32_t)dt_ms;         /* 毫像素 */
    s_dist_m += (int32_t)s_speed * (int32_t)dt_ms;
    s_scroll_m += (int32_t)s_speed * (int32_t)dt_ms;

    {
        int16_t shift = (int16_t)(s_dx_m / 1000);
        if (shift > 0)
        {
            s_dx_m -= (int32_t)shift * 1000;

            for (i = 0; i < RUN_MAX_OBS; i++)
            {
                if (!s_obs[i].active) { continue; }
                s_obs[i].x = (int16_t)(s_obs[i].x - shift);
                if (s_obs[i].x + s_obs[i].w < 0)
                {
                    s_obs[i].active = false;    /* 出屏了；旧位置由 Draw 擦掉 */
                }
            }

            /* 出新的障碍 */
            s_next_gap_m -= (int32_t)shift * 1000;
            if (s_next_gap_m <= 0)
            {
                run_spawn_next();
            }
        }
    }

    /* ---- 3. 分数 ---- */
    {
        uint32_t s = (uint32_t)(s_dist_m / 10000);       /* 每 10 像素 1 分 */
        if (s != s_score)
        {
            s_score = s;
            s_hud_dirty = true;
            run_calc_speed();
        }
    }

    /* ---- 4. 碰撞 ---- */
    if (run_hit())
    {
        s_over = true;
        /* 【结束页升级】结束前存一次最高分（Game Score 只在破纪录时才写 Flash） */
        (void)GS_SetBest((uint8_t)GAME_ID_RUNNER, s_score);
    }
}

void RUN_Draw(void)
{
    uint8_t i;
    int16_t top;
    int16_t h;

    /* 【结束页升级】结束页独占整屏：只画一次，之后每帧直接返回
     * （2048 和雷霆战机也是这个结构） */
    if (s_over)
    {
        if (!s_over_drawn)
        {
            run_draw_end();
        }
        return;
    }

    if (s_need_full)
    {
        run_draw_all();
        return;
    }

    /* 地面虚线滚动（擦掉旧的、画新的） */
    {
        int16_t phase = (int16_t)((s_scroll_m / 1000) % 40);
        int16_t x;

        /* 先把上一帧的虚线擦掉：整条地面的上半部分很窄，代价很小 */
        run_fill(0, (int16_t)(RUN_GROUND_Y + 2), RUN_W, 2, RUN_COL_GROUND);
        for (x = (int16_t)(-phase); x < RUN_W; x = (int16_t)(x + 40))
        {
            if (x >= 0)
            {
                run_fill(x, (int16_t)(RUN_GROUND_Y + 2), 10, 2, RUN_COL_FAR);
            }
        }
    }

    /* 障碍：先把上一次画的位置擦掉，再画到新位置 */
    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        if (s_obs[i].drawn)
        {
            run_fill(s_obs[i].px, s_obs[i].py, s_obs[i].w, s_obs[i].h, RUN_COL_SKY);
            s_obs[i].drawn = false;
        }

        if (!s_obs[i].active) { continue; }

        run_fill(s_obs[i].x, s_obs[i].y, s_obs[i].w, s_obs[i].h, RUN_COL_OBS);
        s_obs[i].px    = s_obs[i].x;
        s_obs[i].py    = s_obs[i].y;
        s_obs[i].drawn = true;
    }

    /* 玩家 */
    run_erase_player();
    top = run_player_top();
    h   = run_player_h();
    run_draw_player(top, h);
    s_draw_py = top;
    s_draw_ph = h;
    s_draw_valid = true;

    if (s_hud_dirty)
    {
        run_draw_hud();
        s_hud_dirty = false;
    }

    if (s_paused)
    {
        Game_DrawTextCenter(40, GTXT_PAUSE, RUN_COL_TEXT, RUN_COL_SKY);
    }

}

bool     RUN_IsOver(void)    { return s_over; }
bool     RUN_IsPaused(void)  { return s_paused; }
uint32_t RUN_GetScore(void)  { return s_score; }
uint32_t RUN_GetDistance(void) { return (uint32_t)(s_dist_m / 1000); }
uint16_t RUN_GetSpeed(void)  { return s_speed; }
uint8_t  RUN_GetPlayerH(void){ return (uint8_t)run_player_h(); }
bool     RUN_IsGrounded(void){ return s_grounded; }
int16_t  RUN_GetPlayerTop(void) { return run_player_top(); }

uint8_t RUN_GetObstacleCount(void)
{
    uint8_t i, n = 0;
    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        if (s_obs[i].active) { n++; }
    }
    return n;
}

bool RUN_GetObstacle(uint8_t i, int16_t *x, int16_t *y, int16_t *w, int16_t *h)
{
    if (i >= RUN_MAX_OBS || !s_obs[i].active) { return false; }
    if (x) { *x = s_obs[i].x; }
    if (y) { *y = s_obs[i].y; }
    if (w) { *w = s_obs[i].w; }
    if (h) { *h = s_obs[i].h; }
    return true;
}

void RUN_ClearObstacles(void)
{
    uint8_t i;
    for (i = 0; i < RUN_MAX_OBS; i++) { s_obs[i].active = false; }
}

void RUN_SpawnObstacle(int16_t x, int16_t y, int16_t w, int16_t h)
{
    uint8_t i;
    for (i = 0; i < RUN_MAX_OBS; i++)
    {
        if (!s_obs[i].active)
        {
            s_obs[i].x = x;
            s_obs[i].y = y;
            s_obs[i].w = w;
            s_obs[i].h = h;
            s_obs[i].active = true;
            s_obs[i].drawn  = false;
            s_obs[i].px     = x;
            s_obs[i].py     = y;
            return;
        }
    }
}

/******************* (C) CH592X 跑酷 *****************/
