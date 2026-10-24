/**************************************************************************
 * ui_kbd.h —— 键盘页（USB / 2.4G / BLE 三通道的唯一驱动者）
 *
 * 依据：需求文档「（二）键盘页面」
 *   1. 自动判断通信方式：USB 0.5s -> 广播 2.4G 1.5s -> 广播 BLE（不限）
 *   2. 版面（横屏 240x135，顶部 20px 电量条以下 115px 高）：
 *        左侧  189px              当前按下的按键
 *        右上  197..246 (50x50)   USB / 2.4G / 蓝牙
 *        右中  197..246 (50x33)   已连接 / 未连接
 *        右下  197..246 (50x50)   3x3 井字格（按下变绿，支持多键）
 *        四个区域用 1px 白线分割
 *   3. 编码器长按 1 秒进键盘设置页
 *   4. 键值发送：USB/BLE 直接发 HID；2.4G 交给接收器转发
 *
 * 本模块负责：驱动 ui_transport 仲裁、把当前键值打成 6KRO HID 报文、
 * 把报文交给当前通道（回调由上层接线）、以及键盘页的绘制。
 **************************************************************************/

#ifndef __UI_KBD_H__
#define __UI_KBD_H__

#include <stdint.h>
#include <stdbool.h>
#include "ui_page.h"
#include "ui_transport.h"

#ifdef __cplusplus
extern "C" {
extern int g_dbg_calls;
extern int g_dbg_force;
#endif

/*==========================================================================
 * 1. 版面数字（需求文档 (二).2.a）
 *========================================================================*/
/* 【修复 P1-6】按需求 (二).2.a 还原右栏为 50px。
 *
 * 右栏里那两样东西是**上下排列**的（右上 50x50 通道状态 + 右中 50x33
 * 连接状态 + 右下 50x50 井字格），不是并排，所以宽度只花 50px：
 *     左栏 188px + 1px 分割线 + 右栏 50px = 239px <= 240px  放得下。
 * 原来注释按"189 + 50 + 50 = 289"算，把右栏压成了 43px（井字格 41px、
 * 每格 13x13），与需求不符。现在井字格 50px = 3*16 + 2*1，每格 16x16。 */
#define UIK_LEFT_W      188     /* 左侧按键显示区宽度（含 1px 分割线共 189，文档 (2).2.a） */
#define UIK_PANEL_X     189     /* 右列起始 x = 188 + 1px 分割线 */
#define UIK_PANEL_W     50      /* 右栏宽（文档：50x50 / 50x33 / 50x50 三个区块） */
#define UIK_GRID_SIZE   50      /* 井字格外围：3*16 + 2*1 */
#define UIK_STAT_H      33      /* 连接状态区高度（文档 50x33） */
#define UIK_REGION_Y    20      /* 区域从电量条下面开始 */
#define UIK_REGION_H    115     /* 135 - 20 */

/*==========================================================================
 * 2. 键值显示（左侧 189px 区）
 *========================================================================*/
#define UIK_TEXT_MAX    32      /* 显示缓冲（含结尾 0） */

/* 一个按键编号（1..9）对应的 HID 用途码（按当前键值映射组）。
 * 取映射值的低字节；单独/组合修饰键见下面两个接口。 */
uint8_t UiKbd_UsageOf(uint8_t key);

/* 取 1..9 键的 16 位映射原值：高字节 = 修饰键位图，低字节 = 主键用途码 */
uint16_t UiKbd_MapValueOf(uint8_t key);

/* 该键要叠加进报文第 0 字节（modifiers）的修饰位：
 * 映射值高字节 + （低字节是 0xE0~0xE7 时）该用途码对应的位 */
uint8_t UiKbd_ModsOf(uint8_t key);

/* HID Usage 0xE0~0xE7 -> 修饰位 0x01~0x80；其它用途码返回 0 */
uint8_t UiKbd_UsageModBit(uint8_t usage);

/* HID 用途码 -> 简短 ASCII 名字（屏幕用，字库没有键名的词条） */
const char *UiKbd_UsageName(uint8_t usage);

/* 把当前按下的键拼成 "1+5" 这样的字符串；没按键返回空串 */
void UiKbd_BuildPressedText(char *buf, uint8_t buf_len);

/*********************************************************************
 * @fn      UiKbd_BuildPressedList
 * @brief   取当前按下的每个键的**键名指针**（不拼接），供分排大字显示
 * @param   out      输出数组（每项指向静态键名字符串，不拷贝）
 * @param   max_n    out 最多放几项（建议 6）
 * @return  实际按下几个键（0 = 没按键）
 * @note    最多返回 max_n 个；超过的忽略（9 键同时按下的极端情况，
 *          屏上也放不下那么多排）。键名来自 UiKbd_UsageName()，只读。
 *********************************************************************/
uint8_t UiKbd_BuildPressedList(const char *out[], uint8_t max_n);

/*==========================================================================
 * 3. 6KRO HID 报文
 *========================================================================*/
#define UIK_HID_REPORT_LEN      8u      /* modifiers + reserved + 6 keys */

/*
 * 把按下的键位图打成标准 6KRO 引导报文（out 至少 8 字节），返回长度
 * 修饰键（左右 Ctrl/Shift/Alt/Win）单独填在 modifiers 字节：
 *   全局 UiKbd_SetModifier() + 各键映射值高字节 + 映射到 0xE0~0xE7 的单键
 */
uint8_t UiKbd_BuildHid6(uint8_t *out8, uint16_t keymap);

/* modifiers 字节 <- 四个修饰位（真机上由 Main.c 在模块里填，PC 上直接测） */
void    UiKbd_SetModifier(uint8_t mods);
uint8_t UiKbd_Modifier(void);

/*==========================================================================
 * 3b. 额外键（编码器按键当"3x3 之外的按键"用，需求 (六).2）
 *
 *   非列表页单击 = 在键盘上敲一下这个键。默认回车 0x28，
 *   和 keyboard 模块的默认表（编码器按下 = 回车）保持一致。
 *   BLE / 2.4G 走 6KRO 报文，USB 走 usb_kbd_report 的 ENC_BTN 槽位。
 *========================================================================*/
#define UIK_EXTRA_USAGE_ENTER   0x28u   /* 默认用途码：回车 */
#define UIK_EXTRA_PULSE_MS      50u     /* 单击后按键脉冲的宽度（毫秒，够一次轮询周期） */

/* 在当前通道上发一次"按下 + 30ms 后松开"的按键脉冲。没连通道就不发。 */
void UiKbd_PulseExtra(void);

/*********************************************************************
 * @fn      UiKbd_PulseMapping
 * @brief   按 16 位映射值（高字节修饰位 + 低字节 HID Usage）脉冲一次
 *
 * 编码器三个动作（正转/反转/单击）取 KEY_ENC_* 槽里的值送进来：
 *   0x0028 = 回车（单击默认）
 *   0x0106 = Ctrl+C（组合键也能挂到编码器上）
 *   0x00E1 = 单独左 Shift
 * 值为 0 = 该动作不按键。
 *********************************************************************/
void UiKbd_PulseMapping(uint16_t map_value);

/* 换掉这个额外键的用途码；传 0 = 关掉这个功能。 */
void UiKbd_SetExtraUsage(uint8_t usage);

/* 额外键现在是不是按着（USB 那条通道要单独喂状态） */
bool UiKbd_ExtraIsDown(void);

/*==========================================================================
 * 4. 通道（USB / 2.4G / BLE）
 *========================================================================*/
void           UiKbd_Init(void);
void           UiKbd_Task(void);                /* 每 1ms 调一次 */
ui_transport_t UiKbd_Channel(void);             /* 当前用哪条通道 */
bool           UiKbd_Connected(void);           /* 选中的通道连上了没 */
const char    *UiKbd_ChannelText(void);         /* "USB" / "2.4G" / "BLE" / "--" */
const char    *UiKbd_StateText(void);           /* "OK" / "NO"（屏上再查字库） */

/* 底层通道状态喂进来（真机由三个模块调；PC 测试直接调） */
void UiKbd_SetUsb(bool plugged, bool ready);
void UiKbd_SetRf24(bool alive);
void UiKbd_SetBle(bool ready);

/* 报文出口：返回 true 表示交出去了 */
void UiKbd_SetSender(bool (*fn)(ui_transport_t tr, const uint8_t *rep, uint8_t len));

/* 广播动作出口（开始/停止广播 2.4G / BLE） */
void UiKbd_SetBroadcaster(void (*fn)(ui_transport_t tr, bool starting));

/* 立即把当前键状态推给通道；返回 true = 发出去了 */
bool UiKbd_Flush(void);

/* 只有键状态真变了且通道已连上才发报文；返回 true = 真的发了 */
bool UiKbd_OnKeyState(uint16_t map);

/* 只读的调试/测试入口 */
const ui_tr_t *UiKbd_Transport(void);

/*==========================================================================
 * 5. 页面单例（ui_pages.c 里原来那张"开发中"占位已删除）
 *========================================================================*/
extern const ui_page_t UI_PAGE_KBD;

#ifdef __cplusplus
}
#endif

#endif /* __UI_KBD_H__ */
