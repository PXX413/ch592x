/**************************************************************************
 * ui_list.c —— 列表控件实现
 **************************************************************************/

#include "ui_list.h"
#include "tft_gfx.h"
#include "game_text.h"

/*--------------------------------------------------------------------------
 * 所有初始化过的列表都记一下，好让 UiList_InvalidateAll() 一网打尽。
 * 页面就那么几个，12 个位置足够；满了就顶掉最后一个。
 *------------------------------------------------------------------------*/
#define UI_LIST_REG_MAX     12
static ui_list_t *s_reg[UI_LIST_REG_MAX];
static uint8_t    s_reg_n;

static void ui_list_register(ui_list_t *l)
{
    uint8_t i;

    for (i = 0u; i < s_reg_n; i++)
    {
        if (s_reg[i] == l)
        {
            return;                     /* 已经记过 */
        }
    }

    if (s_reg_n < (uint8_t)UI_LIST_REG_MAX)
    {
        s_reg[s_reg_n] = l;
        s_reg_n++;
    }
    else
    {
        s_reg[UI_LIST_REG_MAX - 1] = l;
    }
}

void UiList_InvalidateAll(void)
{
    uint8_t i;

    for (i = 0u; i < s_reg_n; i++)
    {
        if (s_reg[i] != 0)
        {
            s_reg[i]->last_sel = 0xFFu;
        }
    }
}

void UiList_InitAt(ui_list_t *l, int16_t frame_x, int16_t frame_y, int16_t item_w,
                   int16_t view_x, int16_t view_y, int16_t view_w, int16_t view_h,
                   ui_list_draw_fn fn, void *user)
{
    if (l == 0)
    {
        return;
    }

    l->frame_x    = frame_x;
    l->frame_y    = frame_y;
    l->item_w     = item_w;
    l->view_x     = view_x;
    l->view_y     = view_y;
    l->view_w     = view_w;
    l->view_h     = view_h;
    l->count      = 0u;
    l->sel        = 0u;
    l->draw_frame = 1u;
    l->draw_item  = fn;
    l->user       = user;
    l->last_sel   = 0xFFu;   /* 0xFF = 还没画过，第一次要整个画一遍 */
    ui_list_register(l);
}

void UiList_Init(ui_list_t *l, int16_t item_w, ui_list_draw_fn fn, void *user)
{
    int16_t view_y = (int16_t)UI_BAT_H;
    int16_t view_h = (int16_t)(UI_SCREEN_H - UI_BAT_H);

    /* 固定黄框：屏幕正中间（文档 (七).b） */
    UiList_InitAt(l,
                  (int16_t)((UI_SCREEN_W - item_w) / 2),
                  (int16_t)UI_FRAME_Y,
                  item_w,
                  (int16_t)((UI_SCREEN_W - item_w) / 2), view_y, item_w, view_h,
                  fn, user);
}

void UiList_SetCount(ui_list_t *l, uint8_t n)
{
    if (l == 0)
    {
        return;
    }
    l->count = n;
    if ((n == 0u) || (l->sel >= n))
    {
        l->sel = 0u;
    }
}

uint8_t UiList_Count(const ui_list_t *l)    { return (l != 0) ? l->count : 0u; }
uint8_t UiList_Selected(const ui_list_t *l) { return (l != 0) ? l->sel : 0u; }

void UiList_SetSel(ui_list_t *l, uint8_t idx)
{
    if ((l == 0) || (l->count == 0u))
    {
        return;
    }
    l->sel = (uint8_t)(idx % l->count);
}

void UiList_Move(ui_list_t *l, int8_t dir)
{
    if ((l == 0) || (l->count == 0u) || (dir == 0))
    {
        return;
    }

    if (dir > 0)
    {
        l->sel = (uint8_t)((l->sel + 1u) % l->count);           /* 正转向下 + 循环 */
    }
    else
    {
        l->sel = (uint8_t)((l->sel + l->count - 1u) % l->count);/* 反转向上 + 循环 */
    }
}

void UiList_OnEncoder(ui_list_t *l, int8_t step)
{
    if (step == 0)
    {
        return;
    }

    /* 一次可能转好几格，逐格走（保证循环正确） */
    while (step > 0)
    {
        UiList_Move(l, 1);
        step--;
    }
    while (step < 0)
    {
        UiList_Move(l, -1);
        step++;
    }
}

void UiList_Invalidate(ui_list_t *l)
{
    if (l == 0)
    {
        return;
    }
    l->last_sel = 0xFFu;            /* 0xFF = 还没画过，下次整个重画 */
}

int16_t UiList_ItemY(const ui_list_t *l, uint8_t idx)
{
    int16_t d;

    if (l == 0)
    {
        return 0;
    }

    /* 有符号差：让"选中项落在框里"这件事跟绕圈无关 */
    d = (int16_t)idx - (int16_t)l->sel;

    return (int16_t)(l->frame_y + d * (int16_t)UI_ITEM_H);
}

void UiList_DrawFrame(const ui_list_t *l)
{
    if ((l == 0) || (l->draw_frame == 0u))
    {
        return;
    }

    /* 23px 高、2px 线宽的黄色圆角框 */
    TFT_DrawRoundRect(l->frame_x, l->frame_y, l->item_w, (int16_t)UI_ITEM_H,
                      (int16_t)UI_FRAME_W, (uint16_t)UI_COL_FRAME);
}

void UiList_Draw(const ui_list_t *l)
{
    uint8_t i;

    if ((l == 0) || (l->draw_item == 0))
    {
        return;
    }

    /*------------------------------------------------------------------------
     * 每次都把可视区里的**每一行**重画一遍。
     *
     * 曾经试过"只重画选中状态变的那两行"的差分刷新，
     * 但那明显是错的：页面刷新前会调 page_bg() 把整屏清成背景色，
     * 不重画的行就变成一片黑——实际表现就是"只显示中间两排"。
     * 差分刷新只在"外部不擦屏"的前提下才成立，这里不成立。
     *
     * 方框不在这里画：由页面先画框、再画行，否则行的底色会把框擦掉。
     *----------------------------------------------------------------------*/
    for (i = 0u; i < l->count; i++)
    {
        int16_t y = UiList_ItemY(l, i);

        /* 裁掉可视区外的 */
        if ((int16_t)(y + (int16_t)UI_ITEM_H) <= l->view_y)
        {
            continue;
        }
        if (y >= (int16_t)(l->view_y + l->view_h))
        {
            continue;
        }

        l->draw_item(l->frame_x, y, l->item_w, (int16_t)UI_ITEM_H,
                     i, (uint8_t)((i == l->sel) ? 1u : 0u), l->user);
    }

    ((ui_list_t *)l)->last_sel = l->sel;
}

/*==========================================================================
 * GTXT 文字版便捷画法
 *========================================================================*/
static void ui_list_text_draw(int16_t x, int16_t y, int16_t w, int16_t h,
                              uint8_t idx, uint8_t selected, void *user)
{
    const uint8_t *ids = (const uint8_t *)user;
    int16_t        tw;

    (void)h;

    if (ids == 0)
    {
        return;
    }

    tw = Game_TextWidth((GameTextId_t)ids[idx]);

    /* 文字 16px，垂直居中在 23px 的槽里 */
    Game_DrawText((int16_t)(x + (w - tw) / 2), (int16_t)(y + 3),
                  (GameTextId_t)ids[idx],
                  (selected != 0u) ? (uint16_t)UI_COL_FRAME : (uint16_t)UI_COL_TEXT,
                  (uint16_t)UI_COL_BG);
}

void UiList_InitText(ui_list_t *l, int16_t item_w, const uint8_t *text_ids, uint8_t n)
{
    UiList_Init(l, item_w, ui_list_text_draw, (void *)text_ids);
    UiList_SetCount(l, n);
}

void UiList_SetTexts(ui_list_t *l, const uint8_t *text_ids, uint8_t n)
{
    if (l == 0)
    {
        return;
    }
    l->user = (void *)text_ids;
    UiList_SetCount(l, n);
}
