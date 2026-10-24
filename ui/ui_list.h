/**************************************************************************
 * ui_list.h —— 列表控件（文档 (七) 列表页）
 *
 * 文档硬性要求：
 *   * 每项 23px 高，宽度可配（主页面/键盘设置页 240，键值编辑页 190）
 *   * 固定 23px 高、2px 线宽的**黄色圆角框**在屏幕中间不动
 *   * 文字 16px，可循环，滚动时"文字动、框不动"
 *
 * 实现：选中项永远落在固定框里 —— 第 i 项的中心 y = 框中心 + (i - sel)*23。
 * 所以滚动时是整个文字列表平移，框原地不动。
 **************************************************************************/

#ifndef __UI_LIST_H__
#define __UI_LIST_H__

#include <stdint.h>
#include <stdbool.h>
#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 画一项的回调：往 (x,y,w,h) 里画第 idx 项，selected = 是不是当前选中 */
typedef void (*ui_list_draw_fn)(int16_t x, int16_t y, int16_t w, int16_t h,
                                uint8_t idx, uint8_t selected, void *user);

typedef struct
{
    int16_t         frame_x;        /* 固定黄框左上角 */
    int16_t         frame_y;
    int16_t         item_w;         /* 选项宽（240 / 190） */
    int16_t         view_x;         /* 可视区（超出就裁掉不画） */
    int16_t         view_y;
    int16_t         view_w;
    int16_t         view_h;
    uint8_t         count;          /* 一共几项 */
    uint8_t         sel;            /* 当前选中项下标 */
    uint8_t         draw_frame;     /* 要不要画黄框（键值编辑页的左侧格子也复用了同一套） */

    /* 上一次画的是哪一行（用来只重画变化的那两行）。
     * 0xFF = 还没画过，需要整个重画一次。 */
    uint8_t         last_sel;
    ui_list_draw_fn draw_item;
    void           *user;
} ui_list_t;

/*********************************************************************
 * @fn      UiList_Init
 * @brief   初始化（全屏列表：frame/(view) 都按屏幕算）
 * @param   item_w  240 = 全宽列表；190 = 键值编辑页那种窄列表
 *********************************************************************/
void UiList_Init(ui_list_t *l, int16_t item_w, ui_list_draw_fn fn, void *user);

/*********************************************************************
 * @fn      UiList_InitAt
 * @brief   指定位置初始化（给非全屏列表用）
 *********************************************************************/
void UiList_InitAt(ui_list_t *l, int16_t frame_x, int16_t frame_y, int16_t item_w,
                   int16_t view_x, int16_t view_y, int16_t view_w, int16_t view_h,
                   ui_list_draw_fn fn, void *user);

void    UiList_SetCount(ui_list_t *l, uint8_t n);
uint8_t UiList_Count(const ui_list_t *l);
uint8_t UiList_Selected(const ui_list_t *l);
void    UiList_SetSel(ui_list_t *l, uint8_t idx);

/*********************************************************************
 * @fn      UiList_Move
 * @brief   上下移动光标（可循环，文档 (七).1.d）
 * @param   dir  +1 向下 / -1 向上
 *********************************************************************/
void UiList_Move(ui_list_t *l, int8_t dir);

/*********************************************************************
 * @fn      UiList_WrapTo
 * @brief   由编码器旋转驱动的移动（正转向下、反转向上，文档 (六).5）
 *********************************************************************/
void UiList_OnEncoder(ui_list_t *l, int8_t step);

/*********************************************************************
 * @fn      UiList_ItemY
 * @brief   第 idx 项所在的 y（选中项 == frame_y）
 * @note    单独抽出来是为了让 PC 测试直接断言滚动位置对不对
 *********************************************************************/
int16_t UiList_ItemY(const ui_list_t *l, uint8_t idx);

/* 画所有可见项 + 固定黄框 */
void UiList_Draw(const ui_list_t *l);

/*********************************************************************
 * @fn      UiList_InvalidateAll
 * @brief   把所有初始化过的列表都标成"下次重画"
 *********************************************************************/
void UiList_InvalidateAll(void);

/*********************************************************************
 * @fn      UiList_Invalidate
 * @brief   下一次 UiList_Draw() 整个重画（而不是只重画变化的两行）
 * @note    画面被别人擦过、或开始画到空白缓冲里，都调一下。
 *********************************************************************/
void UiList_Invalidate(ui_list_t *l);

/*********************************************************************
 * @fn      UiList_DrawFrameOnly
 * @brief   只画那个固定的黄框（内容由调用方自己画时用）
 *********************************************************************/
void UiList_DrawFrame(const ui_list_t *l);

/*==========================================================================
 * 便捷画法：直接用 GTXT_xxx 当条目文字
 *========================================================================*/
void UiList_InitText(ui_list_t *l, int16_t item_w, const uint8_t *text_ids, uint8_t n);
void UiList_SetTexts(ui_list_t *l, const uint8_t *text_ids, uint8_t n);

#ifdef __cplusplus
}
#endif

#endif /* __UI_LIST_H__ */
