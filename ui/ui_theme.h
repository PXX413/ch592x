/**************************************************************************
 * ui_theme.h —— UI 主题（配色 / 尺寸 / 动效参数）
 *
 * 屏幕 240x135 横屏，ST7789，RGB565。
 * 所有尺寸集中在头文件，改一处全局生效。
 *
 * 视觉基调：深色底 + 霓虹青/琥珀强调色，卡片式布局。
 **************************************************************************/

#ifndef __UI_THEME_H__
#define __UI_THEME_H__

#include <stdint.h>
#include "tft_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 配色（RGB565）
 *========================================================================*/

/* 【浅色主题】整套配色是白的底 + 深的字。
 * 起因：磁贴图标（icon_mode/icon_game）是白底圆角位图，四角用白色做
 * 透明键，页面必须是白底才能无缝融合；顺带把其余页面也统一成浅色。
 * 注意：改这里会同时影响所有页面，别再单独硬编码颜色。 */

/* --- 背景层次：白底 + 两级浅灰 --- */
#define C_BG          0xFFFFu   /* 纯白：最底层背景        */
#define C_BG_PANEL    0xF7BEu   /* 极浅灰：面板/分区底     */
#define C_BG_CARD     0xEF5Du   /* 浅灰：卡片常规态        */
#define C_BG_CARD_HI  0xE71Cu   /* 稍深一点：卡片高亮态    */

/* --- 强调色（浅底上都要压深一点，否则看不清）--- */
#define C_ACCENT      0x237Bu   /* 主强调：蓝              */
#define C_ACCENT_DIM  0x7D38u   /* 淡蓝：未激活的强调/分隔 */
#define C_AMBER       0x8AC0u   /* 琥珀：次强调/警告(5.9:1)*/
#define C_GREEN       0x1407u   /* 绿：成功/按下(4.9:1)    */
#define C_RED         0xB800u   /* 红：错误/低电(5.9:1)    */
#define C_YELLOW      0x8AC0u   /* 琥珀（白底上纯黄看不见）*/

/* --- 文字 --- */
#define C_TEXT        0x2124u   /* 主文字：深灰            */
#define C_TEXT_DIM    0x6B4Du   /* 次要文字：中灰(白底上 4.6:1) */
#define C_TEXT_FAINT  0x94B2u   /* 极淡：浅灰（禁用态，3.1:1）*/

/* --- 兼容旧文档硬约束 --- */
#define C_FRAME       0x8AC0u   /* 2px 框（原来是亮黄，改琥珀）*/

/*==========================================================================
 * 2. 布局尺寸
 *========================================================================*/
#define SCR_W           240
#define SCR_H           135

/* 顶部状态栏 */
#define SBAR_H          18      /* 状态栏高度 */
#define SBAR_PAD        4

/* 磁贴卡片（主页面） */
#define TILE_W          72      /* 卡片宽 */
#define TILE_H          62      /* 卡片高 */
#define TILE_GAP_X      6       /* 卡片横向间距 */
#define TILE_GAP_Y      6       /* 卡片纵向间距 */
#define TILE_COLS       3       /* 每行 3 个 */
#define TILE_RADIUS     7       /* 圆角半径 */
#define TILE_ICON_W     26      /* 图标区宽（16x16 图标放大绘制） */
#define TILE_ICON_H     26

/* 列表（子页面，保留文档要求的 23px 行高） */
#define LIST_ITEM_H     23
#define LIST_ITEM_W     200
#define LIST_VISIBLE    4

/* 通用 */
#define PANEL_RADIUS    6
#define PANEL_PAD       4
#define POP_RADIUS      5       /* 弹窗圆角 */

/*==========================================================================
 * 3. 动效参数（毫秒）
 *========================================================================*/
#define ANIM_FAST       140u    /* 快速反馈：按键、高亮      */
#define ANIM_NORMAL     220u    /* 常规：转场、列表滚动      */
#define ANIM_SLOW       340u    /* 慢：开场 logo、弹窗       */
#define ANIM_TILE_STAG  45u     /* 磁贴逐个入场的时间差      */

#define PULSE_PERIOD    1200u   /* 呼吸周期                  */

/* 列表滚动速度：每帧最多移动多少像素（23 行高，分 3 帧到位 ≈ 60ms/行） */
#define LIST_SCROLL_PX  8

/*==========================================================================
 * 4. 小工具
 *========================================================================*/

/* 颜色按亮度插值：a 与 b 之间按 t(0..256) 混合（RGB565 分量近似） */
uint16_t Theme_Blend(uint16_t a, uint16_t b, int16_t t256);

/* 颜色变暗（pct: 0..100，100 = 原色，0 = 全黑） */
uint16_t Theme_Dim(uint16_t c, uint8_t pct);

/* 颜色提亮（往白色靠，pct: 0..100） */
uint16_t Theme_Light(uint16_t c, uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* __UI_THEME_H__ */
