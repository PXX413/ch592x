/**************************************************************************
 * ui_icon.h —— 矢量图标库（纯代码绘制，零位图资源）
 *
 * 为什么不用位图：
 *   1. 省 Flash —— 每个位图 16x16 = 32B，十几个就 500B+；代码绘制几乎不占
 *   2. 可缩放 —— 磁贴放大/缩小动画时直接改 size 参数，无需多套位图
 *   3. 可变色 —— 直接传颜色，配合主题切换
 *
 * 所有图标在 (x,y) 为左上角、(s) 为边长的正方形区域内绘制。
 *========================================================================**/

#ifndef __UI_ICON_H__
#define __UI_ICON_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ICON_KEYBOARD = 0,  /* 键盘模式：键盘轮廓 + 键点 */
    ICON_GAME,          /* 游戏模式：手柄 */
    ICON_REMOTE,        /* 遥控模式：信号塔/天线 */
    ICON_SETTINGS,      /* 设置：齿轮 */
    ICON_BATTERY,       /* 电量 */
    ICON_USB,           /* USB 插头 */
    ICON_BLUETOOTH,     /* 蓝牙符号 */
    ICON_RF24,          /* 2.4G 信号波 */
    ICON_CHECK,         /* 对勾 */
    ICON_CROSS,         /* 叉 */
    ICON_ARROW_R,       /* 右箭头（列表用） */
    ICON_BRIGHT,        /* 亮度：太阳 */
    ICON_SLEEP,         /* 休眠：月亮 */
    ICON_REFRESH,       /* 重置：循环箭头 */
    ICON_LED,           /* 键盘灯 */
    ICON_2048,          /* 数字方块 */
    ICON_RUNNER,        /* 跑酷：小人 */
    ICON_THUNDER,       /* 战机 */
    ICON_COUNT
} icon_t;

/*********************************************************************
 * @fn      Icon_Draw
 * @brief   在 (x,y) 起、边长 s 的方框内绘制图标
 * @param   ic   图标 ID
 * @param   col  颜色
 * @param   s    边长（建议 16 / 20 / 26）
 * @note    x,y 是左上角。图标按 s 等比缩放。
 *********************************************************************/
void Icon_Draw(int16_t x, int16_t y, icon_t ic, uint16_t col, int16_t s);

/*********************************************************************
 * @fn      Icon_DrawIcon
 * @brief   同上，但允许指定背景色（会先把方框刷成 bg）
 *********************************************************************/
void Icon_DrawOn(int16_t x, int16_t y, icon_t ic, uint16_t col, uint16_t bg, int16_t s);

#ifdef __cplusplus
}
#endif

#endif /* __UI_ICON_H__ */
