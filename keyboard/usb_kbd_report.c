/**************************************************************************
 * usb_kbd_report.c —— USB HID 键盘报文层实现
 **************************************************************************/

#include "usb_kbd_report.h"
#include <string.h>

/*==========================================================================
 * 状态
 *========================================================================*/
static uint8_t s_usage[USBKBD_KEY_COUNT];
static uint8_t s_modbit[USBKBD_KEY_COUNT];
static bool    s_down[USBKBD_KEY_COUNT];
static uint8_t s_report[USBKBD_REPORT_LEN];
static bool    s_dirty;
static uint8_t s_overflow;
static uint8_t s_led;

/*==========================================================================
 * 默认键位表
 *
 * ★ 这张表和 CH592X_bluetooth\bt_hid.c 里的 s_default_usage[] 必须逐字节一致。
 *   tests\test_report.c 的 T8 会把它和硬编码的期望值比一遍，
 *   两边任何一处被改动都会立刻测出来。
 *========================================================================*/
static const uint8_t s_default_usage[USBKBD_KEY_COUNT] =
{
    USBKBD_KEY_1, USBKBD_KEY_2, USBKBD_KEY_3,
    USBKBD_KEY_4, USBKBD_KEY_5, USBKBD_KEY_6,
    USBKBD_KEY_7, USBKBD_KEY_8, USBKBD_KEY_9,
    USBKBD_VOL_UP, USBKBD_VOL_DOWN, USBKBD_KEY_ENTER
};

/*==========================================================================
 * 初始化
 *========================================================================*/
void UsbKbd_Init(void)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)USBKBD_KEY_COUNT; i++)
    {
        s_usage[i]  = s_default_usage[i];
        s_modbit[i] = 0u;
        s_down[i]   = false;
    }

    memset(s_report, 0, sizeof(s_report));
    s_dirty    = false;
    s_overflow = 0u;
    s_led      = 0u;
}

/*==========================================================================
 * 键位表
 *========================================================================*/
uint8_t UsbKbd_GetUsage(UsbKbdKey_t key)
{
    if ((uint8_t)key >= (uint8_t)USBKBD_KEY_COUNT)
    {
        return 0u;
    }
    return s_usage[(uint8_t)key];
}

void UsbKbd_SetUsage(UsbKbdKey_t key, uint8_t usage)
{
    if ((uint8_t)key < (uint8_t)USBKBD_KEY_COUNT)
    {
        s_usage[(uint8_t)key] = usage;
    }
}

void UsbKbd_SetModifierBit(UsbKbdKey_t key, uint8_t mod)
{
    if ((uint8_t)key < (uint8_t)USBKBD_KEY_COUNT)
    {
        s_modbit[(uint8_t)key] = mod;
    }
}

/*==========================================================================
 * 按键状态
 *========================================================================*/
void UsbKbd_KeyEvent(UsbKbdKey_t key, bool down)
{
    if ((uint8_t)key >= (uint8_t)USBKBD_KEY_COUNT)
    {
        return;
    }
    if (s_down[(uint8_t)key] == down)
    {
        return;                             /* 状态没变，不置脏 */
    }

    s_down[(uint8_t)key] = down;
    s_dirty = true;
}

bool UsbKbd_IsDown(UsbKbdKey_t key)
{
    if ((uint8_t)key >= (uint8_t)USBKBD_KEY_COUNT)
    {
        return false;
    }
    return s_down[(uint8_t)key];
}

void UsbKbd_ReleaseAll(void)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)USBKBD_KEY_COUNT; i++)
    {
        if (s_down[i])
        {
            s_down[i] = false;
            s_dirty   = true;
        }
    }
}

uint8_t UsbKbd_DownCount(void)
{
    uint8_t i;
    uint8_t n = 0u;

    for (i = 0u; i < (uint8_t)USBKBD_KEY_COUNT; i++)
    {
        if (s_down[i])
        {
            n++;
        }
    }
    return n;
}

bool UsbKbd_AnyDown(void)
{
    return (UsbKbd_DownCount() != 0u);
}

/*==========================================================================
 * 生成 8 字节 boot 报文
 *========================================================================*/
const uint8_t *UsbKbd_BuildReport(void)
{
    uint8_t i;
    uint8_t n = 0u;
    uint8_t mods = 0u;

    memset(s_report, 0, sizeof(s_report));

    for (i = 0u; i < (uint8_t)USBKBD_KEY_COUNT; i++)
    {
        if (!s_down[i])
        {
            continue;
        }

        mods |= s_modbit[i];                /* 不管是不是纯修饰键，都参与修饰位图 */

        if (s_usage[i] == 0u)
        {
            continue;                       /* usage=0：只当修饰键用 */
        }

        if (n < (uint8_t)USBKBD_MAX_KEYS)
        {
            s_report[2u + n] = s_usage[i];
            n++;
        }
        else
        {
            s_overflow++;                   /* boot 报文只能放 6 个，多的丢掉 */
        }
    }

    s_report[0] = mods;
    s_report[1] = 0u;                       /* 保留字节，规范要求固定 0 */

    return s_report;
}

const uint8_t *UsbKbd_PeekReport(void)
{
    return s_report;
}

uint8_t UsbKbd_BuildNkroBitmap(uint8_t *out28)
{
    uint8_t i;
    uint8_t n = 0u;

    if (out28 == 0)
    {
        return 0u;
    }

    memset(out28, 0, (size_t)USBKBD_NKRO_BYTES);

    for (i = 0u; i < (uint8_t)USBKBD_KEY_COUNT; i++)
    {
        uint8_t u;

        if (!s_down[i])
        {
            continue;
        }

        u = s_usage[i];
        if (u == 0u)
        {
            continue;
        }
        if (u >= (uint8_t)(USBKBD_NKRO_BYTES * 8u))
        {
            continue;                       /* 超出 224 键码范围，丢掉 */
        }

        out28[u >> 3] |= (uint8_t)(1u << (u & 7u));
        n++;
    }

    return n;
}

/*==========================================================================
 * 脏标志 / 统计
 *========================================================================*/
bool UsbKbd_IsDirty(void)
{
    return s_dirty;
}

void UsbKbd_MarkDirty(void)
{
    s_dirty = true;
}

void UsbKbd_ClearDirty(void)
{
    s_dirty = false;
}

uint8_t UsbKbd_OverflowCount(void)
{
    return s_overflow;
}

/*==========================================================================
 * LED
 *========================================================================*/
uint8_t UsbKbd_SetLedFromHost(uint8_t led_byte)
{
    /* 只保留标准定义的 5 位，其余位忽略（有些主机会把保留位写成 1） */
    s_led = (uint8_t)(led_byte & (USBKBD_LED_NUMLOCK
                                  | USBKBD_LED_CAPSLOCK
                                  | USBKBD_LED_SCROLLLOCK
                                  | USBKBD_LED_COMPOSE
                                  | USBKBD_LED_KANA));
    return s_led;
}

uint8_t UsbKbd_GetLed(void)
{
    return s_led;
}

bool UsbKbd_LedNumLock(void)
{
    return ((s_led & USBKBD_LED_NUMLOCK) != 0u);
}

bool UsbKbd_LedCapsLock(void)
{
    return ((s_led & USBKBD_LED_CAPSLOCK) != 0u);
}

bool UsbKbd_LedScrollLock(void)
{
    return ((s_led & USBKBD_LED_SCROLLLOCK) != 0u);
}

/******************* (C) CH592X USB HID 键盘 *****************/
