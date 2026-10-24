/**************************************************************************
 * ui_cardlist.c —— 卡片式列表实现
 *
 * 视觉规格（240x135 横屏）：
 *   卡片宽 232（左右各留 4px 边距），高 23，圆角 5，缝隙 3
 *   未选中：暗卡底 C_BG_CARD + 1px 暗描边
 *   选中  ：绿色描边 2px + 绿色左侧竖条 + 稍亮卡底 + 右侧箭头
 *
 * 滚动：选中项固定在 CLIST_FOCUS_Y，列表整体平移（跟 ui_list 同一套思路），
 *       只不过槽位高从 23 变成 26（多了缝隙）。
 **************************************************************************/

#include "ui_cardlist.h"
#include "ui_page.h"      /* UI_Epoch() */
#include "ui_theme.h"
#include "ui_card.h"
#include "tft_gfx.h"
#include "game_text.h"

/*==========================================================================
 * 生命周期
 *========================================================================*/
void UiCList_Init(ui_cardlist_t *l, icon_t icon, ui_clist_item_fn fn, void *user)
{
    if (l == 0) { return; }

    l->x         = 4;
    l->w         = (int16_t)(SCR_W - 8);            /* 232 */
    l->view_y    = (int16_t)SBAR_H;
    l->view_h    = (int16_t)(SCR_H - SBAR_H);
    l->count     = 0u;
    l->sel       = 0u;
    l->icon      = icon;
    l->draw_item = fn;
    l->user       = user;
    l->bg_pending = 1u;
    l->epoch      = UI_Epoch();
}

void UiCList_SetCount(ui_cardlist_t *l, uint8_t n)
{
    if (l == 0) { return; }
    l->count = n;
    if ((n == 0u) || (l->sel >= n)) { l->sel = 0u; }
    l->bg_pending = 1u;             /* 项数变了：内容要整块重画 */
}

uint8_t UiCList_Count(const ui_cardlist_t *l)    { return (l != 0) ? l->count : 0u; }
uint8_t UiCList_Selected(const ui_cardlist_t *l) { return (l != 0) ? l->sel : 0u; }

void UiCList_SetSel(ui_cardlist_t *l, uint8_t idx)
{
    if ((l == 0) || (l->count == 0u)) { return; }
    l->sel = (uint8_t)(idx % l->count);
    l->bg_pending = 1u;             /* 外部直接跳到某一项：当整块重画 */
}

void UiCList_Move(ui_cardlist_t *l, int8_t dir)
{
    if ((l == 0) || (l->count == 0u) || (dir == 0)) { return; }

    if (dir > 0)
    {
        l->sel = (uint8_t)((l->sel + 1u) % l->count);
    }
    else
    {
        l->sel = (uint8_t)((l->sel + l->count - 1u) % l->count);
    }

    /* 注意：这里**故意不置 bg_pending** ——
     * 换一项只需要重画列表区那 4 行，页面背景/标题都不用动。 */
}

/*==========================================================================
 * 位置
 *========================================================================*/
int16_t UiCList_ItemY(const ui_cardlist_t *l, uint8_t idx)
{
    int16_t d;

    if (l == 0) { return 0; }

    /* 有符号差：让"选中项落在焦点槽"跟绕圈无关 */
    d = (int16_t)idx - (int16_t)l->sel;

    return (int16_t)((int16_t)CLIST_FOCUS_Y + d * (int16_t)(CLIST_CARD_H + CLIST_GAP_Y));
}

/*==========================================================================
 * 可视区 / 局部刷新
 *========================================================================*/

/* 可视区（含焦点描边余量）在屏幕上的 y 范围与高度 */
static void clist_region(const ui_cardlist_t *l, int16_t *y0, int16_t *h)
{
    int16_t a;
    int16_t b;
    int16_t vy0;
    int16_t vy1;

    a = (int16_t)((int16_t)CLIST_FOCUS_Y -
                  (int16_t)(CLIST_VIS_ABOVE * (int16_t)CLIST_ITEM_H) - CLIST_FOCUS_MARGIN);
    b = (int16_t)((int16_t)CLIST_FOCUS_Y +
                  (int16_t)((CLIST_VIS_BELOW + 1u) * (int16_t)CLIST_ITEM_H) -
                  CLIST_GAP_Y + CLIST_FOCUS_MARGIN);

    /* 夹到页面给的 view 区里（键值编辑页只占右半边那一条） */
    vy0 = (int16_t)l->view_y;
    vy1 = (int16_t)(l->view_y + l->view_h);
    if (a < vy0) { a = vy0; }
    if (b > vy1) { b = vy1; }
    if (b < a)   { b = a; }

    *y0 = a;
    *h  = (int16_t)(b - a);
}

int16_t UiCList_RegionY(const ui_cardlist_t *l)
{
    int16_t y0, h;
    if (l == 0) { return 0; }
    clist_region(l, &y0, &h);
    return y0;
}

int16_t UiCList_RegionH(const ui_cardlist_t *l)
{
    int16_t y0, h;
    if (l == 0) { return 0; }
    clist_region(l, &y0, &h);
    return h;
}

void UiCList_ForceBg(ui_cardlist_t *l)
{
    if (l != 0) { l->bg_pending = 1u; }
}

uint8_t UiCList_TakeBg(ui_cardlist_t *l)
{
    uint32_t ep;

    if (l == 0) { return 0u; }

    /* 页面被切走过（push/pop/回主页）：屏幕上已经不是这个列表的画面了，
     * 必须整页重画，不能只刷列表区。 */
    ep = UI_Epoch();
    if (l->epoch != ep)
    {
        l->epoch      = ep;
        l->bg_pending = 1u;
    }

    if (l->bg_pending != 0u)
    {
        l->bg_pending = 0u;
        return 1u;
    }
    return 0u;
}

/*==========================================================================
 * 绘制
 *========================================================================*/

/* 焦点槽的绿色高亮描边（选中卡专用） */
static void clist_sel_frame(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
    /* 外层：亮绿 */
    TFT_DrawRoundRect(x, y, w, h, r, C_GREEN);
    /* 内层：稍暗一档，做双层框 */
    if ((w > 6) && (h > 6))
    {
        TFT_DrawRoundRect((int16_t)(x + 1), (int16_t)(y + 1),
                          (int16_t)(w - 2), (int16_t)(h - 2),
                          (int16_t)((r > 1) ? (r - 1) : 0),
                          Theme_Dim(C_GREEN, 55u));
    }
}

/*********************************************************************
 * @fn      clist_render
 * @brief   列表绘制内核
 * @param   text_only  0 = 整块重画（清可视区 + 卡片底板/描边/图标 + 文字）
 *                     1 = **只重画文字**
 *
 *   为什么可以只刷文字：
 *     卡片底板、绿色描边、左侧竖条、顶部高光、图标、右箭头
 *     —— **全都是按"槽位"画的**（槽位 0 永远是选中样式），
 *     换一项时它们的形状/颜色/位置**一点都不变**；
 *     变的只是"每个槽位里显示的是哪一项" -> **只有文字变**。
 *     所以换项时把那 4 行文字重画一遍就够了，
 *     比清 240x107 再画 4 张完整卡片省得多。
 *********************************************************************/
static void clist_render(const ui_cardlist_t *l, uint8_t text_only)
{
    uint8_t  i;
    int16_t  slot;
    uint32_t used = 0u;

    if ((l == 0) || (l->draw_item == 0) || (l->count == 0u))
    {
        return;
    }

    /* ---- 整块重画时：先清自己那一块可视区。
     * 只清这一条，页面其它部分（状态栏/标题）不动 ---- */
    if (text_only == 0u)
    {
        int16_t ry0;
        int16_t rh;
        int16_t rx0;
        int16_t rx1;

        clist_region(l, &ry0, &rh);
        if (rh > 0)
        {
            rx0 = (int16_t)(l->x - CLIST_FOCUS_MARGIN);
            rx1 = (int16_t)(l->x + l->w + CLIST_FOCUS_MARGIN);
            if (rx0 < 0)              { rx0 = 0; }
            if (rx1 > (int16_t)SCR_W) { rx1 = (int16_t)SCR_W; }
            if (rx1 > rx0)
            {
                TFT_FillRect(rx0, ry0, (int16_t)(rx1 - rx0), rh, C_BG);
            }
        }
    }

    /* ---- 按"槽位"画。槽位 = 相对选中项的行号：
     *      -1 是上面那行、0 是选中项本身、+1/+2 是下面两行。
     *
     *      列表是**环形**的（UiCList_Move 本来就是取模循环），
     *      所以槽位也要跟着环绕取模：
     *        选中第一项时，上面那行接的是**最后一项**；
     *        选中最后一项时，下面那行接的是**第一项**。
     *      （项数比窗口还少时会出现同一项占多个槽位，跳过画过的。） */
    for (slot = -(int16_t)CLIST_VIS_ABOVE;
         slot <= (int16_t)CLIST_VIS_BELOW; slot++)
    {
        int16_t  y;
        uint8_t  sel;
        uint16_t fill;
        int16_t  cx, cy, cw;

        /* 槽位 -> 项下标（环绕） */
        i = (uint8_t)(((int16_t)l->sel + slot + (int16_t)l->count) %
                      (int16_t)l->count);

        if (l->count <= 32u)
        {
            if ((used & (uint32_t)(1u << i)) != 0u)
            {
                continue;                   /* 这一项已经画过了 */
            }
            used |= (uint32_t)(1u << i);
        }

        y   = (int16_t)((int16_t)CLIST_FOCUS_Y +
                        slot * (int16_t)CLIST_ITEM_H);
        sel = (uint8_t)((slot == 0) ? 1u : 0u);
        fill = (sel != 0u) ? Theme_Blend(C_BG_CARD_HI, C_GREEN, 40)
                           : C_BG_CARD;

        /* 内容区（图标右侧到右箭头左侧） */
        cx = (int16_t)(l->x + 7 + CLIST_ICON_W + 5);            /* 图标 + 间距 */
        cw = (int16_t)(l->w - (cx - l->x) - 14);                /* 右边留给箭头 */
        cy = (int16_t)(y + (CLIST_CARD_H - 16) / 2);            /* 16px 字垂直居中 */

        if (text_only == 0u)
        {
            /* ---- 卡片底板 ---- */
            if (sel != 0u)
            {
                /* 选中：稍亮卡底 + 柔和阴影，做出浮起感 */
                UiCard_SoftShadow(l->x, y, l->w, (int16_t)CLIST_CARD_H, 2);

                TFT_FillRoundRect(l->x, y, l->w, (int16_t)CLIST_CARD_H,
                                  (int16_t)CLIST_RADIUS, fill);

                /* 绿色描边（双层） */
                clist_sel_frame(l->x, y, l->w, (int16_t)CLIST_CARD_H,
                                (int16_t)CLIST_RADIUS);

                /* 左侧绿色竖条：强化"选中"信号 */
                TFT_FillRect((int16_t)(l->x + 2), (int16_t)(y + 3), 2,
                             (int16_t)(CLIST_CARD_H - 6), C_GREEN);

                /* 顶部高光 */
                TFT_FillRect((int16_t)(l->x + CLIST_RADIUS), (int16_t)(y + 1),
                             (int16_t)(l->w - 2 * CLIST_RADIUS), 1,
                             Theme_Light(fill, 45));
            }
            else
            {
                TFT_FillRoundRect(l->x, y, l->w, (int16_t)CLIST_CARD_H,
                                  (int16_t)CLIST_RADIUS, fill);
                TFT_DrawRoundRect(l->x, y, l->w, (int16_t)CLIST_CARD_H,
                                  (int16_t)CLIST_RADIUS, C_BG_PANEL);
                TFT_FillRect((int16_t)(l->x + CLIST_RADIUS), (int16_t)(y + 1),
                             (int16_t)(l->w - 2 * CLIST_RADIUS), 1,
                             Theme_Light(fill, 22));
            }

            /* ---- 左侧图标（选中绿、未选中灰）----
             * 注意：每张卡用的是**同一个** l->icon，所以图标也不会变 */
            {
                int16_t iy = (int16_t)(y + (CLIST_CARD_H - CLIST_ICON_W) / 2);

                Icon_Draw((int16_t)(l->x + 7), iy, l->icon,
                          (sel != 0u) ? C_GREEN : C_TEXT_DIM,
                          (int16_t)CLIST_ICON_W);
            }
        }
        else
        {
            /* ---- 只刷文字：把这一行的文字区擦成"本槽位的卡片底色" ----
             * 卡片被换成长度不同的名字时，短名字盖不住旧字，
             * 所以要先整块擦一遍再画。 */
            TFT_FillRect(cx, cy, cw, 16, fill);
        }

        /* ---- 内容（文字）---- */
        l->draw_item(cx, cy, cw, (int16_t)CLIST_CARD_H, i, sel, l->user);

        /* ---- 右箭头（仅选中态）：按槽位来的，只刷文字时不用再画 ---- */
        if ((sel != 0u) && (text_only == 0u))
        {
            Icon_Draw((int16_t)(l->x + l->w - 13),
                      (int16_t)(y + (CLIST_CARD_H - 10) / 2),
                      ICON_ARROW_R, C_GREEN, 10);
        }
    }
}

void UiCList_Draw(const ui_cardlist_t *l)
{
    clist_render(l, 0u);
}

void UiCList_DrawText(const ui_cardlist_t *l)
{
    clist_render(l, 1u);
}
