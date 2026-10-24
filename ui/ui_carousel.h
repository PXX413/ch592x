/**************************************************************************
 * ui_carousel.h —— 磁贴轮播控件（主页 / 选游戏页共用）
 *
 * 视觉（横屏 240x135）：
 *      y=0..17     顶部状态栏（标题 + 电量）
 *      y=18..119   磁贴带：选中的圆角正方形磁贴居中，
 *                  左右相邻的只在屏边露出各一半，其余不画
 *      y=120..134  底栏：选中项名字 + 左右箭头
 *
 * 交互：
 *      左右切一格，**循环**（3 -> 0、0 -> 3）
 *
 * 刷新（不整屏刷）：
 *      * 静止时一帧都不重绘
 *      * 切换时只重画"磁贴带"，底栏只在换项时重画一次
 *      * 进页面整页画一次
 *      * 一次切换只画 SWITCH_FRAMES 帧（默认 2 帧，见 .c）
 *
 * 用法：
 *      static const uicar_item_t items[] = { ... };
 *      static uicar_t car;
 *      enter()  { UiCar_Init(&car, items, N, GTXT_xxx); UiCar_Enter(&car); }
 *      key()    { if (左) UiCar_Step(&car, -1); if (右) UiCar_Step(&car, +1);
 *                 if (OK) { UiCar_Tap(&car); ... UiCar_Sel(&car) ... } }
 *      update() { UiCar_Update(&car, dt); }
 *      draw()   { UiCar_Draw(&car); }
 **************************************************************************/

#ifndef __UI_CAROUSEL_H__
#define __UI_CAROUSEL_H__

#include <stdint.h>
#include "ui_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个轮播项 */
typedef struct
{
    const uint16_t *bmp;    /* 60x60 RGB565 位图 = **磁贴本体**（圆角+渐变+白图形都在图里） */
    int16_t  title;         /* GTXT_xxx；GTXT_NONE 时底栏画 num */
    uint16_t color;         /* 该项主题色（底栏顶线 / 状态栏强调线用） */
    uint32_t num;           /* title == GTXT_NONE 时底栏画这个数字（如 2048） */
} uicar_item_t;

#define UICAR_MAX   6u      /* 最多几项 */

typedef struct
{
    const uicar_item_t *items;
    uint8_t  n;             /* 项数 2..UICAR_MAX */
    uint8_t  sel;           /* 当前选中 */
    uint8_t  flash;         /* 按下闪光剩余帧数 */
    uint8_t  entered;       /* 入场动画是否播完 */
    int16_t  sbar_title;    /* 状态栏标题 GTXT_xxx */

    anim_t   cam;           /* 轮播位移 Q8.8：±256 -> 0（256 = 一格） */
    anim_t   entdx;         /* 入场水平偏移 px -> 0 */

    uint8_t  full_dirty;    /* 整页重画 */
    uint8_t  band_dirty;    /* 只重画磁贴带 */
    uint8_t  cap_dirty;     /* 只重画底栏 + 状态栏强调线 */

    /* 上次绘制时的页面切换代号（见 ui_page.h 的 UI_Epoch）。
     * 局部刷新只在"画面还是我画的"前提下成立；一旦被别的页面盖过，
     * 那些 dirty 标记就不准了，必须靠这个代号发现并整页重画。 */
    uint32_t epoch;
} uicar_t;

/* 绑定数据（不画） */
void    UiCar_Init(uicar_t *c, const uicar_item_t *items, uint8_t n,
                   int16_t sbar_title);

/* 进页面：复位状态 + 请求整页重画 */
void    UiCar_Enter(uicar_t *c);

/* 左右切一格（dir: -1 左 / +1 右），带循环 */
void    UiCar_Step(uicar_t *c, int8_t dir);

/* OK 按下反馈（选中磁贴闪一下） */
void    UiCar_Tap(uicar_t *c);

/* 每帧推进动画（静止时内部不会置 dirty，所以不会一直刷屏） */
void    UiCar_Update(uicar_t *c, uint16_t dt_ms);

/* 按 dirty 标记局部刷新 */
void    UiCar_Draw(uicar_t *c);

/* 当前选中项下标 */
uint8_t UiCar_Sel(const uicar_t *c);

#ifdef __cplusplus
}
#endif

#endif /* __UI_CAROUSEL_H__ */
