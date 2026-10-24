/**************************************************************************
 * game_menu.c —— CH592X 小游戏主菜单实现
 *
 * 布局（横屏 240x135）：
 *
 *      y=0    |                 选择游戏（居中）
 *      y=20   | 2048              |  最高分   <- 信息区 x=130 起
 *      y=44   | 跑酷              |  12345
 *      y=68   | 雷霆战机          |  难度
 *      y=86   |                   |  普通
 *      y=106  |                   |  音效 开
 *      y=117  |              提示条（选择 / 按任意键）
 **************************************************************************/

#include "game_menu.h"

/*==========================================================================
 * 布局 / 配色
 *========================================================================*/
#define GM_TITLE_Y      0
#define GM_ROW_Y0       20
#define GM_ROW_H        24
#define GM_LEFT_X       8
#define GM_LEFT_W       116
#define GM_INFO_X       130
#define GM_INFO_W       (GAME_PANEL_H - GM_INFO_X)   /* 240-130 = 110 */

#define GM_COL_BG       0xFFFFu             /* 亮色系：白底 */
#define GM_COL_ROW      0xE71Cu             /* 未选中行：浅灰 */
#define GM_COL_ROW_SEL  0xB6BEu             /* 选中行：浅蓝 */
#define GM_COL_TITLE    0xBBA2u             /* 标题：琥珀 */
#define GM_COL_TEXT     0x0000u             /* 正文：黑 */
#define GM_COL_LABEL    0x0B6Au             /* 标签：深青 */
#define GM_COL_NUM      0x03C0u             /* 数值：深绿 */
#define GM_COL_REC      0x99AAu             /* 纪录：深洋红 */

/*==========================================================================
 * 菜单条目
 *========================================================================*/
typedef struct
{
    uint8_t      id;            /* GAME_ID_xxx */
    GameTextId_t name;          /* 中文名（GTXT_xxx）；GTXT_NONE 表示用数字显示 */
    uint16_t     number;        /* name 为 GTXT_NONE 时显示的数字（2048） */
} GmEntry_t;

static const GmEntry_t s_entries[] =
{
    { GAME_ID_2048,   GTXT_NONE,   2048u },
    { GAME_ID_RUNNER, GTXT_RUNNER, 0u    },
    { GAME_ID_THUNDER, GTXT_THUNDER, 0u  }
};

#define GM_ENTRY_COUNT  ((uint8_t)(sizeof(s_entries) / sizeof(s_entries[0])))

/*==========================================================================
 * 内部状态
 *========================================================================*/
static GmMode_t s_mode     = GM_MODE_MENU;
static uint8_t  s_sel      = 0;         /* 光标位置（数组下标） */
static uint8_t  s_diff     = GAME_DIFF_NORMAL;
static uint8_t  s_sound    = 1u;
static bool     s_need_full = true;     /* 需要整屏重画 */
static uint8_t  s_need_rows = 0xFFu;    /* 需要重画的行（bit0..bit3） */
static bool     s_need_info = true;

static uint8_t  s_res_game = GAME_ID_NONE;
static uint32_t s_res_score = 0;
static uint32_t s_res_best  = 0;
static bool     s_res_new   = false;

/*==========================================================================
 * 小工具
 *========================================================================*/
static const GmEntry_t *gm_entry(uint8_t idx)
{
    if (idx >= GM_ENTRY_COUNT)
    {
        idx = 0;
    }
    return &s_entries[idx];
}

GameTextId_t GM_GetGameNameId(uint8_t game_id)
{
    uint8_t i;

    for (i = 0; i < GM_ENTRY_COUNT; i++)
    {
        if (s_entries[i].id == game_id)
        {
            return s_entries[i].name;
        }
    }
    return GTXT_NONE;
}

static void gm_redraw_all(void)
{
    s_need_full = true;
    s_need_rows = 0xFFu;
    s_need_info = true;
}

/* 显示某个游戏的名字（中文名或数字 2048） */
static void gm_draw_name(int16_t x, int16_t y, const GmEntry_t *e,
                         uint16_t fg, uint16_t bg)
{
    if (e->name != GTXT_NONE)
    {
        Game_DrawText(x, y, e->name, fg, bg);
    }
    else
    {
        Game_DrawNumber(x, y, (uint32_t)e->number, 1u, fg, bg);
    }
}

/*==========================================================================
 * 画：一行
 *========================================================================*/
static void gm_draw_row(uint8_t i)
{
    const GmEntry_t *e = gm_entry(i);
    int16_t          y = (int16_t)(GM_ROW_Y0 + (int16_t)i * GM_ROW_H);
    uint16_t         bg = (i == s_sel) ? GM_COL_ROW_SEL : GM_COL_ROW;

    Game_DrawPanel(GM_LEFT_X, y, GM_LEFT_W, (int16_t)(GM_ROW_H - 2), bg, bg);
    gm_draw_name((int16_t)(GM_LEFT_X + 6), (int16_t)(y + 3), e, GM_COL_TEXT, bg);
}

/*==========================================================================
 * 画：右侧信息区（最高分 / 难度 / 音效）
 *========================================================================*/
static void gm_draw_info(void)
{
    const GmEntry_t *e = gm_entry(s_sel);
    uint32_t         best;
    GameTextId_t     dn;

    /* 先清干净信息区（从标题下一直到提示条上面） */
    Game_DrawPanel(GM_INFO_X, 18, GM_INFO_W, (int16_t)(Game_Height() - GAME_HINT_BAR_H - 18),
                   GM_COL_BG, GM_COL_BG);

    /* 最高分 */
    Game_DrawText(GM_INFO_X, 22, GTXT_BEST, GM_COL_LABEL, GM_COL_BG);
    best = GS_GetBest(e->id);
    Game_DrawNumber(GM_INFO_X, 42, best, 2u, GM_COL_NUM, GM_COL_BG);

    /* 难度 */
    Game_DrawText(GM_INFO_X, 66, GTXT_DIFFICULTY, GM_COL_LABEL, GM_COL_BG);

    if (s_diff == (uint8_t)GAME_DIFF_EASY)      { dn = GTXT_EASY; }
    else if (s_diff == (uint8_t)GAME_DIFF_HARD) { dn = GTXT_HARD; }
    else                                        { dn = GTXT_NORMAL; }
    Game_DrawText(GM_INFO_X, 86, dn, GM_COL_TEXT, GM_COL_BG);

    /* 音效（K7 开关，现在只存设置，还没接蜂鸣器） */
    Game_DrawText(GM_INFO_X, 106, GTXT_SOUND, GM_COL_LABEL, GM_COL_BG);
    Game_DrawText((int16_t)(GM_INFO_X + 48), 106,
                  (s_sound != 0u) ? GTXT_ON : GTXT_OFF, GM_COL_TEXT, GM_COL_BG);
}

/*==========================================================================
 * 画：成绩页
 *========================================================================*/
static void gm_draw_result(void)
{
    GameTextId_t name = GM_GetGameNameId(s_res_game);

    Game_Clear(GM_COL_BG);

    /* 游戏名 */
    if (name != GTXT_NONE)
    {
        Game_DrawTextCenter(6, name, GM_COL_TITLE, GM_COL_BG);
    }
    else
    {
        Game_DrawNumber((int16_t)((Game_Width() - 24) / 2), 6, 2048u, 1u,
                        GM_COL_TITLE, GM_COL_BG);
    }

    /* 分数 */
    Game_DrawTextCenter(32, GTXT_SCORE, GM_COL_TEXT, GM_COL_BG);
    Game_DrawNumber((int16_t)((Game_Width() - (int16_t)Game_NumberWidth(s_res_score, 3u)) / 2),
                    50, s_res_score, 3u, GM_COL_NUM, GM_COL_BG);

    /* 最高分 */
    Game_DrawTextCenter(78, GTXT_BEST, GM_COL_LABEL, GM_COL_BG);
    Game_DrawNumber((int16_t)((Game_Width() - (int16_t)Game_NumberWidth(s_res_best, 2u)) / 2),
                    94, s_res_best, 2u, GM_COL_TEXT, GM_COL_BG);

    if (s_res_new)
    {
        Game_DrawTextCenter(112, GTXT_NEW_RECORD, GM_COL_REC, GM_COL_BG);
    }

    Game_DrawHintBar(GTXT_ANY_KEY, GM_COL_TEXT, 0xE71Cu);
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
void GM_Enter(void)
{
    uint8_t last;

    GS_Init();                              /* 读纪录/设置（有 CRC 校验） */

    s_diff     = GS_GetDifficulty();
    s_sound    = GS_GetSound();
    s_mode     = GM_MODE_MENU;
    s_res_game = GAME_ID_NONE;

    /* 光标停在上次玩的游戏上 */
    last = GS_GetLastGame();
    s_sel = 0;
    if (last != GAME_ID_NONE)
    {
        uint8_t i;
        for (i = 0; i < GM_ENTRY_COUNT; i++)
        {
            if (s_entries[i].id == last)
            {
                s_sel = i;
                break;
            }
        }
    }

    Game_SetOrientation(GAME_LANDSCAPE);
    gm_redraw_all();
    GM_Draw();                              /* 先铺一次整屏 */
}

uint8_t GM_GetSelected(void)
{
    return gm_entry(s_sel)->id;
}

uint8_t GM_GetDifficulty(void)
{
    return s_diff;
}

GmMode_t GM_GetMode(void)
{
    return s_mode;
}

uint32_t GM_GetResultScore(void) { return s_res_score; }
uint32_t GM_GetResultBest(void)  { return s_res_best; }
bool     GM_GetResultNewRecord(void) { return s_res_new; }

void GM_ShowResult(uint8_t game_id, uint32_t score)
{
    s_mode      = GM_MODE_RESULT;
    s_res_game  = game_id;
    s_res_score = score;
    s_res_new   = GS_SetBest(game_id, score);   /* 破纪录会自动写数据 Flash */
    s_res_best  = GS_GetBest(game_id);

    Game_SetOrientation(GAME_LANDSCAPE);
    gm_draw_result();
}

bool GM_HandleKey(const GameKeyEvent_t *ev)
{
    if (ev == 0)
    {
        return false;
    }

    /* ---------------- 成绩页：任意键回菜单 ---------------- */
    if (s_mode == GM_MODE_RESULT)
    {
        if (ev->type == GAME_EV_PRESS || ev->type == GAME_EV_REPEAT)
        {
            s_mode = GM_MODE_MENU;
            gm_redraw_all();
            GM_Draw();                      /* 立刻把菜单画回来 */
        }
        return false;
    }

    /* ---------------- 菜单 ---------------- */
    switch ((int)ev->key)
    {
        case GKEY_ENC_CW:
        case GKEY_K6:
            s_sel = (uint8_t)((s_sel + 1u) % GM_ENTRY_COUNT);
            s_need_rows = 0xFFu;
            s_need_info = true;
            break;

        case GKEY_ENC_CCW:
        case GKEY_K4:
            s_sel = (uint8_t)((s_sel + GM_ENTRY_COUNT - 1u) % GM_ENTRY_COUNT);
            s_need_rows = 0xFFu;
            s_need_info = true;
            break;

        case GKEY_K1:                       /* 难度 - */
            if (s_diff > (uint8_t)GAME_DIFF_EASY)
            {
                s_diff--;
                GS_SetDifficulty(s_diff);
                GS_Save();
                s_need_info = true;
            }
            break;

        case GKEY_K3:                       /* 难度 + */
            if (s_diff < (uint8_t)GAME_DIFF_HARD)
            {
                s_diff++;
                GS_SetDifficulty(s_diff);
                GS_Save();
                s_need_info = true;
            }
            break;

        case GKEY_K7:                       /* 音效开关 */
            s_sound = (s_sound != 0u) ? 0u : 1u;
            GS_SetSound(s_sound);
            GS_Save();
            s_need_info = true;
            break;

        case GKEY_K5:
        case GKEY_ENC_BTN:
            if (ev->type == GAME_EV_PRESS)
            {
                GS_SetLastGame(GM_GetSelected());
                GS_Save();
                return true;                /* 交给 main 去启动游戏 */
            }
            break;

        default:
            break;
    }

    return false;
}

void GM_Update(uint16_t dt_ms)
{
    (void)dt_ms;                            /* 菜单是事件驱动的，这里不用做事 */
}

void GM_Draw(void)
{
    uint8_t i;

    if (s_mode == GM_MODE_RESULT)
    {
        return;                             /* 成绩页在 GM_ShowResult 里已经画好了 */
    }

    if (s_need_full)
    {
        Game_Clear(GM_COL_BG);
        Game_DrawTextCenter(GM_TITLE_Y, GTXT_SELECT_GAME, GM_COL_TITLE, GM_COL_BG);
        s_need_full = false;
        s_need_rows = 0xFFu;
        s_need_info = true;
    }

    for (i = 0; i < GM_ENTRY_COUNT; i++)
    {
        if ((s_need_rows & (uint8_t)(1u << i)) != 0u)
        {
            gm_draw_row(i);
        }
    }
    s_need_rows = 0u;

    if (s_need_info)
    {
        gm_draw_info();
        Game_DrawHintBar(GTXT_CONFIRM, GM_COL_TEXT, 0xE71Cu);
        s_need_info = false;
    }
}

/******************* (C) CH592X 小游戏菜单 *****************/
