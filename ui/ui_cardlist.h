/**************************************************************************
 * ui_cardlist.h —— 卡片式列表（子页面统一控件）
 *
 * 取代原来"2px 黄框 + 23px 行"的列表观感，改成：
 *   * 每一行是一张圆角卡片（带图标 + 文字）
 *   * 选中卡片：绿色描边 + 绿色高光 + 轻微放大 + 右侧箭头
 *   * 滚动时卡片整体平移，选中项永远落在屏幕中央的"焦点槽"里
 *   * 卡片间有 3px 缝隙，营造分离感（不是一坨糊在一起）
 *
 * 与 ui_list.c 的区别：
 *   ui_list 是"文档硬性要求"的黄框版（键值编辑页等仍可能要用）；
 *   本控件是卡片版，视觉跟主页面磁贴统一。
 *
 * 内存：ui_cardlist_t 里不含数据，条目数据由调用方通过回调提供，
 *       所以这个结构体本身就是全部开销（约 30 字节），零动态分配。
 **************************************************************************/

#ifndef __UI_CARDLIST_H__
#define __UI_CARDLIST_H__

#include <stdint.h>
#include <stdbool.h>
#include "ui_icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 尺寸（卡片版列表）
 *========================================================================*/
#define CLIST_ITEM_H        26      /* 卡片高：23px 行 + 3px 缝隙       */
#define CLIST_CARD_H        23      /* 卡片本体高（缝隙 3px 在下面）    */
#define CLIST_GAP_Y         3       /* 卡片纵向缝隙                     */
#define CLIST_RADIUS        5       /* 卡片圆角                         */
#define CLIST_ICON_W        14      /* 卡片内图标边长                   */
#define CLIST_FOCUS_MARGIN  3       /* 焦点槽比卡片向外扩多少（描边空间）*/

/*--------------------------------------------------------------------------
 * 可视窗口：**只显示 4 项** —— 选中项上方 1 个、下方 2 个。
 * 选中项固定在焦点槽（CLIST_FOCUS_Y），所以可见的就是
 * d = idx - sel 属于 [-1, +2] 这四行。
 *------------------------------------------------------------------------*/
#define CLIST_VIS_ABOVE     1u
#define CLIST_VIS_BELOW     2u
#define CLIST_VIS_COUNT     (CLIST_VIS_ABOVE + CLIST_VIS_BELOW + 1u)

/* 焦点槽：屏幕垂直居中放一张卡的槽位（y 由屏幕高算出） */
#define CLIST_FOCUS_Y       (((135 - CLIST_CARD_H) / 2) - 1)

/*********************************************************************
 * @fn      ui_clist_item_fn
 * @brief   由调用方提供：画第 idx 项的"内容"（在卡片内部区域绘制）
 * @param   cx,cy   卡片内容区左上角（已避开图标区）
 * @param   cw,ch   内容区宽高
 * @param   idx     条目下标
 * @param   selected 1 = 当前选中
 * @param   user    用户数据（一般是文字表 / 名字表）
 * @note    卡片底板、描边、图标都由控件负责；本回调只管文字/数值。
 *********************************************************************/
typedef void (*ui_clist_item_fn)(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                                 uint8_t idx, uint8_t selected, void *user);

typedef struct
{
    int16_t          x;             /* 卡片左边界（一般是 4）          */
    int16_t          w;             /* 卡片宽（一般是 232 = 240-8）    */
    int16_t          view_y;        /* 可视区上边界                   */
    int16_t          view_h;        /* 可视区高                       */
    uint8_t          count;         /* 一共几项                       */
    uint8_t          sel;           /* 当前选中                       */
    icon_t           icon;          /* 每张卡左侧的统一图标           */
    ui_clist_item_fn draw_item;     /* 内容回调                       */
    void            *user;

    /* 下面两个是"局部刷新"用的内部状态，调用方不用管 */
    uint8_t          bg_pending;    /* 1 = 下次绘制要连页面背景一起重画 */
    uint32_t         epoch;         /* 上次绘制时的页面代号（见 ui_page.h） */
} ui_cardlist_t;

/*********************************************************************
 * @fn      UiCList_Init
 * @brief   初始化（全屏宽度，可视区自动扣除顶部状态栏）
 * @param   l       列表对象
 * @param   icon    每张卡左侧图标（不需要就传 ICON_ARROW_R，会画淡）
 * @param   fn      内容回调
 * @param   user    回调用户数据
 *********************************************************************/
void UiCList_Init(ui_cardlist_t *l, icon_t icon, ui_clist_item_fn fn, void *user);

void    UiCList_SetCount(ui_cardlist_t *l, uint8_t n);
uint8_t UiCList_Count(const ui_cardlist_t *l);
uint8_t UiCList_Selected(const ui_cardlist_t *l);
void    UiCList_SetSel(ui_cardlist_t *l, uint8_t idx);

/* 上下移动（可循环） */
void    UiCList_Move(ui_cardlist_t *l, int8_t dir);

/*********************************************************************
 * @fn      UiCList_ItemY
 * @brief   第 idx 项卡片的 y（不含滚动偏移；选中项 == CLIST_FOCUS_Y）
 *********************************************************************/
int16_t UiCList_ItemY(const ui_cardlist_t *l, uint8_t idx);

/*********************************************************************
 * @fn      UiCList_Draw
 * @brief   画所有可见卡片（选中卡用绿色）
 *********************************************************************/
/*********************************************************************
 * @fn      UiCList_TakeBg
 * @brief   取"这次要不要连页面背景一起重画"，取一次就清掉标志
 * @return  1 = 需要（刚进页面 / 换过数据 / 页面被别的页盖过）
 *          0 = 只是上下换了一项 —— **不要刷全屏**，只重画列表区
 * @note    典型用法：
 *            if (UiCList_TakeBg(&lst)) { page_bg(); page_title(...); }
 *            UiCList_Draw(&lst);
 *********************************************************************/
uint8_t UiCList_TakeBg(ui_cardlist_t *l);

/* 强制下次连背景一起重画（页面里"列表之外的东西"变了时用） */
void    UiCList_ForceBg(ui_cardlist_t *l);

/* 列表可视区在屏幕上的 y 范围与高度 */
int16_t UiCList_RegionY(const ui_cardlist_t *l);
int16_t UiCList_RegionH(const ui_cardlist_t *l);

/* 绘制：先清自己那一块可视区，再画窗口里的 4 项（卡片 + 文字） */
void    UiCList_Draw(const ui_cardlist_t *l);

/*********************************************************************
 * @fn      UiCList_DrawText
 * @brief   **只重画那 4 行文字**（换项时用）
 *
 *   卡片底板/描边/图标/箭头都是按"槽位"画的，换一项时不变，
 *   只有"每个槽位显示哪一项"变了 -> 只有文字要重画。
 *   比 UiCList_Draw() 省很多：不用清 240x107、不用重画 4 张卡片。
 *
 *   用法（配合 UiCList_TakeBg）：
 *     if (UiCList_TakeBg(&lst)) { page_bg(); page_title(...); UiCList_Draw(&lst); }
 *     else                        UiCList_DrawText(&lst);
 *********************************************************************/
void    UiCList_DrawText(const ui_cardlist_t *l);

#ifdef __cplusplus
}
#endif

#endif /* __UI_CARDLIST_H__ */
