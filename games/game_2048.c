/**************************************************************************
 * game_2048.c —— CH592X 2048 实现（2026-10-13 亮色系 / 分区刷新改版）
 *
 * ------------------------------------------------------------------
 * 界面（横屏 240x135，完全按需求书）：
 *      x = 0..134     左侧 135x135：4x4 棋盘
 *                     格子 33px + 缝 1px，4*33+3*1 = 135，正好铺满
 *      x = 135        1px 宽、135 高的分隔线
 *      x = 136..239   右侧 104x135：当前分数 / 最高分（白底亮色）
 *
 * ------------------------------------------------------------------
 * 刷新策略（禁止整屏刷新）：
 *      进游戏 / 重开     —— 整屏画一次（唯一的整屏时机之一）
 *      游戏中            —— 左侧只重画"变化了的格子"；
 *                           右侧只重画"当前分数"那一条
 *                           （最高分只在被追平时补一次）
 *      暂停 / 胜利提示   —— 只重画面板底部那一行
 *      游戏结束          —— 整屏一次（居中的本局分数 + 历史最高分
 *                           + "按任意键"提示）
 *
 * ------------------------------------------------------------------
 * 按键（按需求书）：
 *      K2 / K4 / K6 / K8  = 上 / 左 / 右 / 下（原来的"撤销"已删除）
 *      编码器单击          = 暂停（锁定进度、禁用操作）；再击继续
 *      游戏结束            = 按任意键重开
 *      （其余按键 / 双击 / 长按由页面框架处理，本文件不响应）
 *
 * ------------------------------------------------------------------
 * 规则是标准 2048：同值相撞合并且一次移动内每个块只合并一次；
 * 每次有效移动在空格里生成一个 2（90%）或 4（10%）。
 **************************************************************************/

#include "game_2048.h"
#include "game_state.h"
#include "game_score.h"

/*==========================================================================
 * 1. 配色（亮色系）
 *
 *    s_color 下标 = log2(块值)：0/1 用不到，2..2048+ 分别是各档颜色。
 *    亮色底配深色字，深色块配白字 —— 保证小屏上的对比度。
 *========================================================================*/
typedef struct
{
    uint16_t bg;
    uint16_t fg;
} G2048Color_t;

static const G2048Color_t s_color[12] =
{
    { 0xE71Cu, 0x0000u      },      /* 0    ：空格的浅灰底（不画字） */
    { 0xE71Cu, 0x0000u      },      /* 1    ：保留 */
    { 0xEF3Bu, 0x4208u      },      /* 2    浅米灰 + 深灰字 */
    { 0xEEF9u, 0x4208u      },      /* 4    浅灰米 + 深灰字 */
    { 0xF58Fu, 0x0000u      },      /* 8    浅橙   + 黑字   */
    { 0xF4ACu, 0xFFFFu      },      /* 16   橙     + 白字   */
    { 0xF3EBu, 0xFFFFu      },      /* 32   深橙   + 白字   */
    { 0xF2E7u, 0xFFFFu      },      /* 64   红橙   + 白字   */
    { 0xEE6Eu, 0x0000u      },      /* 128  黄     + 黑字   */
    { 0xEE6Cu, 0x0000u      },      /* 256  黄     + 黑字   */
    { 0xEE4Au, 0x0000u      },      /* 512  黄     + 黑字   */
    { 0xEE05u, 0x0000u      }       /* 1024+ 亮黄   + 黑字   */
};

#define C_PAGE_BG   0xFFFFu     /* 右侧信息区 / 结束页底色：白 */
#define C_FRAME     0xC638u     /* 棋盘缝（格与格之间的浅灰线） */
#define C_DIV       0xB5B6u     /* 135 高的 1px 分隔线：中灰 */
#define C_LBL       0x8410u     /* 标签字：中灰 */
#define C_SCORE     0x0000u     /* 当前分数：黑 */
#define C_BEST      0x3193u     /* 最高分：深蓝 */
#define C_PAUSE     0xCB20u     /* 暂停提示：橙棕 */
#define C_WIN       0x03C0u     /* 胜利提示：深绿 */
#define C_OVER      0xE249u     /* 游戏结束标题：红 */

/*==========================================================================
 * 2. 版面
 *========================================================================*/
#define G2048_PX(x)     ((int16_t)((int16_t)(x) * (G2048_CELL + G2048_GAP)))
#define G2048_PY(y)     ((int16_t)((int16_t)(y) * (G2048_CELL + G2048_GAP)))

#define G2048_DIV_X         135
#define G2048_PANEL_X       136
#define G2048_VAL_X         144                 /* 信息区正文左边距 */
#define G2048_VAL_W         88                  /* 104 - 8*2，值区宽度 */
#define G2048_Y_SCORE_LBL   8
#define G2048_Y_SCORE_VAL   28
#define G2048_Y_BEST_LBL    56
#define G2048_Y_BEST_VAL    76
#define G2048_Y_STATUS      110                 /* 暂停 / 胜利提示行 */

/*==========================================================================
 * 3. 状态
 *========================================================================*/
static uint16_t *s_board;               /* 4x4 棋盘（[y*4+x]） */
static uint16_t *s_drawn;               /* 已经画到屏上的那份（比对用） */
static uint32_t s_score;
static uint32_t s_best;
static bool     s_over;
static bool     s_win_shown;
static bool     s_need_full;            /* 需要整屏重画 */
static bool     s_score_dirty;          /* 需要重画当前分数 */
static bool     s_best_dirty;           /* 需要重画最高分 */
static bool     s_status_dirty;         /* 需要重画暂停/胜利提示行 */
static bool     s_over_drawn;
static uint16_t s_spawn_value;          /* 0 = 随机 2/4；测试钩子可指定 */

static bool     s_paused;               /* 编码器单击置位 */

/*==========================================================================
 * 4. 小工具
 *========================================================================*/
static uint8_t g2048_log2(uint16_t v)
{
    uint8_t n = 0;
    while (v > 1u && n < 15u) { v = (uint16_t)(v >> 1); n++; }
    return n;
}

static uint8_t g2048_digits(uint32_t v)
{
    uint8_t n = 1;
    while (v >= 10u) { v /= 10u; n++; }
    return n;
}

/* 数字在给定宽度里放不下就降一档字号（2 -> 1） */
static uint8_t g2048_fit_scale(uint32_t v, int16_t max_w)
{
    if (Game_NumberWidth(v, 2u) <= max_w)
    {
        return 2u;
    }
    return 1u;
}

/*********************************************************************
 * @fn      g2048_draw_digits
 * @brief   按"紧凑字距"画一个数字
 *
 * 为什么不用 Game_DrawNumber：默认字距是 6*scale（scale2 = 12px），
 * 3 位数（128/256/512）就要 36px，塞不进 33px 的格子。
 * 这里把字距收到 11px（scale2）：3 位数墨迹宽 = 2*11 + 10 = 32px √
 * 4 位数改用 scale1（墨迹宽 3*6+5 = 23px）—— 再大也放得下。
 *********************************************************************/
static void g2048_draw_digits(int16_t x, int16_t y, uint32_t v,
                              uint8_t scale, uint16_t fg, uint16_t bg)
{
    uint8_t  d[8];
    uint8_t  n = 0u;
    uint32_t t = v;
    uint8_t  i;
    int16_t  adv = (scale >= 2u) ? 11 : 6;

    if (t == 0u)
    {
        d[n++] = 0u;
    }
    while ((t > 0u) && (n < (uint8_t)sizeof(d)))
    {
        d[n++] = (uint8_t)(t % 10u);
        t /= 10u;
    }

    for (i = 0u; i < n; i++)
    {
        Game_DrawNumber((int16_t)(x + (int16_t)i * adv), y,
                        (uint32_t)d[n - 1u - i], scale, fg, bg);
    }
}

/* 紧凑字距下，数字的"墨迹宽度"（最后一个字符后面那 1px 间隔不算） */
static int16_t g2048_digits_width(uint32_t v, uint8_t scale)
{
    uint8_t n = g2048_digits(v);
    int16_t adv = (scale >= 2u) ? 11 : 6;

    return (int16_t)((int16_t)(n - 1u) * adv + (int16_t)5 * (int16_t)scale);
}

/*==========================================================================
 * 5. 画格子
 *========================================================================*/
static void g2048_draw_cell(uint8_t x, uint8_t y, uint16_t v)
{
    int16_t  cx = G2048_PX(x);
    int16_t  cy = G2048_PY(y);
    uint8_t  lg = g2048_log2(v);
    uint8_t  scale;
    int16_t  tw;
    uint16_t bg;
    uint16_t fg;

    if (lg >= 12u) { lg = 11u; }
    bg = s_color[lg].bg;
    fg = s_color[lg].fg;

    TFT_FillRect(cx, cy, G2048_CELL, G2048_CELL, bg);
    if (v == 0u) { return; }

    /* 1~3 位用 2 倍字号（紧凑字距），4 位以上降到 1 倍，保证不压格 */
    scale = (g2048_digits(v) <= 3u) ? 2u : 1u;
    tw    = g2048_digits_width(v, scale);
    g2048_draw_digits((int16_t)(cx + (G2048_CELL - tw) / 2),
                      (int16_t)(cy + (G2048_CELL - 8 * (int16_t)scale) / 2),
                      v, scale, fg, bg);
}

/* 棋盘底板 + 全部格子（只在整屏时机调用） */
static void g2048_draw_board(void)
{
    uint8_t x, y;

    /* 底板：露在格子缝里的浅灰 —— 一格一格之间就是分隔线 */
    TFT_FillRect(0, 0, G2048_BOARD_W, G2048_BOARD_W, C_FRAME);

    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++)
        {
            g2048_draw_cell(x, y, s_board[(uint16_t)y * G2048_SIZE + x]);
            s_drawn[(uint16_t)y * G2048_SIZE + x] = s_board[(uint16_t)y * G2048_SIZE + x];
        }
    }
}

/* 只重画变化了的格子（游戏中的每一次移动都走这里） */
static void g2048_draw_cells_dirty(void)
{
    uint8_t x, y;

    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++)
        {
            uint16_t idx = (uint16_t)((uint16_t)y * G2048_SIZE + x);

            if (s_drawn[idx] != s_board[idx])
            {
                g2048_draw_cell(x, y, s_board[idx]);
                s_drawn[idx] = s_board[idx];
            }
        }
    }
}

/*==========================================================================
 * 6. 右侧信息区
 *========================================================================*/
/* 分隔线 + 白底 + 两个标签：进游戏时画一次就够 */
static void g2048_draw_panel_static(void)
{
    TFT_FillRect(G2048_DIV_X, 0, 1, (int16_t)Game_Height(), C_DIV);
    TFT_FillRect(G2048_PANEL_X, 0, (int16_t)(Game_Width() - G2048_PANEL_X),
                 (int16_t)Game_Height(), C_PAGE_BG);

    Game_DrawText(G2048_VAL_X, G2048_Y_SCORE_LBL, GTXT_SCORE, C_LBL, C_PAGE_BG);
    Game_DrawText(G2048_VAL_X, G2048_Y_BEST_LBL,  GTXT_BEST,  C_LBL, C_PAGE_BG);
}

/* 只刷"当前分数"这一条（擦一整条再画，数字变短时不会留残影） */
static void g2048_draw_score(void)
{
    uint8_t sc = g2048_fit_scale(s_score, G2048_VAL_W);

    TFT_FillRect(G2048_VAL_X, G2048_Y_SCORE_VAL, G2048_VAL_W, 16, C_PAGE_BG);
    Game_DrawNumber(G2048_VAL_X, G2048_Y_SCORE_VAL, s_score, sc, C_SCORE, C_PAGE_BG);
}

/* 只刷"最高分"这一条 */
static void g2048_draw_best(void)
{
    uint8_t sc = g2048_fit_scale(s_best, G2048_VAL_W);

    TFT_FillRect(G2048_VAL_X, G2048_Y_BEST_VAL, G2048_VAL_W, 16, C_PAGE_BG);
    Game_DrawNumber(G2048_VAL_X, G2048_Y_BEST_VAL, s_best, sc, C_BEST, C_PAGE_BG);
}

/* 面板底部：暂停 / 胜利（暂停优先显示） */
static void g2048_draw_status(void)
{
    TFT_FillRect(G2048_VAL_X, G2048_Y_STATUS, G2048_VAL_W, 16, C_PAGE_BG);

    if (s_paused)
    {
        Game_DrawText(G2048_VAL_X, G2048_Y_STATUS, GTXT_PAUSE, C_PAUSE, C_PAGE_BG);
    }
    else if (s_win_shown)
    {
        Game_DrawText(G2048_VAL_X, G2048_Y_STATUS, GTXT_WIN, C_WIN, C_PAGE_BG);
    }
}

/*==========================================================================
 * 7. 整屏：进游戏 / 游戏结束
 *========================================================================*/
static void g2048_draw_all(void)
{
    g2048_draw_board();
    g2048_draw_panel_static();
    g2048_draw_score();
    g2048_draw_best();
    g2048_draw_status();

    s_need_full     = false;
    s_score_dirty   = false;
    s_best_dirty    = false;
    s_status_dirty  = false;
    s_over_drawn    = false;
}

/* 结束页：整屏（本文件里第二个、也是最后一个整屏时机） */
static void g2048_draw_over(void)
{
    int16_t lw, nw, x;

    Game_Clear(C_PAGE_BG);

    /* 标题 */
    lw = Game_TextWidth(GTXT_GAME_OVER);
    Game_DrawText((int16_t)((Game_Width() - lw) / 2), 14,
                  GTXT_GAME_OVER, C_OVER, C_PAGE_BG);

    /* 本局分数：标签 + 数字 成组左右居中 */
    lw = Game_TextWidth(GTXT_SCORE);
    nw = Game_NumberWidth(s_score, 2u);
    x  = (int16_t)((Game_Width() - (lw + 8 + nw)) / 2);
    Game_DrawText(x, 46, GTXT_SCORE, C_LBL, C_PAGE_BG);
    Game_DrawNumber((int16_t)(x + lw + 8), 46, s_score, 2u, C_SCORE, C_PAGE_BG);

    /* 历史最高分：同样成组居中 */
    lw = Game_TextWidth(GTXT_BEST);
    nw = Game_NumberWidth(s_best, 2u);
    x  = (int16_t)((Game_Width() - (lw + 8 + nw)) / 2);
    Game_DrawText(x, 72, GTXT_BEST, C_LBL, C_PAGE_BG);
    Game_DrawNumber((int16_t)(x + lw + 8), 72, s_best, 2u, C_BEST, C_PAGE_BG);

    /* 底部提示 */
    lw = Game_TextWidth(GTXT_ANY_KEY);
    Game_DrawText((int16_t)((Game_Width() - lw) / 2), 104,
                  GTXT_ANY_KEY, C_LBL, C_PAGE_BG);

    s_over_drawn = true;
}

/*==========================================================================
 * 8. 棋盘存取 / 规则（与旧版一致）
 *========================================================================*/
static uint16_t bget(const uint16_t *b, uint8_t x, uint8_t y)
{
    return b[(uint16_t)((uint16_t)y * G2048_SIZE + x)];
}

static void bset(uint16_t *b, uint8_t x, uint8_t y, uint16_t v)
{
    b[(uint16_t)((uint16_t)y * G2048_SIZE + x)] = v;
}

static bool g2048_spawn(void)
{
    uint8_t empty[G2048_SIZE * G2048_SIZE][2];
    uint8_t n = 0;
    uint8_t x, y;

    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++)
        {
            if (s_board[(uint16_t)y * G2048_SIZE + x] == 0u)
            {
                empty[n][0] = x;
                empty[n][1] = y;
                n++;
            }
        }
    }

    if (n == 0u)
    {
        return false;
    }

    {
        uint8_t i = (uint8_t)Game_RandRange((uint32_t)n);

        if (s_spawn_value != 0u)
        {
            bset(s_board, empty[i][0], empty[i][1], s_spawn_value);
        }
        else
        {
            bset(s_board, empty[i][0], empty[i][1],
                 (Game_RandRange(10u) == 0u) ? 4u : 2u);
        }
    }
    return true;
}

static bool g2048_can_move(void)
{
    uint8_t x, y;

    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++)
        {
            uint16_t v = s_board[(uint16_t)y * G2048_SIZE + x];

            if (v == 0u) { return true; }
            if (x + 1u < G2048_SIZE && bget(s_board, (uint8_t)(x + 1u), y) == v) { return true; }
            if (y + 1u < G2048_SIZE && bget(s_board, x, (uint8_t)(y + 1u)) == v) { return true; }
        }
    }
    return false;
}

/* 压一行：去掉 0 -> 相邻同值合并（一次移动内每块只合并一次）-> 补 0 */
static bool g2048_line(uint16_t *line, uint32_t *gain)
{
    uint16_t tmp[G2048_SIZE];
    uint8_t  i, n = 0;
    bool     changed = false;

    for (i = 0; i < G2048_SIZE; i++)
    {
        if (line[i] != 0u) { tmp[n++] = line[i]; }
    }

    for (i = 0; i + 1u < n; i++)
    {
        if (tmp[i] == tmp[i + 1u])
        {
            tmp[i] = (uint16_t)(tmp[i] * 2u);
            *gain += (uint32_t)tmp[i];
            if (tmp[i] == G2048_WIN) { s_win_shown = true; }
            {
                uint8_t k;
                for (k = (uint8_t)(i + 1u); k + 1u < n; k++) { tmp[k] = tmp[k + 1u]; }
                tmp[n - 1u] = 0u;
                n--;
            }
        }
    }

    while (n < G2048_SIZE) { tmp[n++] = 0u; }

    for (i = 0; i < G2048_SIZE; i++)
    {
        if (line[i] != tmp[i]) { changed = true; }
        line[i] = tmp[i];
    }
    return changed;
}

bool G2048_Move(uint8_t dir)
{
    uint16_t line[G2048_SIZE];
    bool     changed = false;
    uint32_t gain = 0u;
    uint8_t  i, j;

    if (s_over || s_paused)
    {
        return false;                   /* 结束/暂停：锁定进度，不接受操作 */
    }

    for (i = 0; i < G2048_SIZE; i++)
    {
        for (j = 0; j < G2048_SIZE; j++)
        {
            if (dir == G2048_LEFT)       { line[j] = s_board[(uint16_t)i * G2048_SIZE + (uint16_t)j]; }
            else if (dir == G2048_RIGHT) { line[j] = bget(s_board, (uint8_t)(G2048_SIZE - 1u - j), (uint8_t)i); }
            else if (dir == G2048_UP)    { line[j] = s_board[(uint16_t)j * G2048_SIZE + (uint16_t)i]; }
            else                         { line[j] = bget(s_board, (uint8_t)i, (uint8_t)(G2048_SIZE - 1u - j)); }
        }

        if (g2048_line(line, &gain))
        {
            changed = true;
        }

        for (j = 0; j < G2048_SIZE; j++)
        {
            if (dir == G2048_LEFT)       { s_board[(uint16_t)i * G2048_SIZE + (uint16_t)j] = line[j]; }
            else if (dir == G2048_RIGHT) { bset(s_board, (uint8_t)(G2048_SIZE - 1u - j), (uint8_t)i, line[j]); }
            else if (dir == G2048_UP)    { s_board[(uint16_t)j * G2048_SIZE + (uint16_t)i] = line[j]; }
            else                         { bset(s_board, (uint8_t)i, (uint8_t)(G2048_SIZE - 1u - j), line[j]); }
        }
    }

    if (!changed)
    {
        return false;                   /* 没动：不生成新块，也不算一步 */
    }

    s_score += gain;
    (void)g2048_spawn();

    s_score_dirty = true;
    if (s_score > s_best)
    {
        s_best       = s_score;         /* 追平/超过最高分：显示先跟上，落盘在结束时做 */
        s_best_dirty = true;
    }
    if (s_win_shown)
    {
        s_status_dirty = true;
    }

    if (!g2048_can_move())
    {
        s_over = true;
        /* 破纪录立即落盘（原来是没人调用 GS_SetBest，最高分永远不更新） */
        (void)GS_SetBest(GAME_ID_2048, s_score);
        s_best       = GS_GetBest(GAME_ID_2048);
        s_best_dirty = true;
    }

    return true;
}

/*==========================================================================
 * 9. 对外接口
 *========================================================================*/
/* 【修复：2048 按键卡住】把"重置一局"和"整屏重画"拆开。
 *
 * 原来 G2048_Enter() 里直接 g2048_draw_all()（一次整屏 240x135 的 SPI 传输）。
 * 而"结束页按任意键重开"也走 G2048_Enter() —— 那个调用点在 UI_Task() 的
 * 事件排空循环里，于是**每收到一个按键事件就整屏重画一次**。
 * 长按方向键时 key_track() 每 90ms 会补一个 GAME_EV_REPEAT，重画的速度
 * 跟不上刷屏的速度，主循环被 SPI 占死 —— 表现就是"按键突然不能控制了，
 * 松开按一会儿才恢复"。
 *
 * 现在重置只置 s_need_full，整屏重画交给下一帧的 G2048_Draw() 做（每帧最多一次）。
 * 事件处理里不再有任何整屏操作。 */
static void g2048_reset(void)
{
    uint8_t x, y;

    s_board = Game_PoolBoard2048();
    s_drawn = Game_PoolDrawn2048();

    Game_SetOrientation(GAME_LANDSCAPE);
    GS_GetDifficulty();

    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++)
        {
            s_board[(uint16_t)y * G2048_SIZE + x] = 0u;
            s_drawn[(uint16_t)y * G2048_SIZE + x] = 0u;
        }
    }

    s_score        = 0u;
    s_best         = GS_GetBest(GAME_ID_2048);
    s_over         = false;
    s_win_shown    = false;
    s_paused       = false;
    s_need_full    = true;
    s_score_dirty  = true;
    s_best_dirty   = true;
    s_status_dirty = true;
    s_over_drawn   = false;

    (void)g2048_spawn();
    (void)g2048_spawn();
}

void G2048_Enter(void)
{
    if (!Game_PoolAcquire())
    {
        return;
    }

    g2048_reset();
    g2048_draw_all();                   /* 进游戏：整屏一次 */
}

bool G2048_HandleKey(const GameKeyEvent_t *ev)
{
    if (ev == 0)
    {
        return false;
    }

    /* 双击编码器：退出游戏（回选游戏页），由页面框架处理 */
    if ((ev->key == GKEY_ENC_DBL) && (ev->type == GAME_EV_PRESS))
    {
        return true;
    }

    if (ev->type == GAME_EV_RELEASE)
    {
        return false;                   /* 2048 全是按下沿触发，松开不做事 */
    }

    /* ---- 结束：按任意键重开 ----
     * 【修复：2048 按键卡住】只认"按下沿"，不认长按连发（GAME_EV_REPEAT）：
     *   1) 长按方向键时每 90ms 会来一个 REPEAT，原来每个 REPEAT 都会重开一局，
     *      而重开里带着整屏重画 -> 主循环被 SPI 占死 -> 按键失灵；
     *   2) 现在重开只重置状态，整屏重画留给 G2048_Draw() 每帧一次。 */
    if (s_over)
    {
        if (ev->type == (uint8_t)GAME_EV_PRESS)
        {
            g2048_reset();
        }
        return false;
    }

    /* ---- 编码器单击：暂停 / 继续 ---- */
    if (ev->key == GKEY_ENC_CLICK)
    {
        s_paused       = !s_paused;
        s_status_dirty = true;
        return false;
    }

    if (s_paused)
    {
        return false;                   /* 暂停中：禁用一切操作 */
    }

    /* 【修复：2048 按键卡住】方向键只认"按下沿"。
     * 原来不判 ev->type，GAME_EV_REPEAT（按住 0.45s 后的每 90ms 连发）
     * 也会当成一次移动：按住不放会一路自动走，几步就把棋盘走死，
     * 走死之后每个 REPEAT 又都去重开一局 + 整屏重画，主循环直接卡住。
     * 需求里 key2/4/6/8 只是"对应上下左右"，没有长按连发，所以这里过滤掉。 */
    if (ev->type != (uint8_t)GAME_EV_PRESS)
    {
        return false;
    }

    switch ((int)ev->key)
    {
        case GKEY_K2: (void)G2048_Move(G2048_UP);    break;
        case GKEY_K8: (void)G2048_Move(G2048_DOWN);  break;
        case GKEY_K4: (void)G2048_Move(G2048_LEFT);  break;
        case GKEY_K6: (void)G2048_Move(G2048_RIGHT); break;

        default:
            break;                      /* 其它键无操作（撤销已删除） */
    }

    return false;
}

void G2048_Update(uint16_t dt_ms)
{
    (void)dt_ms;                        /* 2048 是事件驱动，不需要时间推进 */
}

void G2048_Draw(void)
{
    if (s_over)
    {
        if (!s_over_drawn)
        {
            g2048_draw_over();          /* 结束：整屏一次 */
        }
        return;
    }

    if (s_need_full)
    {
        g2048_draw_all();
        return;
    }

    g2048_draw_cells_dirty();           /* 左侧：只刷变化的格子 */

    if (s_score_dirty)                  /* 右侧：只刷当前分数 */
    {
        g2048_draw_score();
        s_score_dirty = false;
    }
    if (s_best_dirty)
    {
        g2048_draw_best();
        s_best_dirty = false;
    }
    if (s_status_dirty)
    {
        g2048_draw_status();
        s_status_dirty = false;
    }
}

bool     G2048_IsOver(void)   { return s_over; }
bool     G2048_IsPaused(void) { return s_paused; }
bool     G2048_IsWin(void)    { return s_win_shown; }
uint32_t G2048_GetScore(void) { return s_score; }
uint32_t G2048_GetBest(void)  { return s_best; }

uint16_t G2048_GetCell(uint8_t x, uint8_t y)
{
    if (x >= G2048_SIZE || y >= G2048_SIZE) { return 0u; }
    return s_board[(uint16_t)y * G2048_SIZE + x];
}

uint8_t G2048_GetTiles(void)
{
    uint8_t x, y, n = 0;
    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++)
        {
            if (s_board[(uint16_t)y * G2048_SIZE + x] != 0u) { n++; }
        }
    }
    return n;
}

#if G2048_TEST_HOOKS
void G2048_SetCell(uint8_t x, uint8_t y, uint16_t v)
{
    if (x < G2048_SIZE && y < G2048_SIZE) { s_board[(uint16_t)y * G2048_SIZE + x] = v; }
}

void G2048_ClearBoard(void)
{
    uint8_t x, y;
    for (y = 0; y < G2048_SIZE; y++)
    {
        for (x = 0; x < G2048_SIZE; x++) { s_board[(uint16_t)y * G2048_SIZE + x] = 0u; }
    }
}

void G2048_SetScore(uint32_t s) { s_score = s; }
void G2048_SetSpawnValue(uint16_t v) { s_spawn_value = v; }
#endif

/******************* (C) CH592X 2048 *****************/
