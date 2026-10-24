/**************************************************************************
 * ui_card.h —— 卡片式控件（磁贴卡片 / 圆角面板 / 进度条）
 *
 * 磁贴卡片 = 圆角底 + 图标 + 标题 + 选中态（放大 + 高亮边框 + 呼吸）
 *
 * 动效状态由调用方持有（card_anim_t），每帧推进后调 UiCard_DrawTile，
 * 卡片会根据当前缩放值自动调整几何。
 **************************************************************************/

#ifndef __UI_CARD_H__
#define __UI_CARD_H__

#include <stdint.h>
#include <stdbool.h>
#include "ui_anim.h"
#include "ui_icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 磁贴卡片
 *========================================================================*/
typedef struct
{
    int16_t   x, y;         /* 基准位置（左上角，未缩放时） */
    int16_t   w, h;         /* 基准尺寸 */
    icon_t    icon;         /* 图标 */
    int16_t   title_id;     /* GTXT_xxx 标题（0 = 不画标题） */
    int16_t   scale;        /* 当前缩放：256 = 100% */
} card_t;

/*********************************************************************
 * @fn      UiCard_Init
 * @brief   初始化一个卡片
 *********************************************************************/
void UiCard_Init(card_t *c, int16_t x, int16_t y, int16_t w, int16_t h,
                 icon_t ic, int16_t title_id);

/*********************************************************************
 * @fn      UiCard_Draw
 * @brief   绘制卡片（按 c->scale 自动缩放几何）
 * @param   sel        1 = 选中态
 * @param   glow       0..256 呼吸亮度（选中时的高光强度）
 * @param   icon_size  图标边长
 *********************************************************************/
void UiCard_Draw(const card_t *c, uint8_t sel, int16_t glow, int16_t icon_size);

/*==========================================================================
 * 通用绘制原语
 *========================================================================*/

/* 圆角面板（带 1px 描边 + 可选顶边高光，做出"立体感"） */
void UiCard_Panel(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                  uint16_t fill, uint16_t border, uint16_t hi_line);

/* 水平渐变条（左色 -> 右色，按列插值），给状态栏/进度条用 */
void UiCard_GradBar(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t left, uint16_t right);

/* 分段式进度条：n 段，前 k 段点亮 */
void UiCard_SegBar(int16_t x, int16_t y, int16_t w, int16_t h,
                   uint8_t segs, uint8_t lit, uint16_t on, uint16_t off);

/* 选中框：带呼吸动画的圆角高亮框（列表页共用） */
void UiCard_FocusFrame(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t col, int16_t glow);

/* 阴影模拟：在矩形下方画一条暗色渐层（3 行，递减亮度） */
void UiCard_SoftShadow(int16_t x, int16_t y, int16_t w, int16_t h, int16_t depth);

#ifdef __cplusplus
}
#endif

#endif /* __UI_CARD_H__ */
