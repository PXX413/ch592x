/**************************************************************************
 * game_port.h —— CH592X 小游戏公共层（输入 / 计时 / 随机数 / 绘图 / 横竖屏）
 *
 * 目标芯片 : CH592X / CH592F（WCH RISC-V，MounRiver Studio，-std=gnu99）
 * 依赖     : 只依赖已有的 5 个模块，不新增任何外设驱动
 *              key_matrix.h  3x3 矩阵键盘
 *              encoder.h     带按键旋转编码器
 *              tft_port.h / tft_st7789.h / tft_gfx.h   ST7789 屏
 *              game_font16.h / game_text.h             16x16 中文字库（脚本生成）
 *
 * 这一层解决的问题：
 *   1) 把"9 个键盘键 + 编码器旋转/按下"统一成一种按键事件（短按 / 连发 / 松开）
 *   2) 提供 1ms 节拍的毫秒计时（游戏逻辑按毫秒推进）
 *   3) 伪随机数（吃食物、方块、障碍物随机）
 *   4) 常用绘图（中文、放大数字、面板、提示条），并且统一处理横/竖屏
 *
 * 主循环写法（**不阻塞**，时基由 TMR0 1ms 中断提供）：
 *
 *      Game_Init(GAME_PORTRAIT);        // 或 GAME_LANDSCAPE
 *      while (1)
 *      {
 *          Game_Task();                 // 扫描键盘/编码器（不等、不延时）
 *          ... 游戏逻辑、绘图 ...
 *      }
 *
 * 按键表（3x3 键盘按 WASD 手感摆放）：
 *
 *         K1   K2   K3              上 = K5
 *         K4   K5   K6      ==>     左 = K4     右 = K6
 *         K7   K8   K9              下 = K8
 *
 *      所以 GKEY_UP=K5、GKEY_LEFT=K4、GKEY_RIGHT=K6、GKEY_DOWN=K8，
 *      另外 GKEY_OK=K5、GKEY_BACK=K9、GKEY_PAUSE=K7。
 *      编码器：GKEY_ENC_CW / GKEY_ENC_CCW（转一格一个事件）、GKEY_ENC_BTN。
 **************************************************************************/

#ifndef __GAME_PORT_H__
#define __GAME_PORT_H__

#include <stdint.h>
#include <stdbool.h>

#include "CH59x_common.h"
#include "tft_port.h"
#include "tft_st7789.h"
#include "tft_gfx.h"
#include "game_font16.h"
#include "game_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 0. 系统时基
 *
 *   由 User/Main.c 的 TMR0 中断每 1ms 累加一次。
 *   game_port 的"毫秒"必须读它，不能按主循环圈数自己累加 ——
 *   主循环一圈不止 1ms（还要刷屏、收发 RF），按圈数算的话
 *   长按 3 秒实际要十几秒，双击窗口也会跟着缩水。
 *========================================================================*/
extern volatile uint32_t g_ms_tick;

/*==========================================================================
 * 1. 可调参数
 *========================================================================*/
/* 【已废弃】原 Game_Task() 末尾 DelayMs(GAME_SCAN_MS) 的节拍值。
 * 那个忙等已经去掉（见 game_port.c 的说明），现在主循环不再自我限速，
 * 所有"毫秒"一律读 g_ms_tick。这里保留宏只为兼容旧引用，新代码不要再用。 */
#ifndef GAME_SCAN_MS
#define GAME_SCAN_MS            1
#endif
#ifndef GAME_REPEAT_DELAY_MS
#define GAME_REPEAT_DELAY_MS    450     /* 按住多久开始"连续触发" */
#endif
#ifndef GAME_REPEAT_PERIOD_MS
#define GAME_REPEAT_PERIOD_MS   90      /* 连续触发的间隔 */
#endif
#ifndef GAME_EVENT_QUEUE
#define GAME_EVENT_QUEUE        8       /* 事件队列长度（满了丢新事件并计数） */
#endif
#ifndef GAME_FRAME_MS
#define GAME_FRAME_MS           20      /* 游戏逻辑帧长（50Hz），游戏框架会用到 */
#endif
#ifndef GAME_PANEL_W
#define GAME_PANEL_W            135     /* 屏的物理宽（竖屏时用） */
#endif
#ifndef GAME_PANEL_H
#define GAME_PANEL_H            240     /* 屏的物理高（竖屏时用） */
#endif
#ifndef GAME_HINT_BAR_H
#define GAME_HINT_BAR_H         18      /* 底部按键提示条高度 */
#endif

/*==========================================================================
 * 2. 屏幕方向
 *========================================================================*/
typedef enum
{
    GAME_PORTRAIT  = 0,     /* 竖屏：宽 135、高 240（2048 用） */
    GAME_LANDSCAPE = 1      /* 横屏：宽 240、高 135（方块/贪吃蛇/跑酷用） */
} GameOrient_t;

/*==========================================================================
 * 3. 按键
 *========================================================================*/
typedef enum
{
    GKEY_NONE    = 0,

    GKEY_K1      = 1,       /* 矩阵键盘按下标编号，和 KEY_IsPressed(n) 一致 */
    GKEY_K2      = 2,
    GKEY_K3      = 3,
    GKEY_K4      = 4,
    GKEY_K5      = 5,
    GKEY_K6      = 6,
    GKEY_K7      = 7,
    GKEY_K8      = 8,
    GKEY_K9      = 9,

    GKEY_ENC_CW  = 10,      /* 编码器顺时针转一格 */
    GKEY_ENC_CCW = 11,      /* 编码器逆时针转一格 */
    GKEY_ENC_BTN = 12,      /* 编码器按键 */

    /* --- 下面五个是"已经分好类"的编码器事件（文档 (六)）---
     * game_port 自己**不产生**它们（它只知道键按下/松开）；
     * 由 ui\ui_enc.c 那个点击分类器产生，UI 主循环转成 GameKeyEvent_t
     * 再投给游戏。游戏代码只认这五个，不去碰原始 BTN 事件。 */
    GKEY_ENC_CLICK   = 13,  /* 单击：列表页确认 / 非列表页当普通按键 */
    GKEY_ENC_DBL     = 14,  /* 双击：返回上一级 */
    GKEY_ENC_HOLD    = 15,  /* 长按 >=0.5s：执行当前可执行的操作 */
    GKEY_ENC_HOLD1   = 17,  /* 长按 >=1s：键盘页进设置页（需求 (二).3） */
    GKEY_ENC_HOLD3   = 16,  /* 长按 >3s：直接回主页面 */

    GKEY_COUNT   = 18
} GameKey_t;

/* 语义别名：方向键按 WASD 手感（中间一行是左右，中间键是上，下面中间是下） */
#define GKEY_UP         GKEY_K5
#define GKEY_DOWN       GKEY_K8
#define GKEY_LEFT       GKEY_K4
#define GKEY_RIGHT      GKEY_K6
#define GKEY_OK         GKEY_K5
#define GKEY_BACK       GKEY_K9
#define GKEY_PAUSE      GKEY_K7

/*==========================================================================
 * 4. 按键事件
 *========================================================================*/
typedef enum
{
    GAME_EV_PRESS   = 1,    /* 刚按下 */
    GAME_EV_REPEAT  = 2,    /* 按住后的连续触发（长按） */
    GAME_EV_RELEASE = 3     /* 松开 */
} GameEventType_t;

typedef struct
{
    GameKey_t key;          /* 哪个键 */
    uint8_t   type;         /* GameEventType_t */
    uint16_t  held_ms;      /* 事件发生时已经按住了多久（PRESS 时为 0） */
} GameKeyEvent_t;

/*==========================================================================
 * 5. 接口
 *========================================================================*/

/*********************************************************************
 * @fn      Game_Init
 * @brief   初始化键盘 + 编码器 + 屏，并设定初始屏幕方向
 * @param   orient  GAME_PORTRAIT（竖屏 135x240）/ GAME_LANDSCAPE（横屏 240x135）
 * @note    会清屏为黑色；随机数种子取自系统 tick
 *********************************************************************/
void Game_Init(GameOrient_t orient);

/*********************************************************************
 * @fn      Game_Task
 * @brief   放进主循环：扫描键盘/编码器、推进 1ms 节拍、产生按键事件
 * @note    内含 DelayMs(GAME_SCAN_MS)，主循环里不要再额外延时
 *********************************************************************/
void Game_Task(void);

/*********************************************************************
 * @fn      Game_PollEvent
 * @brief   取出一个按键事件
 * @param   ev  输出事件
 * @return  true = 取到一个事件；false = 没有事件了
 *********************************************************************/
bool Game_PollEvent(GameKeyEvent_t *ev);

/* 丢掉队列里所有未处理事件（比如从菜单切进游戏时清一下） */
void Game_FlushEvents(void);

/* 【2026-10-14】切页时把"触发这次切页的键"记下来：在它松开之前，
 * 键层不为它产生任何事件（PRESS/REPEAT/RELEASE 都不发）。
 * 用来根治"按住 KEY5 进新页，新页又收到一次 KEY5"。 */
void Game_SwallowKey(uint8_t key);

/* 某个键现在是否按着（已消抖）；编码器旋转键恒为 false */
bool Game_KeyDown(GameKey_t key);

/* 某个键已经按住多久（毫秒），没按着返回 0 */
uint16_t Game_KeyHeldMs(GameKey_t key);

/* 从 Game_Init 开始的毫秒计数（只随 Game_Task() 前进） */
uint32_t Game_Millis(void);

/* ---- 屏幕方向 ---- */
void         Game_SetOrientation(GameOrient_t o);    /* 切方向 + 更新宽高 */
GameOrient_t Game_GetOrientation(void);
int16_t      Game_Width(void);                       /* 当前方向下的可用宽 */
int16_t      Game_Height(void);                      /* 当前方向下的可用高 */

/* ---- 随机数（xorshift32，够游戏用） ---- */
void     Game_Srand(uint32_t seed);                  /* 0 会被换成一个默认种子 */
uint32_t Game_Rand(void);                            /* 0 ~ 0xFFFFFFFF */
uint32_t Game_RandRange(uint32_t n);                 /* 0 ~ n-1（n=0 时返回 0） */

/* ---- 绘图：中文 ---- */
void    Game_Clear(uint16_t color);                                  /* 整屏清成 color */
void    Game_DrawGlyph(int16_t x, int16_t y, GameGlyph_t g,
                       uint16_t fg, uint16_t bg);                    /* 一个字（16x16） */
void    Game_DrawText(int16_t x, int16_t y, GameTextId_t id,
                      uint16_t fg, uint16_t bg);                     /* 一串中文（GTXT_xxx） */
int16_t Game_TextWidth(GameTextId_t id);                             /* 像素宽 */
void    Game_DrawTextCenter(int16_t y, GameTextId_t id,
                            uint16_t fg, uint16_t bg);               /* 水平居中 */

/* ---- 绘图：数字（用 5x7 字库放大，scale = 1~4） ---- */
int16_t Game_NumberWidth(uint32_t num, uint8_t scale);
void    Game_DrawNumber(int16_t x, int16_t y, uint32_t num, uint8_t scale,
                        uint16_t fg, uint16_t bg);
void    Game_DrawNumberRight(int16_t x_right, int16_t y, uint32_t num, uint8_t scale,
                             uint16_t fg, uint16_t bg);              /* 右对齐（分数用） */

/* ---- 绘图：ASCII 字符串（比如蓝牙设备名） ---- */
int16_t Game_AsciiWidth(const char *s, uint8_t scale);
void    Game_DrawAscii(int16_t x, int16_t y, const char *s, uint8_t scale,
                       uint16_t fg, uint16_t bg);
/* ---- 绘图：面板 / 提示条 ---- */
void Game_DrawPanel(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint16_t fill, uint16_t border);                 /* 面板/格子底 */
void Game_DrawHintBar(GameTextId_t id, uint16_t fg, uint16_t bg);    /* 底部按键提示条 */

/*********************************************************************
 * @fn      Game_Sound
 * @brief   【预留】音效接口，现在是空函数
 * @note    以后接无源蜂鸣器时，只在这个函数里填 PWM 代码，
 *          所有游戏代码一行都不用改
 *********************************************************************/
void Game_Sound(uint16_t freq_hz, uint16_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __GAME_PORT_H__ */
