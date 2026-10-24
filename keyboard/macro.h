/**************************************************************************
 * macro.h —— 键盘宏（一个键触发一串按键）
 *
 * 【什么是宏】
 *   把"按一串键"录成一步一条，绑定到某一个物理键上：按一下那个键，
 *   键盘自己按顺序把这一串键"敲"出来（每个键还是正常的按下+松开报文）。
 *   例：宏 = Ctrl 按下 → C 按下 → 松开 → V 按下 → 松开 = 一键粘贴。
 *
 * 【本实现的规格（v1）】
 *   宏个数        MACRO_SLOTS = 4        （宏 1 ~ 宏 4）
 *   每个宏最多    MACRO_STEPS = 8 步
 *   每一步        16 位，格式和按键映射完全一样：
 *                    高字节 = HID 修饰位（bit0 LCtrl … bit7 RGUI，见 ui_kbd.c）
 *                    低字节 = HID Usage（0x00~0xE7，0 = 空步）
 *                 所以每一步既能是单键，也能是组合键（Ctrl+C = 0x0106）。
 *   步与步之间    MACRO_STEP_MS = 45ms
 *                 每步本身的"按下→松开"由 ui_kbd.c 的脉冲机制负责（约 20ms），
 *                 所以一个 8 步宏大约 0.4 秒放完。
 *   绑定          每组 9 个键各自可以绑一个宏：
 *                    s_bind[group][key-1] = 0 不绑 / 1..4 = 宏号
 *                 绑定表是**独立**于映射值的，所以不会和"高字节当修饰键"
 *                 那套编码打架。
 *
 * 【Flash】
 *   独立占一整块 256 B（数据 Flash 偏移 0x1600，紧跟在遥控表 bank B 之后），
 *   和 0x0400 的按键映射记录互不影响 —— 升级本功能不会动到用户已存的映射。
 *
 * 【播放时的按键优先级】
 *   宏是用"额外键脉冲"（ui_kbd.c）注入到 HID 报文里的，和物理按键共用同一
 *   个报文通道，所以宏播放过程中用户照常打字；新按下的任何键都会先**停掉**
 *   正在播放的宏（防止宏卡住不放），如果那个键自己也绑了宏，就接着放它的。
 **************************************************************************/

#ifndef __MACRO_H__
#define __MACRO_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MACRO_SLOTS         4u      /* 宏个数 */
#define MACRO_STEPS         8u      /* 每个宏最多几步 */
#define MACRO_STEP_MS       45u     /* 步间隔（毫秒） */

#define MACRO_FLASH_OFF     0x1600u /* 数据 Flash 偏移（独占一块 256B） */
#define MACRO_FLASH_LEN     120u    /* 实际写入长度（4 的倍数，方便 ROM 写） */
#define MACRO_MAGIC         0x414Du /* 'M' | 'A'<<8 */
#define MACRO_VER           0x0001u

/* 初始化：从 Flash 载入（没有记录就保持"全空"） */
void     Macro_Init(void);

/* 每 1ms 调一次：扫按键按下沿触发宏 + 推进正在播放的宏 */
void     Macro_Task(void);

/* 播放控制 */
bool     Macro_IsRunning(void);
void     Macro_Stop(void);

/* 当前组里"绑了宏、而且此刻正按着"的键位图（bit0~bit8 = K1~K9）。
 * ui_kbd.c 用它在发 HID 报文时把这些键的原映射屏蔽掉 —— 绑了宏的键
 * 就只放宏，不再顺手把它自己那个键值也发出去。 */
uint16_t Macro_BoundMask(void);
void     Macro_Trigger(uint8_t idx);    /* idx: 1..MACRO_SLOTS */

/* 绑定表（key: 1..9） */
uint8_t  Macro_GetBind(uint8_t group, uint8_t key);
void     Macro_SetBind(uint8_t group, uint8_t key, uint8_t idx);
void     Macro_GetBindRow(uint8_t group, uint8_t *out9);
void     Macro_SetBindRow(uint8_t group, const uint8_t *in9);

/* 宏内容 */
uint8_t  Macro_GetLen(uint8_t idx);                     /* idx: 1..MACRO_SLOTS */
uint16_t Macro_GetStep(uint8_t idx, uint8_t n);         /* n: 0..MACRO_STEPS-1 */
void     Macro_SetLen(uint8_t idx, uint8_t len);
void     Macro_SetStep(uint8_t idx, uint8_t n, uint16_t value);

/* 存盘：返回 0 成功，非 0 = 失败码（和 KmapStore_Save 一个风格） */
uint8_t  Macro_Save(void);

#ifdef __cplusplus
}
#endif

#endif /* __MACRO_H__ */
