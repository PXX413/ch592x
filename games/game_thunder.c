/**************************************************************************
 * game_thunder.c —— CH592X 雷霆战机 实现
 *
 * 定点数：位置和速度全部用 1/16 像素（Q16）为单位的整数，
 *         板上没有 FPU，所以一行浮点都不用。
 *         换算成屏幕像素 = Q16 坐标 / THD_Q。
 *
 * 内存：所有对象都是静态数组，一个 malloc 都没有。
 **************************************************************************/

#include "game_thunder.h"
#include "game_state.h"
#include "game_score.h"

/*==========================================================================
 * 1. 参数
 *========================================================================*/
#define THD_Q               16      /* 1 像素 = 16 个定点单位 */
#define THD_TICK_MS         20      /* 速度按"每 20ms 走多少"定义 */

#define THD_BOOT_MS         500u    /* 开机 Logo 停留时间 */
#define THD_FIRST_SPAWN_MS  900u    /* 开局先给玩家一点反应时间 */

/* ---- 玩家 ---- */
#define THD_PLAYER_X        14      /* 战机固定在屏幕左侧这个 x */
#define THD_PLAYER_W        15
#define THD_PLAYER_H        12
#define THD_PLAYER_SPD      48      /* 3 像素 / tick */
#define THD_FIRE_MS         120u    /* 发射间隔（文档 6.2） */
#define THD_INVIN_MS        2000u   /* 被撞后的无敌时间（文档 6.2） */
#define THD_BOMB_INVIN_MS   800u    /* 放炸弹时的短暂无敌 */
#define THD_BLINK_MS        100u    /* 无敌期间闪烁的周期 */

/* ---- 各对象尺寸（固定不变，擦除时直接拿来用） ---- */
#define THD_PB_W            3
#define THD_PB_H            7
#define THD_EB_W            3
#define THD_EB_H            3
#define THD_EN_W            14
#define THD_EN_H            11
#define THD_BOOM_W          12      /* 爆炸的最大包围盒 */
#define THD_BOOM_H          12
#define THD_BOOM_MS         60u     /* 爆炸存活时间（3 帧 x 20ms） */
#define THD_BOSS_W          44
#define THD_BOSS_H          30

/* 碰撞盒每边内缩 1 像素 = 合计比图形小 2 像素（文档 7.3 / 十四章 6） */
#define THD_HIT_INSET       1

/* ---- 速度（Q16 单位 / tick） ---- */
#define THD_PB_SPD          96      /* 玩家子弹  6 像素/tick */
#define THD_EB_SPD          48      /* 敌弹      3 像素/tick */
#define THD_E_SMALL_SPD     16      /* 小飞机    1 像素/tick */
#define THD_E_DIAG_SPD      16      /* 斜飞      1 像素/tick */
#define THD_E_DIAG_VY       24      /* 斜飞      1.5 像素/tick */
#define THD_E_SHOOT_SPD     12      /* 射击机    0.75 像素/tick */
#define THD_E_FAST_SPD      32      /* 高速机    2 像素/tick */
#define THD_BOSS_SPD        32      /* Boss 进场速度 */
#define THD_BOSS_VY         16      /* Boss 上下摆动速度 */
#define THD_BOSS_HOME_X     184     /* Boss 停在这个 x（像素） */
/* Boss 上下活动的范围：下边留 12px 给三向炮的扇形，
 * 不然贴底时最下面那颗炮弹一出生就掉出屏幕了 */
#define THD_BOSS_Y_MIN      THD_TOP
#define THD_BOSS_Y_MAX      (THD_BOTTOM - THD_BOSS_H - 12)

/* ---- 敌机行为 ---- */
#define THD_SHOOT_MS        1400u   /* 射击机开火间隔 */
#define THD_BOSS_FIRE_MS    900u    /* Boss 开火间隔 */
#define THD_BOSS_HP         50u     /* Boss 血量（文档 6.3）—— 只作首架的上限 */
#define THD_BOSS_SCORE      5000u
/* 【2026-10-15 需求 3】打完 Boss 不再通关结束：接着玩，直到生命值归零。
 * 击落一架后隔 THD_BOSS_RESPAWN_MS 毫秒再放下一架，血量上限加
 * THD_BOSS_HP_STEP（封顶 THD_BOSS_HP_MAX），并奖励一颗炸弹。 */
#define THD_BOSS_HP_MAX     200u
#define THD_BOSS_HP_STEP    20u
#define THD_BOSS_RESPAWN_MS 8000u

/* ---- 难度曲线（文档第十章） ---- */
#define THD_WAVE_MS         30000u  /* 每 30 秒一波 */
#define THD_WAVE_MAX        5
/* 第 3 波（60s）Boss 登场。
 * 注意：文档 6.4 写"第 3 波出现 Boss"，第十章的表格却写"120s+ 出 Boss"，
 *       两处冲突。这里按 6.4 + 需求走"第 3 波"，四种敌机在第 3 波前都会露面。
 *       想改回 120s，只要把这个数改成 5 即可。 */
#define THD_BOSS_WAVE       3

/* ---- 颜色 ---- */
#define THD_COL_BG          0xB6BEu     /* 亮色系：浅天蓝底 */
#define THD_COL_STAR        0xFFFFu     /* 星星改白云点：亮底上仍可见 */
#define THD_COL_HUD         0xE71Cu     /* HUD 条：浅灰 */
#define THD_COL_HUD_ALERT   0xC186u     /* HUD 告警闪：中红 */
#define THD_COL_LABEL       0x0B6Au     /* 标签：深青 */
#define THD_COL_NUM         0x03C0u     /* 数值：深绿 */
#define THD_COL_BEST        0x3193u     /* 最高分：深蓝（和 2048 结束页一致） */
#define THD_COL_PLAYER      0x1AF4u     /* 我机：深蓝（原青色在亮底看不清） */
#define THD_COL_COCKPIT     ST77XX_WHITE
#define THD_COL_PBUL        0xF800u     /* 我方弹：红（原白色不可见） */
/* 敌方弹：黑。原来用深紫 0x780F，和 Boss 机身同色，糊在一起分不清；
 * 亮天蓝底上黑点最醒目。（另外 thd_draw_bullet() 原来漏了 thd_px()，
 * 子弹其实一颗都没画出来，看到的只是擦除用的背景色块 —— 已一并修掉。） */
#define THD_COL_EBUL        0x0000u
#define THD_COL_HP          0x03C0u     /* 血条：深绿 */
#define THD_COL_HP_MID      0xBBA2u     /* 血条中：琥珀 */
#define THD_COL_HP_LOW      0xC000u     /* 血条低：深红 */
#define THD_COL_HP_OFF      0xCE79u     /* 血条空槽：浅灰 */
#define THD_COL_BOMB        0xBBA2u     /* 炸弹数：琥珀（原黄） */
#define THD_COL_BOMB_OFF    0x4208u
#define THD_COL_BOSS        0x780Fu
#define THD_COL_EN_SMALL    0xC000u     /* 小敌机：深红 */
#define THD_COL_EN_DIAG     ST77XX_MAGENTA
#define THD_COL_EN_SHOOT    ST77XX_ORANGE
#define THD_COL_EN_FAST     0xBBA2u     /* 快敌机：琥珀（原黄） */

/* ---- HUD 排版 ---- */
#define THD_HP_X            36
#define THD_HP_W            10
#define THD_HP_STEP         12
#define THD_BOMB_X          76
#define THD_BOMB_STEP       7
#define THD_LEVEL_X         104
#define THD_SCORE_X         164
#define THD_SCORE_R         237

/*==========================================================================
 * 2. 对象
 *========================================================================*/
/* ThdObj_t 现在在 game_thunder.h 里（共享池需要看到它） */

/* 编译期预算守卫：对象结构一旦被改大，这里会直接编译报错，
 * 提醒同步更新 README 里的内存预算（48 个对象 x 20B = 960B）。 */
typedef char thd_obj_size_check[(sizeof(ThdObj_t) == 20) ? 1 : -1];

/* 改成共享池：原来自己开 THD_MAX_PBULLET 个，现在用 game_state
 * 里的一段（只有一个游戏会跑，所以可以共用） */
static ThdObj_t *s_pbullet;
/* 改成共享池：原来自己开 THD_MAX_ENEMY 个，现在用 game_state
 * 里的一段（只有一个游戏会跑，所以可以共用） */
static ThdObj_t *s_enemy;
/* 改成共享池：原来自己开 THD_MAX_EBULLET 个，现在用 game_state
 * 里的一段（只有一个游戏会跑，所以可以共用） */
static ThdObj_t *s_ebullet;
/* 改成共享池：原来自己开 THD_MAX_BOOM 个，现在用 game_state
 * 里的一段（只有一个游戏会跑，所以可以共用） */
static ThdObj_t *s_boom;
static ThdObj_t s_boss;
static ThdObj_t s_player;

/*==========================================================================
 * 3. 星图（静态背景，擦除时会被自动补回来）
 *========================================================================*/
typedef struct
{
    uint8_t x;
    uint8_t y;
} ThdStar_t;

static const ThdStar_t s_star[] =
{
    {  30,  26 }, {  72,  52 }, { 118,  33 }, { 155,  90 }, {  47, 104 },
    { 200,  60 }, {  88,  78 }, { 132, 118 }, {  20,  70 }, { 176,  25 },
    { 108,  66 }, {  60,  36 }, { 214, 110 }, { 142,  80 }
};
#define THD_STAR_N  ((uint8_t)(sizeof(s_star) / sizeof(s_star[0])))

/*==========================================================================
 * 4. 全局状态
 *========================================================================*/
static uint8_t  s_state;
static bool     s_need_full;
static bool     s_hud_dirty;

static uint32_t s_score;
static uint8_t  s_lives;
static uint8_t  s_bombs;

static uint32_t s_t;            /* PLAY 已经进行了多少毫秒 */
static uint32_t s_boot_ms;      /* BOOT 页停留了多久 */
static uint32_t s_fire_ms;      /* 距离下一次开火还差多少毫秒 */
static uint32_t s_spawn_ms;     /* 距离下一架敌机还差多少毫秒 */
static uint32_t s_invin_ms;     /* 无敌剩余毫秒 */
static uint32_t s_blink_ms;
static bool     s_blink_on;
static uint8_t  s_shown_level;  /* 已经显示在 HUD 上的等级 */
static uint32_t s_hud_flash_ms; /* Boss 登场时 HUD 闪几下 */

static bool     s_boss_spawned;

/* 【需求 3】Boss 重放：血量上限逐架加厚，击落后等一段时间再放下一架 */
static uint8_t  s_boss_hp_max;
static uint32_t s_boss_respawn_ms;

/* 按住的键：事件里维护一份，同时再查一次实时状态，双保险 */
static bool     s_ev_up;
static bool     s_ev_down;
static bool     s_ev_fire;

/*==========================================================================
 * 5. 小工具
 *========================================================================*/
static int16_t thd_px(int16_t q)
{
    return (int16_t)(q / THD_Q);
}

/* 把矩形裁剪到战场区里再画 —— 保证任何一次绘图都不会越界 */
static void thd_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (w <= 0 || h <= 0)                     { return; }
    if (x < 0)                                { w += x; x = 0; }
    if (y < (int32_t)THD_TOP)                 { h -= ((int32_t)THD_TOP - y); y = (int32_t)THD_TOP; }
    if (x >= (int32_t)THD_W)                  { return; }
    if (y >= (int32_t)THD_BOTTOM)             { return; }
    if ((x + w) > (int32_t)THD_W)             { w = (int32_t)THD_W - x; }
    if ((y + h) > (int32_t)THD_BOTTOM)        { h = (int32_t)THD_BOTTOM - y; }
    if (w <= 0 || h <= 0)                     { return; }

    TFT_FillRect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, c);
}

/* 擦一块地方，并把落在里面的星点补回来（否则星图会被擦出一条条空道） */
static void thd_erase_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    uint8_t i;

    thd_fill(x, y, w, h, THD_COL_BG);

    for (i = 0; i < THD_STAR_N; i++)
    {
        if (((int32_t)s_star[i].x >= x) && ((int32_t)s_star[i].x < (x + w)) &&
            ((int32_t)s_star[i].y >= y) && ((int32_t)s_star[i].y < (y + h)))
        {
            thd_fill(s_star[i].x, s_star[i].y, 1, 1, THD_COL_STAR);
        }
    }
}

/* 一帧走多远（Q16 单位）：v 按"每 20ms 多少"定义，pct 是难度百分比 */
static int16_t thd_step(int16_t v, uint16_t dt_ms, uint16_t pct)
{
    int32_t d = ((int32_t)v * (int32_t)pct * (int32_t)dt_ms) / (100 * THD_TICK_MS);
    return (int16_t)d;
}

/* AABB 相交（全部是 Q16 单位） */
static bool thd_box_hit(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                        int32_t bx, int32_t by, int32_t bw, int32_t bh)
{
    return ((ax < (bx + bw)) && (bx < (ax + aw)) &&
            (ay < (by + bh)) && (by < (ay + ah))) ? true : false;
}

static void thd_obj_erase(ThdObj_t *o)
{
    if (o->drawn != 0u)
    {
        thd_erase_rect(o->px, o->py, o->w, o->h);
        o->drawn = 0u;
    }
}

static void thd_obj_mark(ThdObj_t *o)
{
    o->px = thd_px(o->x);
    o->py = thd_px(o->y);
    o->drawn = 1u;
}

static ThdObj_t *thd_alloc(ThdObj_t *pool, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++)
    {
        if (pool[i].alive == 0u)
        {
            return &pool[i];
        }
    }
    return 0;
}

static void thd_pool_clear(ThdObj_t *pool, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; i++)
    {
        pool[i].alive = 0u;
        pool[i].drawn = 0u;
        pool[i].hp    = 0u;
        pool[i].kind  = 0u;
        pool[i].timer = 0u;
        pool[i].x = 0;
        pool[i].y = 0;
        pool[i].vx = 0;
        pool[i].vy = 0;
        pool[i].px = 0;
        pool[i].py = 0;
        pool[i].w = 0u;
        pool[i].h = 0u;
    }
}

static uint16_t thd_enemy_color(uint8_t kind)
{
    switch ((int)kind)
    {
        case THD_EK_DIAG:  return THD_COL_EN_DIAG;
        case THD_EK_SHOOT: return THD_COL_EN_SHOOT;
        case THD_EK_FAST:  return THD_COL_EN_FAST;
        default:           return THD_COL_EN_SMALL;
    }
}

static uint32_t thd_score_of(uint8_t kind)
{
    switch ((int)kind)
    {
        case THD_EK_DIAG:  return 150u;
        case THD_EK_SHOOT: return 300u;
        case THD_EK_FAST:  return 200u;
        case THD_EK_BOSS:  return THD_BOSS_SCORE;
        default:           return 100u;
    }
}

/*==========================================================================
 * 6. 难度曲线
 *========================================================================*/
static const uint8_t  s_wave_speed[THD_WAVE_MAX] = { 100u, 120u, 150u, 180u, 200u };
static const uint16_t s_wave_spawn[THD_WAVE_MAX] = { 1000u, 800u, 600u, 500u, 400u };

static uint8_t thd_wave_idx(void)
{
    uint32_t w = s_t / THD_WAVE_MS;

    if (w >= (uint32_t)THD_WAVE_MAX)
    {
        w = (uint32_t)(THD_WAVE_MAX - 1);
    }
    return (uint8_t)w;
}

static uint8_t thd_level(void)
{
    return (uint8_t)(thd_wave_idx() + 1u);
}

static uint16_t thd_speed_pct(void)
{
    uint16_t p = (uint16_t)s_wave_speed[thd_wave_idx()];
    uint8_t  d = GS_GetDifficulty();

    if (d == (uint8_t)GAME_DIFF_EASY)
    {
        p = (uint16_t)((p * 8u) / 10u);
    }
    else if (d == (uint8_t)GAME_DIFF_HARD)
    {
        p = (uint16_t)((p * 5u) / 4u);
    }
    return p;
}

/*==========================================================================
 * 7. 生成
 *========================================================================*/
static void thd_spawn_boom(int16_t xq, int16_t yq)
{
    ThdObj_t *o = thd_alloc(s_boom, THD_MAX_BOOM);

    if (o == 0)
    {
        return;
    }

    o->x = (int16_t)(xq - ((THD_BOOM_W / 2) * THD_Q));
    o->y = (int16_t)(yq - ((THD_BOOM_H / 2) * THD_Q));
    o->vx = 0;
    o->vy = 0;
    o->w  = THD_BOOM_W;
    o->h  = THD_BOOM_H;
    o->hp = 1u;
    o->kind = 0u;
    o->timer = (uint16_t)THD_BOOM_MS;
    o->alive = 1u;
    o->drawn = 0u;
}

static void thd_spawn_enemy(uint8_t kind, int16_t xq, int16_t yq)
{
    ThdObj_t *o = thd_alloc(s_enemy, THD_MAX_ENEMY);

    if (o == 0)
    {
        return;
    }

    o->x = xq;
    o->y = yq;
    o->vx = 0;
    o->vy = 0;
    o->w  = THD_EN_W;
    o->h  = THD_EN_H;
    o->kind = kind;
    o->hp = (kind == THD_EK_SHOOT) ? 2u : 1u;   /* 射击机 2 血，其余 1 血 */
    o->timer = 0u;
    o->alive = 1u;
    o->drawn = 0u;
    o->px = 0;
    o->py = 0;

    switch ((int)kind)
    {
        case THD_EK_DIAG:
            o->vx = (int16_t)(-THD_E_DIAG_SPD);
            o->vy = (Game_RandRange(2u) != 0u) ? (int16_t)THD_E_DIAG_VY
                                               : (int16_t)(-THD_E_DIAG_VY);
            break;

        case THD_EK_SHOOT:
            o->vx = (int16_t)(-THD_E_SHOOT_SPD);
            break;

        case THD_EK_FAST:
            o->vx = (int16_t)(-THD_E_FAST_SPD);
            break;

        default:
            o->vx = (int16_t)(-THD_E_SMALL_SPD);
            break;
    }
}

static void thd_spawn_enemy_random(void)
{
    uint8_t idx = thd_wave_idx();
    uint8_t kind;
    uint8_t r;
    uint32_t span;
    int32_t  y;

    if (idx == 0u)
    {
        kind = THD_EK_SMALL;                    /* 第 1 波：只有小飞机 */
    }
    else if (idx == 1u)
    {
        kind = (Game_RandRange(100u) < 40u) ? THD_EK_DIAG : THD_EK_SMALL;
    }
    else
    {
        r = (uint8_t)Game_RandRange(100u);      /* 第 3 波起：四种都会出现 */
        if (r < 25u)       { kind = THD_EK_SMALL; }
        else if (r < 50u)  { kind = THD_EK_DIAG; }
        else if (r < 75u)  { kind = THD_EK_SHOOT; }
        else               { kind = THD_EK_FAST; }
    }

    span = (uint32_t)(THD_BOTTOM - THD_TOP - 4 - (int)THD_EN_H);
    y = (int32_t)THD_TOP + 2 + (int32_t)Game_RandRange(span);

    thd_spawn_enemy(kind, (int16_t)(THD_W * THD_Q), (int16_t)(y * (int32_t)THD_Q));
}

static void thd_spawn_boss(void)
{
    s_boss.x = (int16_t)(THD_W * THD_Q);
    s_boss.y = (int16_t)((((int32_t)THD_BOSS_Y_MIN + THD_BOSS_Y_MAX) / 2) * THD_Q);
    s_boss.vx = (int16_t)(-THD_BOSS_SPD);
    s_boss.vy = (int16_t)THD_BOSS_VY;
    s_boss.w  = THD_BOSS_W;
    s_boss.h  = THD_BOSS_H;
    s_boss.hp = s_boss_hp_max;          /* 需求 3：一架比一架厚 */
    s_boss.kind = THD_EK_BOSS;
    s_boss.timer = 0u;
    s_boss.alive = 1u;
    s_boss.drawn = 0u;
    s_boss.px = 0;
    s_boss.py = 0;
}

/*==========================================================================
 * 8. 开火
 *========================================================================*/
static void thd_player_fire(void)
{
    ThdObj_t *o = thd_alloc(s_pbullet, THD_MAX_PBULLET);

    if (o == 0)
    {
        return;
    }

    o->x = (int16_t)(s_player.x + ((THD_PLAYER_W - 1) * THD_Q));
    o->y = (int16_t)(s_player.y + ((THD_PLAYER_H - THD_PB_H) / 2) * THD_Q);
    o->vx = (int16_t)THD_PB_SPD;
    o->vy = 0;
    o->w  = THD_PB_W;
    o->h  = THD_PB_H;
    o->hp = 1u;
    o->kind = 0u;
    o->timer = 0u;
    o->alive = 1u;
    o->drawn = 0u;

    Game_Sound(1800u, 25u);
}

static void thd_enemy_fire(const ThdObj_t *e)
{
    ThdObj_t *o = thd_alloc(s_ebullet, THD_MAX_EBULLET);

    if (o == 0)
    {
        return;
    }

    o->x = (int16_t)(e->x + ((e->w / 2) * THD_Q));
    o->y = (int16_t)(e->y + ((e->h / 2) * THD_Q));
    o->vx = (int16_t)(-THD_EB_SPD);
    o->vy = 0;
    o->w  = THD_EB_W;
    o->h  = THD_EB_H;
    o->hp = 1u;
    o->kind = 0u;
    o->timer = 0u;
    o->alive = 1u;
    o->drawn = 0u;
}

static void thd_boss_fire(void)
{
    int8_t k;

    for (k = -1; k <= 1; k++)
    {
        ThdObj_t *o = thd_alloc(s_ebullet, THD_MAX_EBULLET);

        if (o == 0)
        {
            return;
        }

        o->x = (int16_t)(s_boss.x + (THD_EB_W * THD_Q));
        o->y = (int16_t)(s_boss.y + ((int32_t)THD_BOSS_H * THD_Q) / 2 +
                         (int32_t)k * ((int32_t)THD_BOSS_H * THD_Q) / 3);
        o->vx = (int16_t)(-THD_EB_SPD);
        o->vy = (int16_t)((int32_t)k * (THD_EB_SPD / 3));
        o->w  = THD_EB_W;
        o->h  = THD_EB_H;
        o->hp = 1u;
        o->kind = 0u;
        o->timer = 0u;
        o->alive = 1u;
        o->drawn = 0u;
    }

    Game_Sound(1400u, 30u);
}

/*==========================================================================
 * 9. 伤害 / 得分
 *========================================================================*/
static void thd_kill_enemy(ThdObj_t *e)
{
    int16_t bx = e->x;
    int16_t by = e->y;

    e->alive = 0u;
    s_score += thd_score_of(e->kind);
    s_hud_dirty = true;
    thd_spawn_boom(bx, by);
    Game_Sound(400u, 60u);
}

static void thd_damage_boss(uint16_t n)
{
    if (s_boss.alive == 0u)
    {
        return;
    }

    if ((uint16_t)s_boss.hp > n)
    {
        s_boss.hp = (uint8_t)((uint16_t)s_boss.hp - n);
        return;
    }

    /* 击落：通关 */
    s_boss.hp = 0u;
    s_boss.alive = 0u;
    s_score += THD_BOSS_SCORE;
    s_hud_dirty = true;

    thd_spawn_boom(s_boss.x, s_boss.y);
    thd_spawn_boom((int16_t)(s_boss.x + (8 * THD_Q)), s_boss.y);
    thd_spawn_boom((int16_t)(s_boss.x - (8 * THD_Q)), s_boss.y);
    Game_Sound(200u, 400u);

    /* 【2026-10-15 需求 3】原来这里直接进 WIN 通关结束。现在接着玩：
     * 先给一段喘息时间，再放一架更厚的 Boss；打掉一架奖励一颗炸弹。
     * 只有生命值归零（thd_hurt_player）才真正结束。 */
    s_boss_respawn_ms = THD_BOSS_RESPAWN_MS;
    if (s_boss_hp_max < (uint8_t)THD_BOSS_HP_MAX)
    {
        s_boss_hp_max = (uint8_t)(s_boss_hp_max + (uint8_t)THD_BOSS_HP_STEP);
        if (s_boss_hp_max > (uint8_t)THD_BOSS_HP_MAX)
        {
            s_boss_hp_max = (uint8_t)THD_BOSS_HP_MAX;
        }
    }
    if (s_bombs < (uint8_t)THD_BOMB_MAX) { s_bombs++; }
    s_hud_flash_ms = 600u;
    s_hud_dirty = true;
}

static void thd_hurt_player(void)
{
    if (s_invin_ms > 0u)
    {
        return;
    }

    Game_Sound(180u, 150u);

    if (s_lives > 0u)
    {
        s_lives--;
    }
    s_hud_dirty = true;

    if (s_lives == 0u)
    {
        /* 【需求 2】结束前存最高分（只有破纪录时才真的写 Flash） */
        (void)GS_SetBest((uint8_t)GAME_ID_THUNDER, s_score);
        s_state = (uint8_t)THD_ST_OVER;
        s_need_full = true;
        return;
    }

    s_invin_ms = THD_INVIN_MS;
    s_blink_ms = 0u;
    s_blink_on = true;
}

static void thd_use_bomb(void)
{
    uint8_t i;

    if (s_bombs == 0u)
    {
        return;
    }

    s_bombs--;
    s_hud_dirty = true;
    Game_Sound(300u, 250u);

    s_invin_ms = THD_BOMB_INVIN_MS;
    s_blink_ms = 0u;
    s_blink_on = true;

    /* 敌弹全部清掉 */
    for (i = 0; i < THD_MAX_EBULLET; i++)
    {
        if (s_ebullet[i].alive != 0u)
        {
            int16_t bx = s_ebullet[i].x;
            int16_t by = s_ebullet[i].y;

            s_ebullet[i].alive = 0u;
            thd_spawn_boom(bx, by);
        }
    }

    /* 场上敌机各扣 3 血 */
    for (i = 0; i < THD_MAX_ENEMY; i++)
    {
        if (s_enemy[i].alive == 0u)
        {
            continue;
        }
        if (s_enemy[i].hp > 3u)
        {
            s_enemy[i].hp = (uint8_t)(s_enemy[i].hp - 3u);
        }
        else
        {
            thd_kill_enemy(&s_enemy[i]);
        }
    }

    /* Boss 扣 10 血 */
    thd_damage_boss(10u);
}

/*==========================================================================
 * 10. 每帧更新
 *========================================================================*/
static void thd_update_player(uint16_t dt_ms)
{
    bool    up   = (s_ev_up   || Game_KeyDown(GKEY_UP))   ? true : false;
    bool    down = (s_ev_down || Game_KeyDown(GKEY_DOWN)) ? true : false;
    bool    fire = (s_ev_fire || Game_KeyDown(GKEY_LEFT)) ? true : false;
    int32_t dy   = 0;
    int32_t ny;

    if (up)   { dy -= (int32_t)THD_PLAYER_SPD; }
    if (down) { dy += (int32_t)THD_PLAYER_SPD; }

    if (dy != 0)
    {
        ny = (int32_t)s_player.y + ((int32_t)dy * (int32_t)dt_ms) / THD_TICK_MS;

        if (ny < ((int32_t)THD_TOP * THD_Q))
        {
            ny = (int32_t)THD_TOP * THD_Q;
        }
        if (ny > ((int32_t)(THD_BOTTOM - THD_PLAYER_H) * THD_Q))
        {
            ny = (int32_t)(THD_BOTTOM - THD_PLAYER_H) * THD_Q;
        }
        s_player.y = (int16_t)ny;
    }

    /* 连发：松开时先把"充能"填满，按下第一帧就能打出去 */
    if (fire)
    {
        s_fire_ms += dt_ms;
        if (s_fire_ms >= THD_FIRE_MS)
        {
            s_fire_ms = 0u;
            thd_player_fire();
        }
    }
    else
    {
        s_fire_ms = THD_FIRE_MS;
    }

    /* 无敌时间 + 闪烁 */
    if (s_invin_ms > 0u)
    {
        if (s_invin_ms > dt_ms)
        {
            s_invin_ms -= dt_ms;
        }
        else
        {
            s_invin_ms = 0u;
            s_blink_on = true;
        }

        s_blink_ms += dt_ms;
        if (s_blink_ms >= THD_BLINK_MS)
        {
            s_blink_ms -= THD_BLINK_MS;
            s_blink_on = (s_blink_on ? false : true);
        }
    }
    else
    {
        s_blink_on = true;
    }
}

static void thd_update_pbullet(uint16_t dt_ms)
{
    uint8_t i;

    for (i = 0; i < THD_MAX_PBULLET; i++)
    {
        ThdObj_t *o = &s_pbullet[i];

        if (o->alive == 0u)
        {
            continue;
        }

        o->x = (int16_t)(o->x + thd_step(o->vx, dt_ms, 100u));

        if (thd_px(o->x) > THD_W)
        {
            o->alive = 0u;
        }
    }
}

static void thd_update_ebullet(uint16_t dt_ms)
{
    uint8_t i;

    for (i = 0; i < THD_MAX_EBULLET; i++)
    {
        ThdObj_t *o = &s_ebullet[i];

        if (o->alive == 0u)
        {
            continue;
        }

        o->x = (int16_t)(o->x + thd_step(o->vx, dt_ms, 100u));
        o->y = (int16_t)(o->y + thd_step(o->vy, dt_ms, 100u));

        if ((thd_px(o->x) + (int16_t)o->w < 0) ||
            (thd_px(o->y) + (int16_t)o->h < THD_TOP) ||
            (thd_px(o->y) > THD_BOTTOM))
        {
            o->alive = 0u;
        }
    }
}

static void thd_update_enemy(uint16_t dt_ms)
{
    uint16_t pct = thd_speed_pct();
    uint8_t  i;

    for (i = 0; i < THD_MAX_ENEMY; i++)
    {
        ThdObj_t *o = &s_enemy[i];

        if (o->alive == 0u)
        {
            continue;
        }

        o->x = (int16_t)(o->x + thd_step(o->vx, dt_ms, pct));
        o->y = (int16_t)(o->y + thd_step(o->vy, dt_ms, pct));

        /* 撞到上下边界就弹回来（斜飞的那种） */
        if (o->y < ((int32_t)THD_TOP * THD_Q))
        {
            o->y  = (int16_t)((int32_t)THD_TOP * THD_Q);
            o->vy = (int16_t)(-o->vy);
        }
        if (o->y > ((int32_t)(THD_BOTTOM - (int)o->h) * THD_Q))
        {
            o->y  = (int16_t)((int32_t)(THD_BOTTOM - (int)o->h) * THD_Q);
            o->vy = (int16_t)(-o->vy);
        }

        if (o->kind == THD_EK_SHOOT)
        {
            o->timer = (uint16_t)(o->timer + dt_ms);
            if (o->timer >= THD_SHOOT_MS)
            {
                o->timer = 0u;
                thd_enemy_fire(o);
            }
        }

        if ((thd_px(o->x) + (int16_t)o->w) < 0)
        {
            o->alive = 0u;
        }
    }
}

static void thd_update_boss(uint16_t dt_ms)
{
    if (s_boss.alive == 0u)
    {
        return;
    }

    if (s_boss.x > ((int32_t)THD_BOSS_HOME_X * THD_Q))
    {
        /* 还在进场：一直往左飞 */
        s_boss.x = (int16_t)(s_boss.x + thd_step(s_boss.vx, dt_ms, thd_speed_pct()));
        return;
    }

    s_boss.y = (int16_t)(s_boss.y + thd_step(s_boss.vy, dt_ms, thd_speed_pct()));

    if (s_boss.y < ((int32_t)THD_BOSS_Y_MIN * THD_Q))
    {
        s_boss.y  = (int16_t)((int32_t)THD_BOSS_Y_MIN * THD_Q);
        s_boss.vy = (int16_t)(-s_boss.vy);
    }
    if (s_boss.y > ((int32_t)THD_BOSS_Y_MAX * THD_Q))
    {
        s_boss.y  = (int16_t)((int32_t)THD_BOSS_Y_MAX * THD_Q);
        s_boss.vy = (int16_t)(-s_boss.vy);
    }

    s_boss.timer = (uint16_t)(s_boss.timer + dt_ms);
    if (s_boss.timer >= THD_BOSS_FIRE_MS)
    {
        s_boss.timer = 0u;
        thd_boss_fire();
    }
}

static void thd_update_boom(uint16_t dt_ms)
{
    uint8_t i;

    for (i = 0; i < THD_MAX_BOOM; i++)
    {
        ThdObj_t *o = &s_boom[i];

        if (o->alive == 0u)
        {
            continue;
        }

        if (o->timer > dt_ms)
        {
            o->timer = (uint16_t)(o->timer - dt_ms);
        }
        else
        {
            o->timer = 0u;
            o->alive = 0u;
        }
    }
}

static void thd_update_spawn(uint16_t dt_ms)
{
    uint8_t  idx = thd_wave_idx();
    uint16_t interval;

    /* 【需求 3】上一架 Boss 被打掉之后的喘息倒计时 */
    if (s_boss_respawn_ms > 0u)
    {
        if (s_boss_respawn_ms > (uint32_t)dt_ms)
        {
            s_boss_respawn_ms -= (uint32_t)dt_ms;
            return;                     /* 喘口气：这段时间连杂兵也先不放 */
        }
        s_boss_respawn_ms = 0u;
        s_boss_spawned    = false;      /* 允许下面的分支立刻放下一架 */
    }

    /* 到点就该 Boss 登场了 */
    if ((s_boss_spawned == false) &&
        ((uint8_t)(idx + 1u) >= (uint8_t)THD_BOSS_WAVE))
    {
        s_boss_spawned = true;
        thd_spawn_boss();
        Game_Sound(120u, 400u);
        s_hud_flash_ms = 600u;
        s_hud_dirty = true;
        return;
    }

    interval = s_wave_spawn[idx];

    /* Boss 战期间把普通敌机放慢一点，不然太挤 */
    if ((s_boss.alive != 0u) && (interval < 800u))
    {
        interval = (uint16_t)(interval * 2u);
    }

    if (s_spawn_ms > dt_ms)
    {
        s_spawn_ms -= dt_ms;
        return;
    }

    s_spawn_ms = interval;
    thd_spawn_enemy_random();
}

static void thd_check_collisions(void)
{
    uint8_t i;
    uint8_t j;

    /* ---- 玩家子弹 x 敌机 ---- */
    for (i = 0; i < THD_MAX_PBULLET; i++)
    {
        ThdObj_t *b = &s_pbullet[i];

        if (b->alive == 0u)
        {
            continue;
        }

        for (j = 0; j < THD_MAX_ENEMY; j++)
        {
            ThdObj_t *e = &s_enemy[j];

            if (e->alive == 0u)
            {
                continue;
            }
            if (!thd_box_hit(b->x, b->y, (int32_t)b->w * THD_Q, (int32_t)b->h * THD_Q,
                             e->x, e->y, (int32_t)e->w * THD_Q, (int32_t)e->h * THD_Q))
            {
                continue;
            }

            b->alive = 0u;
            if (e->hp > 0u)
            {
                e->hp--;
            }
            if (e->hp == 0u)
            {
                thd_kill_enemy(e);
            }
            else
            {
                thd_spawn_boom(b->x, b->y);
                Game_Sound(900u, 20u);
            }
            break;
        }

        /* ---- 玩家子弹 x Boss ---- */
        if ((b->alive != 0u) && (s_boss.alive != 0u) &&
            thd_box_hit(b->x, b->y, (int32_t)b->w * THD_Q, (int32_t)b->h * THD_Q,
                        s_boss.x, s_boss.y, (int32_t)s_boss.w * THD_Q, (int32_t)s_boss.h * THD_Q))
        {
            b->alive = 0u;
            thd_spawn_boom(b->x, b->y);
            thd_damage_boss(1u);
        }
    }

    if (s_invin_ms > 0u)
    {
        return;                         /* 无敌期间不判定玩家被撞 */
    }

    {
        /* 玩家的碰撞盒：比图形每边内缩 1 像素（合计小 2 像素） */
        int32_t px = (int32_t)s_player.x + (THD_HIT_INSET * THD_Q);
        int32_t py = (int32_t)s_player.y + (THD_HIT_INSET * THD_Q);
        int32_t pw = (int32_t)(THD_PLAYER_W - 2 * THD_HIT_INSET) * THD_Q;
        int32_t ph = (int32_t)(THD_PLAYER_H - 2 * THD_HIT_INSET) * THD_Q;

        /* ---- 玩家 x 敌机 ---- */
        for (j = 0; j < THD_MAX_ENEMY; j++)
        {
            ThdObj_t *e = &s_enemy[j];

            if (e->alive == 0u)
            {
                continue;
            }
            if (thd_box_hit(px, py, pw, ph, e->x, e->y,
                            (int32_t)e->w * THD_Q, (int32_t)e->h * THD_Q))
            {
                int16_t bx = e->x;
                int16_t by = e->y;

                e->alive = 0u;
                thd_spawn_boom(bx, by);
                thd_hurt_player();
                return;
            }
        }

        /* ---- 玩家 x 敌弹 ---- */
        for (i = 0; i < THD_MAX_EBULLET; i++)
        {
            ThdObj_t *e = &s_ebullet[i];

            if (e->alive == 0u)
            {
                continue;
            }
            if (thd_box_hit(px, py, pw, ph, e->x, e->y,
                            (int32_t)e->w * THD_Q, (int32_t)e->h * THD_Q))
            {
                e->alive = 0u;
                thd_hurt_player();
                return;
            }
        }

        /* ---- 玩家 x Boss ---- */
        if ((s_boss.alive != 0u) &&
            thd_box_hit(px, py, pw, ph, s_boss.x, s_boss.y,
                        (int32_t)s_boss.w * THD_Q, (int32_t)s_boss.h * THD_Q))
        {
            thd_hurt_player();
        }
    }
}

/*==========================================================================
 * 11. 画
 *========================================================================*/
static void thd_draw_stars(void)
{
    uint8_t i;

    for (i = 0; i < THD_STAR_N; i++)
    {
        thd_fill(s_star[i].x, s_star[i].y, 1, 1, THD_COL_STAR);
    }
}

static void thd_draw_player(void)
{
    int16_t x = thd_px(s_player.x);
    int16_t y = thd_px(s_player.y);

    /* 三段矩形拼出一架朝右的战机：尾翼 + 机身 + 机头（文档 8.2） */
    thd_fill((int32_t)x + 0,  (int32_t)y + 3, 3, 6, THD_COL_PLAYER);
    thd_fill((int32_t)x + 3,  (int32_t)y + 1, 7, 10, THD_COL_PLAYER);
    thd_fill((int32_t)x + 10, (int32_t)y + 4, 5, 4, THD_COL_PLAYER);
    thd_fill((int32_t)x + 5,  (int32_t)y + 4, 3, 3, THD_COL_COCKPIT);

    thd_obj_mark(&s_player);
}

static void thd_draw_enemy(const ThdObj_t *e)
{
    int16_t  x = thd_px(e->x);
    int16_t  y = thd_px(e->y);
    uint16_t c = thd_enemy_color(e->kind);

    /* 倒着飞的敌机：机翼（竖条）+ 机身 + 朝左的机头 */
    thd_fill((int32_t)x + 8, y, 6, THD_EN_H, c);
    thd_fill((int32_t)x + 3, (int32_t)y + 2, 7, 7, c);
    thd_fill(x, (int32_t)y + 4, 3, 3, c);

    if (e->kind == THD_EK_SHOOT)
    {
        thd_fill((int32_t)x + 4, (int32_t)y + 4, 2, 3, ST77XX_WHITE);
    }
    else if (e->kind == THD_EK_FAST)
    {
        thd_fill((int32_t)x + 10, (int32_t)y + 3, 3, 5, ST77XX_WHITE);
    }
}

static void thd_draw_boss(const ThdObj_t *b)
{
    int16_t x = thd_px(b->x);
    int16_t y = thd_px(b->y);
    int32_t bar;

    /* 机身 + 机首 + 上下炮口 */
    thd_fill((int32_t)x + 8,  (int32_t)y + 2, 32, 22, THD_COL_BOSS);
    thd_fill(x,              (int32_t)y + 7, 8, 12, THD_COL_BOSS);
    thd_fill((int32_t)x + 22, y, 8, 3, ST77XX_RED);
    thd_fill((int32_t)x + 22, (int32_t)y + 25, 8, 3, ST77XX_RED);
    thd_fill((int32_t)x + 30, (int32_t)y + 11, 8, 8, ST77XX_YELLOW);

    /* 底部一条血条 */
    thd_fill((int32_t)x + 8, (int32_t)y + 27, 32, 3, 0x2104u);

    bar = ((int32_t)b->hp * 32) / (int32_t)s_boss_hp_max;
    if (bar > 0)
    {
        uint16_t hc = THD_COL_HP;

        /* 阈值跟着"这一架"的血量上限走（上限会随击落架数加厚） */
        if (b->hp <= (uint8_t)(s_boss_hp_max / 5u))      { hc = THD_COL_HP_LOW; }
        else if (b->hp <= (uint8_t)(s_boss_hp_max / 2u)) { hc = THD_COL_HP_MID; }

        thd_fill((int32_t)x + 8, (int32_t)y + 27, bar, 3, hc);
    }
}

static void thd_draw_boom(const ThdObj_t *o)
{
    int16_t x = thd_px(o->x);
    int16_t y = thd_px(o->y);
    uint16_t c;
    int16_t  sz;

    if (o->timer > 40u)      { c = ST77XX_ORANGE; sz = 12; }
    else if (o->timer > 20u) { c = ST77XX_ORANGE; sz = 8;  }
    else                     { c = 0x7800u;        sz = 4;  }

    thd_fill((int32_t)x + ((THD_BOOM_W - sz) / 2), (int32_t)y + ((THD_BOOM_H - sz) / 2),
             sz, sz, c);
}

static void thd_draw_bullet(ThdObj_t *o, uint16_t c)
{
    /* 【2026-10-15 修 bug】o->x / o->y 是"定点像素"（THD_Q = 16，1 像素 = 16 个单位），
     * 这个结构里所有坐标/速度都是这个粒度，画之前必须先 thd_px() 换算。
     * 这里把原始值（我机子弹 x 约 400+）直接喂给 thd_fill()，会被它开头的
     * "if (x >= THD_W) return;" 整段丢掉 —— 子弹一颗都没画出来，玩家看到的
     * "和背景同色的子弹"其实是下一帧 thd_obj_erase() 用背景色在正确
     * 位置擦出来的块。别的 draw_xxx 全都写过 thd_px()，只有这里漏了。 */
    thd_fill(thd_px(o->x), thd_px(o->y), o->w, o->h, c);
    thd_obj_mark(o);
}

static void thd_draw_hud(void)
{
    uint8_t  i;
    uint16_t band = (s_hud_flash_ms > 0u) ? THD_COL_HUD_ALERT : THD_COL_HUD;

    TFT_FillRect(0, 0, THD_W, THD_HUD_H, band);

    /* 生命：3 格血条 */
    Game_DrawText(2, 1, GTXT_LIFE, THD_COL_LABEL, band);
    for (i = 0; i < THD_LIVES_MAX; i++)
    {
        uint16_t c = (i < s_lives) ? THD_COL_HP : THD_COL_HP_OFF;
        TFT_FillRect((int16_t)(THD_HP_X + (i * THD_HP_STEP)), 5, THD_HP_W, 8, c);
    }

    /* 炸弹：剩几个画几个小方块 */
    for (i = 0; i < THD_BOMB_MAX; i++)
    {
        uint16_t c = (i < s_bombs) ? THD_COL_BOMB : THD_COL_BOMB_OFF;
        TFT_FillRect((int16_t)(THD_BOMB_X + (i * THD_BOMB_STEP)), 6, 5, 6, c);
    }

    /* 等级（波次） */
    Game_DrawText(THD_LEVEL_X, 1, GTXT_LEVEL, THD_COL_LABEL, band);
    Game_DrawNumber((int16_t)(THD_LEVEL_X + Game_TextWidth(GTXT_LEVEL) + 2), 5,
                    (uint32_t)thd_level(), 1u, THD_COL_NUM, band);

    /* 分数（右上角，右对齐） */
    Game_DrawText(THD_SCORE_X, 1, GTXT_SCORE, THD_COL_LABEL, band);
    Game_DrawNumberRight(THD_SCORE_R, 5, s_score, 1u, THD_COL_NUM, band);
}

static void thd_draw_menu(void)
{
    uint32_t best;
    int16_t  w;
    int16_t  x;

    thd_draw_stars();
    Game_DrawTextCenter(24, GTXT_THUNDER, 0x1AF4u, THD_COL_BG);

    if (s_state == (uint8_t)THD_ST_MENU)
    {
        best = GS_GetBest((uint8_t)GAME_ID_THUNDER);

        Game_DrawTextCenter(58, GTXT_ANY_KEY, THD_COL_LABEL, THD_COL_BG);

        w = (int16_t)(Game_TextWidth(GTXT_BEST) + 6 + Game_NumberWidth(best, 1u));
        x = (int16_t)((THD_W - w) / 2);
        Game_DrawText(x, 88, GTXT_BEST, THD_COL_LABEL, THD_COL_BG);
        Game_DrawNumber((int16_t)(x + Game_TextWidth(GTXT_BEST) + 6), 92, best, 1u,
                        THD_COL_NUM, THD_COL_BG);
    }

    Game_DrawHintBar(GTXT_EXIT, THD_COL_LABEL, 0xE71Cu);
}

static void thd_draw_end(GameTextId_t title, uint16_t color)
{
    /* 【需求 2】照 2048 的结束页排：整屏清干净，标题 / 本局分数 / 最高分 /
     * 提示各一行。以前只是往战场上面盖几行字，看着像游戏还在跑。 */
    uint32_t best = GS_GetBest((uint8_t)GAME_ID_THUNDER);
    int16_t  lw;
    int16_t  nw;
    int16_t  x;

    Game_Clear(THD_COL_BG);

    lw = Game_TextWidth(title);
    Game_DrawText((int16_t)((THD_W - lw) / 2), 14, title, color, THD_COL_BG);

    lw = Game_TextWidth(GTXT_SCORE);
    nw = Game_NumberWidth(s_score, 2u);
    x  = (int16_t)((THD_W - (lw + 8 + nw)) / 2);
    Game_DrawText(x, 46, GTXT_SCORE, THD_COL_LABEL, THD_COL_BG);
    Game_DrawNumber((int16_t)(x + lw + 8), 46, s_score, 2u, THD_COL_NUM, THD_COL_BG);

    lw = Game_TextWidth(GTXT_BEST);
    nw = Game_NumberWidth(best, 2u);
    x  = (int16_t)((THD_W - (lw + 8 + nw)) / 2);
    Game_DrawText(x, 72, GTXT_BEST, THD_COL_LABEL, THD_COL_BG);
    Game_DrawNumber((int16_t)(x + lw + 8), 72, best, 2u, THD_COL_BEST, THD_COL_BG);

    lw = Game_TextWidth(GTXT_ANY_KEY);
    Game_DrawText((int16_t)((THD_W - lw) / 2), 104, GTXT_ANY_KEY, THD_COL_LABEL, THD_COL_BG);
}

static void thd_erase_objects(void)
{
    uint8_t i;

    for (i = 0; i < THD_MAX_PBULLET; i++) { thd_obj_erase(&s_pbullet[i]); }
    for (i = 0; i < THD_MAX_ENEMY;  i++) { thd_obj_erase(&s_enemy[i]); }
    for (i = 0; i < THD_MAX_EBULLET; i++) { thd_obj_erase(&s_ebullet[i]); }
    for (i = 0; i < THD_MAX_BOOM;   i++) { thd_obj_erase(&s_boom[i]); }
    thd_obj_erase(&s_boss);
    thd_obj_erase(&s_player);
}

static void thd_draw_objects(void)
{
    uint8_t i;

    for (i = 0; i < THD_MAX_BOOM; i++)
    {
        if (s_boom[i].alive != 0u)
        {
            thd_draw_boom(&s_boom[i]);
            thd_obj_mark(&s_boom[i]);
        }
    }

    for (i = 0; i < THD_MAX_ENEMY; i++)
    {
        if (s_enemy[i].alive != 0u)
        {
            thd_draw_enemy(&s_enemy[i]);
            thd_obj_mark(&s_enemy[i]);
        }
    }

    if (s_boss.alive != 0u)
    {
        thd_draw_boss(&s_boss);
        thd_obj_mark(&s_boss);
    }

    for (i = 0; i < THD_MAX_EBULLET; i++)
    {
        if (s_ebullet[i].alive != 0u) { thd_draw_bullet(&s_ebullet[i], THD_COL_EBUL); }
    }

    for (i = 0; i < THD_MAX_PBULLET; i++)
    {
        if (s_pbullet[i].alive != 0u) { thd_draw_bullet(&s_pbullet[i], THD_COL_PBUL); }
    }

    /* 无敌期间闪烁：灭的时候干脆不画（擦除已经做过了） */
    if (s_blink_on)
    {
        thd_draw_player();
    }
}

static void thd_draw_all(void)
{
    Game_Clear(THD_COL_BG);

    switch ((int)s_state)
    {
        case THD_ST_BOOT:
        case THD_ST_MENU:
            thd_draw_menu();
            break;

        case THD_ST_PLAY:
        case THD_ST_PAUSE:
            thd_draw_stars();
            thd_draw_objects();
            thd_draw_hud();
            if (s_state == (uint8_t)THD_ST_PAUSE)
            {
                Game_DrawTextCenter(58, GTXT_PAUSE, 0x4208u, THD_COL_BG);
            }
            break;

        case THD_ST_OVER:
            /* 【需求 2】结束页独占整屏（不再把战场画在底下） */
            thd_draw_end(GTXT_GAME_OVER, ST77XX_RED);
            break;

        case THD_ST_WIN:
            /* 需求 3 之后不会再进这个状态，留着以防万一 */
            thd_draw_end(GTXT_WIN, ST77XX_GREEN);
            break;

        default:
            break;
    }

    s_hud_dirty = false;
}

/*==========================================================================
 * 12. 开局 / 重置
 *========================================================================*/
static void thd_reset_play(void)
{
    thd_pool_clear(s_pbullet, THD_MAX_PBULLET);
    thd_pool_clear(s_enemy,   THD_MAX_ENEMY);
    thd_pool_clear(s_ebullet, THD_MAX_EBULLET);
    thd_pool_clear(s_boom,    THD_MAX_BOOM);

    s_boss.alive = 0u;
    s_boss.drawn = 0u;
    s_boss.hp    = 0u;
    s_boss.timer = 0u;
    s_boss.x = 0;
    s_boss.y = 0;
    s_boss.vx = 0;
    s_boss.vy = 0;
    s_boss.px = 0;
    s_boss.py = 0;
    s_boss.w = THD_BOSS_W;
    s_boss.h = THD_BOSS_H;
    s_boss.kind = THD_EK_BOSS;

    s_player.alive = 1u;
    s_player.drawn = 0u;
    s_player.x = (int16_t)(THD_PLAYER_X * THD_Q);
    s_player.y = (int16_t)((((int32_t)THD_TOP + THD_BOTTOM) / 2 - (THD_PLAYER_H / 2)) * THD_Q);
    s_player.vx = 0;
    s_player.vy = 0;
    s_player.px = 0;
    s_player.py = 0;
    s_player.w  = THD_PLAYER_W;
    s_player.h  = THD_PLAYER_H;
    s_player.hp = 1u;
    s_player.kind = 0u;
    s_player.timer = 0u;

    s_score        = 0u;
    s_lives        = THD_LIVES_MAX;
    s_bombs        = THD_BOMB_MAX;
    s_t            = 0u;
    s_fire_ms      = THD_FIRE_MS;
    s_spawn_ms     = THD_FIRST_SPAWN_MS;
    s_invin_ms     = THD_INVIN_MS;      /* 开局也给 2 秒无敌，免得一进场就被撞 */
    s_blink_ms     = 0u;
    s_blink_on     = true;
    s_shown_level  = 1u;
    s_hud_flash_ms = 0u;
    s_boss_spawned = false;

    /* 【需求 3】Boss 从第一架重新开始（血量上限复位） */
    s_boss_hp_max     = (uint8_t)THD_BOSS_HP;
    s_boss_respawn_ms = 0u;

    s_ev_up   = false;
    s_ev_down = false;
    s_ev_fire = false;
}

static void thd_start(void)
{
    thd_reset_play();
    s_state = (uint8_t)THD_ST_PLAY;
    s_need_full = true;
    s_hud_dirty = true;
}

/*==========================================================================
 * 13. 对外接口
 *========================================================================*/
void THD_Enter(void)
{
    /* 共享池是按需申请的：进游戏时自己拿，
     * 不指望上层已经帮我拿过。拿不到就什么都不画。 */
    if (!Game_PoolAcquire())
    {
        return;
    }

    /* 从共享池里取自己的四段 */
    s_pbullet = Game_PoolPbullet();
    s_enemy   = Game_PoolEnemy();
    s_ebullet = Game_PoolEbullet();
    s_boom    = Game_PoolBoom();

    Game_SetOrientation(GAME_LANDSCAPE);
    (void)GS_GetDifficulty();

    thd_reset_play();

    s_state    = (uint8_t)THD_ST_BOOT;
    s_boot_ms  = 0u;
    s_need_full = true;
    s_hud_dirty = true;
}

bool THD_HandleKey(const GameKeyEvent_t *ev)
{
    bool press;

    if (ev == 0)
    {
        return false;
    }

    press = (ev->type == (uint8_t)GAME_EV_PRESS) ? true : false;

    /* 退出：编码器双击（文档「（三）3」） */
    if ((ev->key == GKEY_ENC_DBL) && press)
    {
        return true;
    }

    switch ((int)s_state)
    {
        /* ---------------- 开机 / 标题页 ---------------- */
        case THD_ST_BOOT:
        case THD_ST_MENU:
            if (press)
            {
                if ((ev->key == GKEY_K5) || (ev->key == GKEY_ENC_CLICK))
                {
                    thd_start();
                }
            }
            break;

        /* ---------------- 游戏中 ---------------- */
        case THD_ST_PLAY:
            switch ((int)ev->key)
            {
                case GKEY_K1:                   /* 向上飞（文档要求） */
                    s_ev_up = (ev->type != (uint8_t)GAME_EV_RELEASE) ? true : false;
                    break;

                case GKEY_K4:                   /* 向下飞（文档要求） */
                    s_ev_down = (ev->type != (uint8_t)GAME_EV_RELEASE) ? true : false;
                    break;

                case GKEY_K6:                   /* 开火（文档要求） */
                    s_ev_fire = (ev->type != (uint8_t)GAME_EV_RELEASE) ? true : false;
                    break;

                case GKEY_K3:                   /* 炸弹（文档要求） */
                    if (press) { thd_use_bomb(); }
                    break;

                case GKEY_ENC_CLICK:            /* 暂停 / 继续 */
                    if (press)
                    {
                        s_state = (uint8_t)THD_ST_PAUSE;
                        s_need_full = true;
                    }
                    break;

                default:
                    break;
            }
            break;

        /* ---------------- 暂停 ---------------- */
        case THD_ST_PAUSE:
            if (press && (ev->key == GKEY_ENC_CLICK))
            {
                s_state = (uint8_t)THD_ST_PLAY;
                s_need_full = true;
            }
            break;

        /* ---------------- 结束 / 通关 ---------------- */
        case THD_ST_OVER:
        case THD_ST_WIN:
            if (press && ((ev->key == GKEY_K5) || (ev->key == GKEY_ENC_CLICK)))
            {
                thd_start();
            }
            break;

        default:
            break;
    }

    return false;
}

void THD_Update(uint16_t dt_ms)
{
    if (dt_ms == 0u)
    {
        return;
    }

    if (s_state == (uint8_t)THD_ST_BOOT)
    {
        s_boot_ms += dt_ms;
        if (s_boot_ms >= THD_BOOT_MS)
        {
            s_state = (uint8_t)THD_ST_MENU;
            s_need_full = true;
        }
        return;
    }

    if (s_state != (uint8_t)THD_ST_PLAY)
    {
        return;
    }

    s_t += dt_ms;

    thd_update_player(dt_ms);
    thd_update_pbullet(dt_ms);
    thd_update_enemy(dt_ms);
    thd_update_ebullet(dt_ms);
    thd_update_boss(dt_ms);
    thd_update_spawn(dt_ms);
    thd_check_collisions();
    thd_update_boom(dt_ms);

    /* HUD 上的等级变了就重画一次 HUD */
    if (thd_level() != s_shown_level)
    {
        s_shown_level = thd_level();
        s_hud_dirty = true;
    }

    /* Boss 登场的警报：HUD 闪 4 下 */
    if (s_hud_flash_ms > 0u)
    {
        uint32_t prev = s_hud_flash_ms;

        if (s_hud_flash_ms > dt_ms)
        {
            s_hud_flash_ms -= dt_ms;
        }
        else
        {
            s_hud_flash_ms = 0u;
        }

        if ((prev / 150u) != (s_hud_flash_ms / 150u))
        {
            s_hud_dirty = true;
        }
    }
}

void THD_Draw(void)
{
    if (s_need_full)
    {
        thd_draw_all();
        s_need_full = false;
        return;
    }

    if (s_state != (uint8_t)THD_ST_PLAY)
    {
        return;                         /* 菜单 / 暂停 / 结束画面是静态的 */
    }

    thd_erase_objects();
    thd_draw_objects();

    if (s_hud_dirty)
    {
        thd_draw_hud();
        s_hud_dirty = false;
    }
}

bool     THD_IsOver(void)   { return (s_state == (uint8_t)THD_ST_OVER) || (s_state == (uint8_t)THD_ST_WIN); }
bool     THD_IsPaused(void) { return (s_state == (uint8_t)THD_ST_PAUSE); }
bool     THD_IsWin(void)    { return (s_state == (uint8_t)THD_ST_WIN); }
uint32_t THD_GetScore(void) { return s_score; }
uint8_t  THD_GetLives(void) { return s_lives; }
uint8_t  THD_GetBombs(void) { return s_bombs; }
uint8_t  THD_GetLevel(void) { return thd_level(); }
uint8_t  THD_GetState(void) { return s_state; }
bool     THD_BossAlive(void){ return (s_boss.alive != 0u); }
uint16_t THD_GetBossHp(void) { return (uint16_t)s_boss.hp; }

#if THD_TEST_HOOKS
/*==========================================================================
 * 14. 自测用的钩子（真机固件里编译不进来）
 *========================================================================*/
void THD_ClearEnemies(void)
{
    thd_pool_clear(s_enemy, THD_MAX_ENEMY);
}

void THD_ClearEnemyBullets(void)
{
    thd_pool_clear(s_ebullet, THD_MAX_EBULLET);
}

void THD_ClearPlayerBullets(void)
{
    thd_pool_clear(s_pbullet, THD_MAX_PBULLET);
}

void THD_SpawnEnemy(uint8_t kind, int16_t x, int16_t y)
{
    thd_spawn_enemy(kind, (int16_t)(x * THD_Q), (int16_t)(y * THD_Q));
}

void THD_SpawnEnemyBullet(int16_t x, int16_t y)
{
    ThdObj_t *o = thd_alloc(s_ebullet, THD_MAX_EBULLET);

    if (o == 0)
    {
        return;
    }
    o->x = (int16_t)(x * THD_Q);
    o->y = (int16_t)(y * THD_Q);
    o->vx = (int16_t)(-THD_EB_SPD);
    o->vy = 0;
    o->w  = THD_EB_W;
    o->h  = THD_EB_H;
    o->hp = 1u;
    o->kind = 0u;
    o->timer = 0u;
    o->alive = 1u;
    o->drawn = 0u;
}

void THD_SpawnBoss(void)
{
    s_boss_spawned = true;
    thd_spawn_boss();
}

void THD_SetPlayerY(int16_t y)
{
    s_player.y = (int16_t)(y * THD_Q);
}

void THD_SetTimeMs(uint32_t ms)
{
    s_t = ms;
}

void THD_DamageBoss(uint16_t n)
{
    thd_damage_boss(n);
}

void THD_HitPlayer(void)
{
    s_invin_ms = 0u;                    /* 强行命中：先解除无敌 */
    thd_hurt_player();
}

uint8_t THD_GetEnemyCount(void)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0; i < THD_MAX_ENEMY; i++)
    {
        if (s_enemy[i].alive != 0u) { n++; }
    }
    return n;
}

uint8_t THD_GetEnemyBulletCount(void)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0; i < THD_MAX_EBULLET; i++)
    {
        if (s_ebullet[i].alive != 0u) { n++; }
    }
    return n;
}

uint8_t THD_GetPlayerBulletCount(void)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0; i < THD_MAX_PBULLET; i++)
    {
        if (s_pbullet[i].alive != 0u) { n++; }
    }
    return n;
}

uint8_t THD_GetBoomCount(void)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0; i < THD_MAX_BOOM; i++)
    {
        if (s_boom[i].alive != 0u) { n++; }
    }
    return n;
}

bool THD_GetEnemy(uint8_t i, int16_t *x, int16_t *y, uint8_t *kind, uint8_t *hp)
{
    if ((i >= THD_MAX_ENEMY) || (s_enemy[i].alive == 0u))
    {
        return false;
    }
    if (x)    { *x = thd_px(s_enemy[i].x); }
    if (y)    { *y = thd_px(s_enemy[i].y); }
    if (kind) { *kind = s_enemy[i].kind; }
    if (hp)   { *hp = s_enemy[i].hp; }
    return true;
}

bool THD_GetBossBox(int16_t *x, int16_t *y, uint8_t *w, uint8_t *h)
{
    if (s_boss.alive == 0u)
    {
        return false;
    }
    if (x) { *x = thd_px(s_boss.x); }
    if (y) { *y = thd_px(s_boss.y); }
    if (w) { *w = s_boss.w; }
    if (h) { *h = s_boss.h; }
    return true;
}

bool THD_GetEnemyBullet(uint8_t i, int16_t *x, int16_t *y)
{
    if ((i >= THD_MAX_EBULLET) || (s_ebullet[i].alive == 0u))
    {
        return false;
    }
    if (x) { *x = thd_px(s_ebullet[i].x); }
    if (y) { *y = thd_px(s_ebullet[i].y); }
    return true;
}

int16_t THD_GetPlayerY(void) { return thd_px(s_player.y); }
uint8_t THD_GetPlayerW(void) { return s_player.w; }
uint8_t THD_GetPlayerH(void) { return s_player.h; }
bool    THD_IsInvincible(void) { return (s_invin_ms > 0u); }

/*********************************************************************
 * @fn      THD_GetPoolBytes
 * @brief   对象池一共占多少字节 SRAM（自测时用来卡预算）
 *********************************************************************/
uint16_t THD_GetPoolBytes(void)
{
    /* 注意：这四个现在是**指针**，不能再用 sizeof 算数组大小
     * （那会算成指针自己的 4/8 字节，静默把预算少算）。
     * 用数量乘单个实体大小。 */
    return (uint16_t)((uint16_t)(THD_MAX_PBULLET + THD_MAX_ENEMY +
                                 THD_MAX_EBULLET + THD_MAX_BOOM) * (uint16_t)sizeof(ThdObj_t)
                      + (uint16_t)sizeof(s_boss) + (uint16_t)sizeof(s_player));
}
#endif /* THD_TEST_HOOKS */

/******************* (C) CH592X 雷霆战机 *****************/
