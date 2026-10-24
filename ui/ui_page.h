/**************************************************************************
 * ui_page.h —— 页面栈 + 路由 + 页面基类
 *
 * 整机是一个"页面机"：主页面在最底下，选中"键盘模式"就 push 键盘页，
 * 双击回上一级，长按 >3s 直接回主页面（文档 (六)）。
 *
 * 每个页面就是一个 const ui_page_t + 五个可选回调：
 *     enter / leave / on_key / update / draw
 *
 * 按键（含已分类的编码器事件）统一走 on_key()，返回值告诉框架怎么走：
 *     UI_NAV_STAY  留在本页
 *     UI_NAV_POP   回上一级（等价于双击）
 *     UI_NAV_HOME  回主页面（等价于长按 >3s）
 **************************************************************************/

#ifndef __UI_PAGE_H__
#define __UI_PAGE_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_port.h"
#include "ui_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 版面常量（横屏 240x135）
 *========================================================================*/
#define UI_SCREEN_W         240
#define UI_SCREEN_H         135
#define UI_BAT_H            20      /* 顶部电量条高度（文档 (一).2） */
#define UI_ITEM_H           23      /* 列表项高度（文档 (七)） */
#define UI_FRAME_W          2       /* 选中框线宽（文档 (七).b） */

/* 列表固定框位置：屏幕正中间 */
#define UI_FRAME_Y          ((UI_SCREEN_H - UI_ITEM_H) / 2)

/* 页面栈深度：主页面 -> 键盘页 -> 键盘设置页 -> 键值编辑页 = 4 */
#define UI_PAGE_DEPTH       6

/* 配色 */
#define UI_COL_BG           0xFFFFu
#define UI_COL_TEXT         0x2124u
#define UI_COL_DIM          0x6B4Du
#define UI_COL_FRAME        0x8AC0u     /* 黄色（文档要求） */
#define UI_COL_LINE         0x94B2u     /* 1px 白线 */
#define UI_COL_LINE_YEL     0x8AC0u     /* 1px 黄线 */
#define UI_COL_GREEN        0x1407u
#define UI_COL_SEL_BG       0xE71Cu

/*==========================================================================
 * 2. 导航返回值
 *========================================================================*/
typedef enum
{
    UI_NAV_STAY     = 0,
    UI_NAV_POP      = 1,
    UI_NAV_HOME     = 2,
    /* 页面已经把这次按键"消化"掉了，请框架不要再执行全局规则。
     * 目前只有游戏页用：游戏里双击是"退出游戏回选游戏列表"（需求 (三)），
     * 而不是框架默认的"双击回上一级"（那会把整个游戏页弹回主页面）。 */
    UI_NAV_CONSUMED = 3
} ui_nav_t;

/*==========================================================================
 * 3. 页面基类
 *========================================================================*/
typedef struct ui_page_s
{
    const char *name;

    void      (*enter)(void *arg);              /* arg 是 push 时给的参数 */
    void      (*leave)(void);
    ui_nav_t  (*on_key)(const GameKeyEvent_t *ev);
    void      (*update)(uint16_t dt_ms);
    void      (*draw)(void);
} ui_page_t;

/*==========================================================================
 * 4. 框架
 *========================================================================*/

/*********************************************************************
 * @fn      UI_Init
 * @brief   建立一个只有主页面的页面栈
 *********************************************************************/
void UI_Init(void);

/*********************************************************************
 * @fn      UI_Task
 * @brief   主循环里反复调（每 1ms 一次，紧跟 Game_Task()）
 *          * 把编码器按键喂给 UiEnc_Step 做点击分类
 *          * 排空 Game_PollEvent()，逐条路由给当前页面
 *          * 每 UI_FRAME_MS 跑一次 update + draw
 * @note    编码器"旋转"事件（GKEY_ENC_CW/CCW）也走 on_key 给页面
 * @return  1 = 本次真的执行了 page->draw()（画完了一帧）
 *          0 = 被 20ms 帧率门挡住 / 页面为空 / 没有脏标记，未画屏
 *          开机时主程序靠这个返回值判断"第一帧已经刷好"，再开背光，
 *          避免 ST7789 上电 GRAM 里的随机内容被用户看到。
 *********************************************************************/
uint8_t UI_Task(void);

/* ---- 栈操作（页面自己也可以调，实现跳转） ---- */
void UI_Push(const ui_page_t *p, void *arg);
void UI_Pop(void);
void UI_Replace(const ui_page_t *p, void *arg);
void UI_GoHome(void);

/* 请求重画当前页面（切页、页面内数据变了都调它） */
void UI_Dirty(void);

/* 请求**整页重画**：除置脏外还把页面代号 +1，逼 carousel / cardlist
 * 这类自持 dirty 标记的控件走"整页重画"分支。
 * 电量条这种画在控件之外的数据变了就得用它（见 ui_widget.c 的说明）。 */
void UI_DirtyFull(void);

const ui_page_t *UI_Current(void);
uint8_t          UI_Depth(void);

/*********************************************************************
 * @fn      UI_Epoch
 * @brief   页面切换代号：每次 push / pop / replace / 回主页 都会 +1。
 *
 *   给"只重画变化区域"的控件用。这类控件自己记着 dirty 标记，
 *   但如果它的画面被**别的页面**盖掉了，那些标记就不准了
 *   （标记是 0，可屏幕上已经不是它画的东西）。
 *   控件只要存一份上次绘制时的代号，发现代号变了就整页重画。
 *********************************************************************/
uint32_t         UI_Epoch(void);

/*********************************************************************
 * @fn      UI_PostKey
 * @brief   往当前页面投一个按键（不经过事件队列）
 * @note    页面之间转发、以及 PC 测试直接驱动页面时用
 *********************************************************************/
ui_nav_t UI_PostKey(uint8_t key, uint8_t type);

/*==========================================================================
 * 5. 各页面的单例（定义在各自的 .c 里，这里只声明）
 *========================================================================*/
extern const ui_page_t UI_PAGE_HOME;    /* 主页面（动效磁贴版，栈底） */
extern const ui_page_t UI_PAGE_KBD;     /* 键盘页 */
extern const ui_page_t UI_PAGE_KBSET;   /* 键盘设置页 */
extern const ui_page_t UI_PAGE_KEYEDIT; /* 键值编辑页 */
extern const ui_page_t UI_PAGE_GAME;    /* 游戏模式入口页 */
extern const ui_page_t UI_PAGE_RC;      /* 遥控模式页 */
extern const ui_page_t UI_PAGE_SETTING; /* 设置页 */
extern const ui_page_t UI_PAGE_LEDFX;   /* 灯效选择页 */
extern const ui_page_t UI_PAGE_BRIGHT;  /* 亮度调节页 */
extern const ui_page_t UI_PAGE_SLEEP;   /* 休眠时间页 */
extern const ui_page_t UI_PAGE_SCRBRIGHT; /* 屏幕亮度页 */
extern const ui_page_t UI_PAGE_RESET;   /* 恢复默认确认页 */

#ifdef __cplusplus
}
#endif

#endif /* __UI_PAGE_H__ */
