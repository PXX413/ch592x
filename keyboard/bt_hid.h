/**************************************************************************
 * bt_hid.h —— CH592X 标准 BLE HID 键盘（报文层）
 *
 * 只负责"按键状态 -> 8 字节 HID 报文"，真正的发送走 BT_SendHid()，
 * 所以这一层不需要蓝牙协议栈，可以单独自测。
 **************************************************************************/

#ifndef __BT_HID_H__
#define __BT_HID_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 报文
 *========================================================================*/
#define BT_HID_REPORT_LEN   8u      /* boot 键盘报文长度 */
#define BT_HID_MAX_KEYS     6u      /* 最多同时上报 6 个键 */

/*==========================================================================
 * 2. 按键编号（3x3 键盘 + 编码器）
 *========================================================================*/
typedef enum
{
    BT_HID_K1 = 0, BT_HID_K2, BT_HID_K3,
    BT_HID_K4,     BT_HID_K5, BT_HID_K6,
    BT_HID_K7,     BT_HID_K8, BT_HID_K9,
    BT_HID_ENC_CW, BT_HID_ENC_CCW, BT_HID_ENC_BTN,
    BT_HID_KEY_COUNT
} BtHidKey_t;

/*==========================================================================
 * 3. 常用 HID 键码（Usage ID）
 *========================================================================*/
#define BT_HID_KEY_A        0x04u
#define BT_HID_KEY_1        0x1Eu
#define BT_HID_KEY_2        0x1Fu
#define BT_HID_KEY_3        0x20u
#define BT_HID_KEY_4        0x21u
#define BT_HID_KEY_5        0x22u
#define BT_HID_KEY_6        0x23u
#define BT_HID_KEY_7        0x24u
#define BT_HID_KEY_8        0x25u
#define BT_HID_KEY_9        0x26u
#define BT_HID_KEY_ENTER    0x28u
#define BT_HID_KEY_ESC      0x29u
#define BT_HID_KEY_SPACE    0x2Cu
#define BT_HID_LEFT         (0x50u)         /* 方向键 */
#define BT_HID_RIGHT        (0x4Fu)
#define BT_HID_UP           (0x52u)
#define BT_HID_DOWN         (0x51u)
#define BT_HID_VOL_UP       (0x80u)
#define BT_HID_VOL_DOWN     (0x81u)

/* 修饰键位图（报文的第 0 字节） */
#define BT_HID_MOD_LCTRL    0x01u
#define BT_HID_MOD_LSHIFT   0x02u
#define BT_HID_MOD_LALT     0x04u
#define BT_HID_MOD_LGUI     0x08u
#define BT_HID_MOD_RCTRL    0x10u
#define BT_HID_MOD_RSHIFT   0x20u
#define BT_HID_MOD_RALT     0x40u
#define BT_HID_MOD_RGUI     0x80u

/*==========================================================================
 * 4. 接口
 *========================================================================*/
void    BT_HID_Init(void);                              /* 恢复默认键位表 */

void    BT_HID_KeyEvent(BtHidKey_t key, bool down);     /* 按下/松开 */
void    BT_HID_ReleaseAll(void);                        /* 全部松开（断开前必做） */
bool    BT_HID_IsDown(BtHidKey_t key);

const uint8_t *BT_HID_Report(void);                     /* 生成并返回 8 字节报文 */
bool    BT_HID_IsDirty(void);
void    BT_HID_MarkDirty(void);
uint8_t BT_HID_OverflowCount(void);                     /* 同时按键超过 6 个的次数 */

/*********************************************************************
 * @fn      BT_HID_Flush
 * @brief   有新状态就通过 BT_SendHid() 发出去
 * @return  true = 真的发了；false = 没变化 / 没连接 / 发送失败
 *********************************************************************/
bool    BT_HID_Flush(void);

/* 键位表（默认见 bt_hid.c 顶部；usage = 0 表示只当修饰键用） */
uint8_t BT_HID_GetUsage(BtHidKey_t key);
void    BT_HID_SetUsage(BtHidKey_t key, uint8_t usage);
void    BT_HID_SetModifierBit(BtHidKey_t key, uint8_t mod);

#ifdef __cplusplus
}
#endif

#endif /* __BT_HID_H__ */
