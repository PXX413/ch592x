/**************************************************************************
 * ui_card.c —— 卡片与绘制原语实现
 **************************************************************************/

#include "ui_card.h"
#include "ui_theme.h"
#include "tft_gfx.h"
#include "game_text.h"
#include "game_port.h"

/* 定点：v * t / 256 */
static int16_t sc(int16_t v, int16_t t)
{
    int32_t r = (int32_t)v * (int32_t)t;
    if (r >= 0) { r += 128; } else { r -= 128; }
    return (int16_t)(r / 256);
}

/*==========================================================================
 * 卡片
 *========================================================================*/
void UiCard_Init(card_t *c, int16_t x, int16_t y, int16_t w, int16_t h,
                 icon_t ic, int16_t title_id)
{
    if (c == 0) { return; }
    c->x        = x;
    c->y        = y;
    c->w        = w;
    c->h        = h;
    c->icon     = ic;
    c->title_id = title_id;
    c->scale    = 256;
}

void UiCard_Draw(const card_t *c, uint8_t sel, int16_t glow, int16_t icon_size)
{
    int16_t  dw, dh, dx, dy;        /* 实际（缩放后）几何 */
    int16_t  icx, icy;              /* 图标位置 */
    uint16_t fill, border;
    int16_t  r;
    int16_t  i;

    if (c == 0) { return; }

    /* 按 scale 缩放宽高，中心保持不动 */
    dw = sc(c->w, c->scale);
    dh = sc(c->h, c->scale);
    dx = (int16_t)(c->x + (c->w - dw) / 2);
    dy = (int16_t)(c->y + (c->h - dh) / 2);

    r = (int16_t)sc((int16_t)TILE_RADIUS, c->scale);
    if (r < 1) { r = 1; }

    /* ---- 配色 ----
     * 未选中：暗卡片底 + 灰调描边
     * 选中  ：亮卡片底 + 强调色描边（随 glow 提亮） */
    if (sel != 0u)
    {
        int16_t g = glow;
        if (g < 0)   { g = 0; }
        if (g > 256) { g = 256; }

        fill   = Theme_Blend(C_BG_CARD_HI, C_ACCENT, (int16_t)(g / 3));
        border = Theme_Blend(C_ACCENT_DIM, C_ACCENT, g);
    }
    else
    {
        fill   = C_BG_CARD;
        border = C_BG_PANEL;
    }

    /* ---- 阴影（仅选中态画，制造浮起感） ---- */
    if (sel != 0u)
    {
        for (i = 1; i <= 3; i++)
        {
            int16_t t = (int16_t)(3 - i);
            uint16_t sc_col = Theme_Blend(C_BG, C_BG_PANEL, (int16_t)(t * 60));
            TFT_FillRoundRect((int16_t)(dx + i), (int16_t)(dy + i),
                              dw, dh, r, sc_col);
        }
    }

    /* ---- 卡片主体 ---- */
    TFT_FillRoundRect(dx, dy, dw, dh, r, fill);

    /* 描边（选中 2px，未选中 1px） */
    if (sel != 0u)
    {
        TFT_DrawRoundRect(dx, dy, dw, dh, r, border);
        TFT_DrawRoundRect((int16_t)(dx + 1), (int16_t)(dy + 1),
                          (int16_t)(dw - 2), (int16_t)(dh - 2),
                          (int16_t)(r - 1), border);
    }
    else
    {
        TFT_DrawRoundRect(dx, dy, dw, dh, r, border);
    }

    /* ---- 顶部高光细线（立体感） ---- */
    if (dw > 12)
    {
        uint16_t hl = (sel != 0u) ? Theme_Light(fill, 45) : Theme_Light(fill, 22);
        TFT_FillRect((int16_t)(dx + r), (int16_t)(dy + 1),
                     (int16_t)(dw - 2 * r), 1, hl);
    }

    /* ---- 图标（水平居中，垂直偏向中上） ---- */
    {
        int16_t isz = sc(icon_size, c->scale);
        icx = (int16_t)(dx + (dw - isz) / 2);
        icy = (int16_t)(dy + (dh - isz) / 2 - (int16_t)sc(8, c->scale));

        if (icy < (int16_t)(dy + 3))
        {
            icy = (int16_t)(dy + 3);
        }

        Icon_Draw(icx, icy, c->icon,
                  (sel != 0u) ? C_TEXT : C_TEXT_DIM, isz);
    }

    /* ---- 标题（底部居中） ---- */
    if ((c->title_id != 0) && (dh > 30))
    {
        int16_t tw = Game_TextWidth((GameTextId_t)c->title_id);
        int16_t tx = (int16_t)(dx + (dw - tw) / 2);
        int16_t ty = (int16_t)(dy + dh - 17);

        Game_DrawText(tx, ty, (GameTextId_t)c->title_id,
                      (sel != 0u) ? C_TEXT : C_TEXT_DIM,
                      fill);

        /* 选中时标题加一条下方强调线 */
        if (sel != 0u)
        {
            TFT_FillRect((int16_t)(dx + dw / 2 - 8), (int16_t)(dy + dh - 3),
                         16, 1, C_ACCENT);
        }
    }
}

/*==========================================================================
 * 面板
 *========================================================================*/
void UiCard_Panel(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                  uint16_t fill, uint16_t border, uint16_t hi_line)
{
    if ((w <= 0) || (h <= 0)) { return; }

    TFT_FillRoundRect(x, y, w, h, r, fill);
    if (border != fill)
    {
        TFT_DrawRoundRect(x, y, w, h, r, border);
    }
    /* 顶部高光线 */
    if ((hi_line != 0u) && (w > 2 * r + 4))
    {
        TFT_FillRect((int16_t)(x + r), y, (int16_t)(w - 2 * r), 1, hi_line);
    }
}

/*==========================================================================
 * 渐变条
 *========================================================================*/
void UiCard_GradBar(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t left, uint16_t right)
{
    int16_t i;

    if ((w <= 0) || (h <= 0)) { return; }

    for (i = 0; i < w; i++)
    {
        int16_t t = (int16_t)(((int32_t)i * 256) / (w > 1 ? w - 1 : 1));
        TFT_FillRect((int16_t)(x + i), y, 1, h, Theme_Blend(left, right, t));
    }
}

/*==========================================================================
 * 分段进度条
 *========================================================================*/
void UiCard_SegBar(int16_t x, int16_t y, int16_t w, int16_t h,
                   uint8_t segs, uint8_t lit, uint16_t on, uint16_t off)
{
    uint8_t i;
    int16_t sw;
    int16_t gap = 1;

    if ((segs == 0u) || (w <= 0)) { return; }

    sw = (int16_t)((w - (int16_t)(segs - 1) * gap) / segs);
    if (sw < 1) { sw = 1; }

    for (i = 0; i < segs; i++)
    {
        int16_t sx = (int16_t)(x + (int16_t)i * (sw + gap));
        TFT_FillRect(sx, y, sw, h, (i < lit) ? on : off);
    }
}

/*==========================================================================
 * 选中框（呼吸）
 *========================================================================*/
void UiCard_FocusFrame(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t col, int16_t glow)
{
    int16_t  g = glow;
    uint16_t c;

    if (g < 0)   { g = 0; }
    if (g > 256) { g = 256; }

    /* 外圈：随呼吸变亮 */
    c = Theme_Blend(C_ACCENT_DIM, col, g);
    TFT_DrawRoundRect(x, y, w, h, r, c);

    /* 内圈：稍暗，做出双层框的精致感 */
    if ((w > 6) && (h > 6))
    {
        TFT_DrawRoundRect((int16_t)(x + 1), (int16_t)(y + 1),
                          (int16_t)(w - 2), (int16_t)(h - 2),
                          (int16_t)((r > 1) ? r - 1 : 0),
                          Theme_Blend(C_BG_PANEL, c, (int16_t)(g / 2)));
    }

    /* 四角强调（呼吸到峰值时出现） */
    if (g > 180)
    {
        int16_t L = 4;
        TFT_FillRect(x, y, L, 1, C_TEXT);
        TFT_FillRect(x, y, 1, L, C_TEXT);
        TFT_FillRect((int16_t)(x + w - L), y, L, 1, C_TEXT);
        TFT_FillRect((int16_t)(x + w - 1), y, 1, L, C_TEXT);
        TFT_FillRect(x, (int16_t)(y + h - 1), L, 1, C_TEXT);
        TFT_FillRect(x, (int16_t)(y + h - L), 1, L, C_TEXT);
        TFT_FillRect((int16_t)(x + w - L), (int16_t)(y + h - 1), L, 1, C_TEXT);
        TFT_FillRect((int16_t)(x + w - 1), (int16_t)(y + h - L), 1, L, C_TEXT);
    }
}

/*==========================================================================
 * 柔和阴影
 *========================================================================*/
void UiCard_SoftShadow(int16_t x, int16_t y, int16_t w, int16_t h, int16_t depth)
{
    int16_t i;

    (void)h;
    if (depth <= 0) { return; }

    for (i = 1; i <= depth; i++)
    {
        uint16_t c = Theme_Blend(C_BG, C_BG_PANEL, (int16_t)((depth - i) * 30));
        TFT_FillRect((int16_t)(x + i), (int16_t)(y + i), w, 1, c);
    }
}
