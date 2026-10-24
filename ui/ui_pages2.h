/**************************************************************************
 * ui_pages2.h —— 后三个页面（键盘设置页 / 键值编辑页 / 遥控页）
 *
 * 单例都定义在 ui_pages2.c 里；ui_page.h 里已有 extern 声明。
 **************************************************************************/

#ifndef __UI_PAGES2_H__
#define __UI_PAGES2_H__

#include "ui_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 键值编辑页的版面（文档 (7).3，按 240px 屏宽修正）
 *   左边 50px：上=当前按键名，下=3x3 井字格
 *   1px 黄线
 *   右边 189px：HID 键值名称列表（文档写 190，这里吃不下那 1px）
 */
#define UIK2_KEY_W      50
#define UIK2_LIST_W     189
#define UIK2_ITEM_H     23

/* 遥控页：设备列表页 / 9 键功能页 用同一个页面，内部两个阶段 */
/* 遥控能列几台从机。每台约 460B（rc_host_dev_t 里嵌了整个 rc_keymap_t，
 * 含设备名 20 + 9 个键名各 20），所以这个数字直接吃 RAM：
 *     6 台 -> 约 2.76KB     3 台 -> 约 1.38KB
 * 协议上限是 RC_HOST_MAX_DEV(6)，调大别超过它。 */
#ifndef UIRC_DEV_MAX
#define UIRC_DEV_MAX    6
#endif


/*==========================================================================
 * 遥控页对外接口（真机里由 Rc_Host* 驱动，PC 测试直接喂）
 *========================================================================*/
void     UiRc_SetDeviceCount(uint8_t n);
void     UiRc_SetDeviceName(uint8_t idx, const char *name);      /* 从机上下线，上限 UIRC_DEV_MAX */

/* 顶部状态提示（0 = 清除）。用于把"内存不足"这类故障显示出来，
 * 参数必须指向**静态存储期的字符串**（字面量即可），本模块只存指针。 */
void     UiRc_SetStatus(const char *msg);
uint8_t  UiRc_DeviceCount(void);
bool     UiRc_InFunction(void);               /* true = 已进入 9 键功能阶段 */
uint8_t  UiRc_SelectedDevice(void);           /* 0xFF = 还在设备列表里 */
uint16_t UiRc_KeyState(void);                 /* 当前按下的 9 键位图 */
uint8_t  UiRc_TakePending(void);              /* 取一次"待发的编码器操作"，取完自动清零 */
                                              /* 返回 key_id：10=左转 11=右转 12=单击；0=没有 */
uint8_t  UiRc_Function(void);                 /* 最后按下的功能下标 0..8 */
void     UiRc_ReleaseAll(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_PAGES2_H__ */
