/**************************************************************************
 * ui_carousel.c —— 磁贴轮播控件实现
 *
 * 磁贴样式：**直接贴 60x60 的彩色圆角 App 图标位图**（icon_mode / icon_game），
 *   不再用"底色 + 描边 + 矢量图标"拼。
 *   图标四角是白的（图上就是白底圆角），图形本身也有白色元素，
 *   所以**不能用白色做色键** —— 靠**白色页面背景**自然融合。
 *
 * 选中态：选中的放大到 96x96，左右相邻缩小到 72x72（cover-flow 手感），
 *   不做颜色处理（改了颜色会把白色图形也一起改掉）。
 *
 * 其余（局部刷新 / 2 帧动画 / 页面代号）与之前一致。
 **************************************************************************/

#include "ui_carousel.h"
#include "ui_page.h"       /* UI_Epoch() */
#include "ui_theme.h"
#include "ui_anim.h"
#include "ui_icon.h"
#include "ui_iconbmp.h"
#include "ui_widget.h"
#include "tft_gfx.h"
#include "game_port.h"
#include "game_text.h"

/*==========================================================================
 * 布局
 *========================================================================*/
#define TILE_SP     120                         /* 相邻中心间距（露一半的关键） */
#define ICON_RING_R 12                          /* 选中描边圈的圆角（图标实测圆角 ≈8.5 + 外扩 4） */

#define TILE_CX     ((int16_t)(SCR_W / 2))
#define BAND_Y      ((int16_t)SBAR_H)           /* 磁贴带顶 = 状态栏底 */
#define CAP_Y       ((int16_t)(SCR_H - 15))     /* 底栏顶 */
#define BAND_H      ((int16_t)(CAP_Y - BAND_Y))
#define TILE_CY     ((int16_t)(BAND_Y + BAND_H / 2))

/* 每帧要清掉的范围：**只清磁贴真正占用的高度**，不要把整条带都刷一遍。
 * 磁贴 60px 高，选中描边再外扩 5px，留 1px 余量 -> 72px。
 * 带的上下留白（y 18~32 和 y 105~119）自始至终只有底色，没有东西画进去，
 * 所以根本不需要每帧重刷 —— 240x102 -> 240x72，每帧省 7200 像素（约 5.8ms @20MHz）。 */
#define TILE_CLR_PAD  6
#define BAND_CLR_Y    ((int16_t)(TILE_CY - (int16_t)(ICONBMP_H / 2) - TILE_CLR_PAD))
#define BAND_CLR_H    ((int16_t)(ICONBMP_H + 2 * TILE_CLR_PAD))

/* 配色直接用主题（现在是浅色主题，见 ui_theme.h） */
#define BG_MAIN     C_BG
#define TXT_DK      C_TEXT

/*--------------------------------------------------------------------------
 * 动画帧数：一次切换只画这么多帧（1 帧 = 20ms）
 *------------------------------------------------------------------------*/
#define CAR_FRAME_MS    20u

/* 切换动画帧数：0 = 不做过渡，一帧直接到位。
 *
 * 【2026-10-12 去掉动画】原来是 2 帧。一次切换 = 2 次整条磁贴带重画，
 *   每帧约 25ms（清带 13.4ms + 4 张 60x60 图标 11.5ms），而 UI 帧预算是 20ms
 *   —— 超支 25%，表现就是"动画一顿一顿"；更糟的是这两帧会把整个主循环
 *   卡住约 50ms，打字 / 转编码器全都跟着迟滞。
 *
 * 改成 0 之后：Anim_Start(dur=0) 走 ui_anim.c 的快捷分支，直接把镜头归零、
 *   active=0，一帧画到位。每次切换只重画一次（省掉一半），也不再占用动画帧，
 *   UiCar_Update() 基本变成空操作。
 *
 * 【想恢复过渡效果】把下面改回 2u（要更平滑就用 4u/6u，但每帧一样贵）。 */
#define SWITCH_FRAMES   0u
#define SWITCH_MS       ((uint16_t)(SWITCH_FRAMES * CAR_FRAME_MS))

/* OK 按下时磁贴外圈再补一圈闪烁：0 = 关（默认） */
#ifndef CAR_TAP_FLASH
#define CAR_TAP_FLASH   0
#endif

/* 入场过渡帧数：0 = 进页面直接画到位 */
#define ENTRANCE_FRAMES 0u

/*==========================================================================
 * 位置：环上最近距离
 *========================================================================*/
static int16_t rel_slot(const uicar_t *c, uint8_t i)
{
    int16_t d    = (int16_t)((int16_t)i - (int16_t)c->sel);
    int16_t half = (int16_t)(c->n / 2);

    if (d >  half) { d = (int16_t)(d - (int16_t)c->n); }
    if (d < -half) { d = (int16_t)(d + (int16_t)c->n); }
    return d;
}

/*==========================================================================
 * 绘制
 *========================================================================*/

/* 顶部状态栏：白底 + 深字 + 选中色强调线 */
static void draw_status_bar(const uicar_t *c)
{
    TFT_FillRect(0, 0, (int16_t)SCR_W, (int16_t)SBAR_H, BG_MAIN);

    /* 右上角电量（白底 → 用深色画） */
    UiW_DrawBatBar(150, 0, (int16_t)SBAR_H, BG_MAIN, TXT_DK);

    /* 左边标题 */
    Game_DrawText(4, 1, (GameTextId_t)c->sbar_title, TXT_DK, BG_MAIN);

    /* 底部一条选中色细线 */
    TFT_FillRect(0, (int16_t)(SBAR_H - 1), (int16_t)SCR_W, 1,
                 Theme_Dim(c->items[c->sel].color, 80));
}

/* 底部标题条：白底 + 深字 + 选中色顶线 */
static void draw_caption(const uicar_t *c)
{
    const uicar_item_t *it = &c->items[c->sel];
    int16_t  cy = (int16_t)(CAP_Y + 8);
    int16_t  tw;
    int16_t  tx;
    uint16_t ac = Theme_Dim(it->color, 80);

    TFT_FillRect(0, CAP_Y, (int16_t)SCR_W, 15, BG_MAIN);
    TFT_FillRect(0, CAP_Y, (int16_t)SCR_W, 1, ac);

    if (it->title != (int16_t)GTXT_NONE)
    {
        tw = Game_TextWidth((GameTextId_t)it->title);
        tx = (int16_t)((SCR_W - tw) / 2);
        Game_DrawText(tx, (int16_t)(CAP_Y + 1),
                      (GameTextId_t)it->title, TXT_DK, BG_MAIN);
    }
    else
    {
        /* 没有中文字模的项（如 2048）用数字画，scale 2 = 16px 高 */
        tw = Game_NumberWidth(it->num, 2u);
        tx = (int16_t)((SCR_W - tw) / 2);
        Game_DrawNumber(tx, (int16_t)(CAP_Y + 1), it->num, 2u, TXT_DK, BG_MAIN);
    }

    /* 左右两个小三角：提示可以左右循环切换 */
    TFT_FillTriangle(7, (int16_t)(cy - 4), 7, (int16_t)(cy + 4), 2, cy, ac);
    TFT_FillTriangle(232, (int16_t)(cy - 4), 232, (int16_t)(cy + 4), 237, cy, ac);
}

/* 单张磁贴：**原始 60x60 直接贴，不缩放**（裁剪在 IconBmp_Draw 内部处理） */
static void draw_tile(const uicar_t *c, uint8_t i)
{
    int16_t slot_q8 = (int16_t)(rel_slot(c, i) * 256) + Anim_Value(&c->cam);
    int16_t cx   = (int16_t)(TILE_CX +
                             ((int32_t)slot_q8 * TILE_SP) / 256 +
                             Anim_Value(&c->entdx));
    int16_t x    = (int16_t)(cx - (int16_t)(ICONBMP_W / 2));
    int16_t y    = (int16_t)(TILE_CY - (int16_t)(ICONBMP_H / 2));
    uint8_t sel  = (uint8_t)(i == c->sel);
    uint8_t full = (uint8_t)((x >= 0) &&
                             ((x + (int16_t)ICONBMP_W) <= (int16_t)SCR_W));

    if (c->items[i].bmp == 0)
    {
        return;                     /* 没给位图就不画 */
    }

    IconBmp_Draw(c->items[i].bmp, x, y);

    /* 选中态：套一圈圆角描边。
     * 图标四角是白的，白底上本来就看不见边界，描一圈最直观，
     * 也就不用靠缩放来区分选中了（用户要求不加缩放）。
     *
     * ★ 只有完整可见才描边：TFT_DrawRoundRect 走 write_pixel_raw，
     *   **没有越界裁剪**，传负坐标会把数据写进 GRAM 的乱地址。
     *   （滑动动画中相邻那张会有一半在屏外，正好被这个判断挡住。） */
    if ((sel != 0u) && (full != 0u))
    {
        TFT_DrawRoundRect((int16_t)(x - 4), (int16_t)(y - 4),
                          (int16_t)(ICONBMP_W + 8), (int16_t)(ICONBMP_H + 8),
                          ICON_RING_R, C_ACCENT);
        TFT_DrawRoundRect((int16_t)(x - 3), (int16_t)(y - 3),
                          (int16_t)(ICONBMP_W + 6), (int16_t)(ICONBMP_H + 6),
                          (int16_t)(ICON_RING_R - 1), C_ACCENT);

        if (c->flash != 0u)     /* OK 按下时外面再补一圈，做确认反馈 */
        {
            TFT_DrawRoundRect((int16_t)(x - 5), (int16_t)(y - 5),
                              (int16_t)(ICONBMP_W + 10), (int16_t)(ICONBMP_H + 10),
                              (int16_t)(ICON_RING_R + 1), C_ACCENT);
        }
    }
}

/* 磁贴带：清一条 + 画全部
 *
 * 【性能 2026-10-12】清的范围从整条带（240x102 = 24480 像素 = 19.6ms）
 * 收到磁贴实际占用高度（240x72 = 17280 像素 = 13.4ms），每帧省 5.8ms。
 * 视觉上完全一样：被省掉的那两条只有底色，本来就没东西画进去。 */
static void draw_band(const uicar_t *c)
{
    uint8_t i;

    TFT_FillRect(0, BAND_CLR_Y, (int16_t)SCR_W, BAND_CLR_H, BG_MAIN);

    for (i = 0; i < c->n; i++)
    {
        draw_tile(c, i);
    }
}

/*==========================================================================
 * 对外
 *========================================================================*/
void UiCar_Init(uicar_t *c, const uicar_item_t *items, uint8_t n,
                int16_t sbar_title)
{
    if ((c == 0) || (items == 0)) { return; }

    if (n < 2u)                 { n = 2u; }
    if (n > (uint8_t)UICAR_MAX) { n = (uint8_t)UICAR_MAX; }

    c->items      = items;
    c->n          = n;
    c->sel        = 0u;
    c->flash      = 0u;
    c->entered    = 0u;
    c->sbar_title = sbar_title;

    c->full_dirty = 1u;
    c->band_dirty = 1u;
    c->cap_dirty  = 1u;
    c->epoch      = 0xFFFFFFFFu;
}

void UiCar_Enter(uicar_t *c)
{
    if (c == 0) { return; }

    c->sel        = 0u;
    c->flash      = 0u;

    Anim_Start(&c->cam, 0, 0, 0, EASE_LINEAR);

#if (ENTRANCE_FRAMES > 0u)
    Anim_Start(&c->entdx, 26, 0,
               (uint16_t)(ENTRANCE_FRAMES * CAR_FRAME_MS), EASE_LINEAR);
    c->entered = 0u;
#else
    Anim_Start(&c->entdx, 0, 0, 0, EASE_LINEAR);
    c->entered = 1u;
#endif

    c->full_dirty = 1u;
    c->band_dirty = 1u;
    c->cap_dirty  = 1u;
    c->epoch      = UI_Epoch();

    UI_Dirty();
}

void UiCar_Step(uicar_t *c, int8_t dir)
{
    uint8_t old;

    if ((c == 0) || (c->n < 2u)) { return; }

    old    = c->sel;
    c->sel = (uint8_t)(((int16_t)c->sel + (int16_t)dir + (int16_t)c->n) %
                       (int16_t)c->n);
    if (c->sel == old)
    {
        return;
    }

    Anim_Start(&c->cam, (int16_t)((int16_t)dir * 256), 0, SWITCH_MS, EASE_LINEAR);

    c->band_dirty = 1u;
    c->cap_dirty  = 1u;
    UI_Dirty();
}

void UiCar_Tap(uicar_t *c)
{
    if (c == 0) { return; }

#if (CAR_TAP_FLASH)
    /* 原行为：额外再闪 2 帧（每帧约 25ms 的整条带重画） */
    c->flash      = 2u;
    c->band_dirty = 1u;
    UI_Dirty();
#else
    /* 【2026-10-12】关掉。OK 按下后页面立刻就被推走了，这个闪烁根本看不见，
     * 却要让磁贴带多刷 2 遍 —— 纯浪费。想恢复就打开这个开关。 */
    (void)c;
#endif
}

void UiCar_Update(uicar_t *c, uint16_t dt_ms)
{
    uint8_t any = 0u;

    if (c == 0) { return; }

    if (c->entered == 0u)
    {
        if (Anim_Active(&c->entdx))
        {
            (void)Anim_Step(&c->entdx, dt_ms);
        }
        else
        {
            c->entered = 1u;
        }
        c->band_dirty = 1u;
        any = 1u;
    }

    /* 注意：Anim_Step 收尾那帧返回 false 但值已到位，
     * 所以必须"先看 Anim_Active 再 Step" */
    if (Anim_Active(&c->cam))
    {
        (void)Anim_Step(&c->cam, dt_ms);
        c->band_dirty = 1u;
        any = 1u;
    }

    if (c->flash != 0u)
    {
        c->flash--;
        c->band_dirty = 1u;
        any = 1u;
    }

    if (any != 0u)
    {
        UI_Dirty();
    }
}

void UiCar_Draw(uicar_t *c)
{
    uint32_t ep;

    if (c == 0) { return; }

    /* 页面被切走过 -> dirty 标记不可信，整页重画 */
    ep = UI_Epoch();
    if (c->epoch != ep)
    {
        c->epoch      = ep;
        c->full_dirty = 1u;
    }

    if (c->full_dirty != 0u)
    {
        c->full_dirty = 0u;
        c->band_dirty = 0u;
        c->cap_dirty  = 0u;

        TFT_FillRect(0, 0, (int16_t)SCR_W, (int16_t)SCR_H, BG_MAIN);
        draw_status_bar(c);
        draw_band(c);
        draw_caption(c);
        return;
    }

    if (c->cap_dirty != 0u)
    {
        c->cap_dirty = 0u;
        TFT_FillRect(0, (int16_t)(SBAR_H - 1), (int16_t)SCR_W, 1,
                     Theme_Dim(c->items[c->sel].color, 80));
        draw_caption(c);
    }

    if (c->band_dirty != 0u)
    {
        c->band_dirty = 0u;
        draw_band(c);
    }
}

uint8_t UiCar_Sel(const uicar_t *c)
{
    return (c != 0) ? c->sel : 0u;
}
