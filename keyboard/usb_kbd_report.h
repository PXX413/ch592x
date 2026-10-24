/**************************************************************************
 * usb_kbd_report.h —— USB HID 键盘「报文层」（纯逻辑，不碰任何寄存器）
 *
 * 这一层只做一件事：把 3x3 键盘 + 编码器的按键状态，翻译成标准
 * USB HID boot 键盘的 8 字节报文。所以它：
 *   * 不 include 任何 CH59x 头文件
 *   * 可以在 PC 上直接单测
 *
 * 报文格式（USB HID boot keyboard，和 BLE HID 完全一样）：
 *      [0]     修饰键位图（bit0 左Ctrl、bit1 左Shift …… bit7 右GUI）
 *      [1]     保留，固定 0
 *      [2..7]  最多 6 个同时按下的键的 HID 键码（Usage ID），0 = 空
 *
 * ------------------------------------------------------------------
 * 【键位映射和 CH592X_bluetooth\bt_hid.c 是完全同一份】
 *
 *      K1 ~ K9     -> 数字 1 ~ 9        (HID 0x1E ~ 0x26)
 *      编码器正转  -> 音量 +            (HID 0x80)
 *      编码器反转  -> 音量 -            (HID 0x81)
 *      编码器按下  -> 回车              (HID 0x28)
 *
 * bt_hid.c 里那张 s_default_usage[] 和这里的 s_default_usage[] 逐字节相同，
 * tests\test_report.c 有一条断言专门盯着这件事（见 T8）。
 * 为什么不把表抽成公共头让两边 include：bt_hid.c 属于已经交付并自测过的
 * bluetooth 模块，动它会连带影响它的测试；这里先把表**原样**复制过来并
 * 用测试钉死，等哪天腾出手再把 kbd_map 抽成公共头。
 **************************************************************************/

#ifndef __USB_KBD_REPORT_H__
#define __USB_KBD_REPORT_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 报文尺寸
 *========================================================================*/
#define USBKBD_REPORT_LEN       8u      /* boot 键盘报文固定 8 字节 */
#define USBKBD_MAX_KEYS         6u      /* 最多同时上报 6 个非修饰键 */
#define USBKBD_NKRO_BYTES       28u     /* NKRO 位图：224 个键码 = 28 字节 */

/*==========================================================================
 * 2. 按键编号（和 bt_hid 的 BtHidKey_t 一一对应）
 *========================================================================*/
typedef enum
{
    USBKBD_K1 = 0, USBKBD_K2, USBKBD_K3,
    USBKBD_K4,     USBKBD_K5, USBKBD_K6,
    USBKBD_K7,     USBKBD_K8, USBKBD_K9,
    USBKBD_ENC_CW, USBKBD_ENC_CCW, USBKBD_ENC_BTN,
    USBKBD_KEY_COUNT
} UsbKbdKey_t;

/*==========================================================================
 * 3. 常用 HID 键码（Usage ID，USB HID Usage Tables 第 10 节）
 *========================================================================*/
#define USBKBD_KEY_A        0x04u
#define USBKBD_KEY_1        0x1Eu
#define USBKBD_KEY_2        0x1Fu
#define USBKBD_KEY_3        0x20u
#define USBKBD_KEY_4        0x21u
#define USBKBD_KEY_5        0x22u
#define USBKBD_KEY_6        0x23u
#define USBKBD_KEY_7        0x24u
#define USBKBD_KEY_8        0x25u
#define USBKBD_KEY_9        0x26u
#define USBKBD_KEY_ENTER    0x28u
#define USBKBD_KEY_ESC      0x29u
#define USBKBD_KEY_SPACE    0x2Cu
#define USBKBD_RIGHT        0x4Fu
#define USBKBD_LEFT         0x50u
#define USBKBD_DOWN         0x51u
#define USBKBD_UP           0x52u
#define USBKBD_VOL_UP       0x80u
#define USBKBD_VOL_DOWN     0x81u

/* 修饰键位图（报文第 0 字节的位） */
#define USBKBD_MOD_LCTRL    0x01u
#define USBKBD_MOD_LSHIFT   0x02u
#define USBKBD_MOD_LALT     0x04u
#define USBKBD_MOD_LGUI     0x08u
#define USBKBD_MOD_RCTRL    0x10u
#define USBKBD_MOD_RSHIFT   0x20u
#define USBKBD_MOD_RALT     0x40u
#define USBKBD_MOD_RGUI     0x80u

/*==========================================================================
 * 4. LED 输出报文（主机 -> 键盘）
 *
 * 主机告诉我们键盘指示灯该亮哪个。标准定义只有低 3 位有意义：
 *      bit0 = NumLock    bit1 = CapsLock    bit2 = ScrollLock
 *      bit3 = Compose    bit4 = Kana
 *========================================================================*/
#define USBKBD_LED_NUMLOCK      0x01u
#define USBKBD_LED_CAPSLOCK     0x02u
#define USBKBD_LED_SCROLLLOCK   0x04u
#define USBKBD_LED_COMPOSE      0x08u
#define USBKBD_LED_KANA         0x10u

/*==========================================================================
 * 5. 接口
 *========================================================================*/

/* 恢复默认键位表、清空所有按键与状态 */
void UsbKbd_Init(void);

/* ---- 按键事件 ---- */
void UsbKbd_KeyEvent(UsbKbdKey_t key, bool down);
void UsbKbd_ReleaseAll(void);               /* 断开 / 停止前必须调一次 */
bool UsbKbd_IsDown(UsbKbdKey_t key);

/* ---- 键位表 ---- */
uint8_t UsbKbd_GetUsage(UsbKbdKey_t key);
void    UsbKbd_SetUsage(UsbKbdKey_t key, uint8_t usage);   /* usage=0 表示只当修饰键 */
void    UsbKbd_SetModifierBit(UsbKbdKey_t key, uint8_t mod);

/* ---- 生成报文 ----
 * UsbKbd_BuildReport() 每次都按当前按键状态重算，返回的指针指向内部
 * 静态缓冲（8 字节），调用方不要修改也不要长期保存。 */
const uint8_t *UsbKbd_BuildReport(void);

/* 取当前报文但**不重算**（调试/断言用） */
const uint8_t *UsbKbd_PeekReport(void);

/* NKRO 位图（可选）：把当前按下的键写成 28 字节位图，返回非 0 键个数。
 * 注意：USB 描述符默认声明的是 boot（6KRO）报表，
 *       真要在 USB 上用 NKRO 得换一份报表描述符，见 README 第三章。 */
uint8_t UsbKbd_BuildNkroBitmap(uint8_t *out28);

/* ---- 脏标志（有新状态需要上报） ---- */
bool UsbKbd_IsDirty(void);
void UsbKbd_MarkDirty(void);
void UsbKbd_ClearDirty(void);       /* 报文成功送到主机后清掉 */

/* 同时按超过 6 个键被丢掉的次数（调试用） */
uint8_t UsbKbd_OverflowCount(void);

/* ---- LED 状态解析 ---- */
uint8_t UsbKbd_SetLedFromHost(uint8_t led_byte);    /* 返回解析后的 LED 字节 */
uint8_t UsbKbd_GetLed(void);
bool    UsbKbd_LedNumLock(void);
bool    UsbKbd_LedCapsLock(void);
bool    UsbKbd_LedScrollLock(void);

/*==========================================================================
 * 6. 给上层用的「语义化」查询
 *    上层（比如 AUTO 切通道的逻辑）只关心"现在有没有键按着"
 *========================================================================*/
uint8_t UsbKbd_DownCount(void);
bool    UsbKbd_AnyDown(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_KBD_REPORT_H__ */
