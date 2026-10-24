/**************************************************************************
 * bt_hid.c —— CH592X 标准 BLE HID 键盘报文（8 字节 boot 键盘）
 *
 * 报文格式（蓝牙 HID boot keyboard，和 USB 键盘一样）：
 *      [0] 修饰键位图（bit0 左Ctrl、bit1 左Shift、……）
 *      [1] 保留，固定 0
 *      [2..7] 最多 6 个同时按下的键的 HID 键码（Usage ID）
 *
 * 默认键位：
 *      K1 ~ K9    -> 数字 1 ~ 9          （HID 0x1E ~ 0x26）
 *      编码器正转 -> 音量 +              （HID 0x80）
 *      编码器反转 -> 音量 -              （HID 0x81）
 *      编码器按下 -> 回车                （HID 0x28）
 *
 * 想改成别的键：BT_HID_SetUsage()，或者给某个键挂修饰键
 * BT_HID_SetModifierBit()（例如把它设成 Shift，再配合其它键）。
 **************************************************************************/

#include "bt_hid.h"
#include "bt_port.h"
#include <string.h>

/*==========================================================================
 * 状态
 *========================================================================*/
static uint8_t s_usage[BT_HID_KEY_COUNT];
static uint8_t s_modbit[BT_HID_KEY_COUNT];
static bool    s_down[BT_HID_KEY_COUNT];
static uint8_t s_report[BT_HID_REPORT_LEN];
static bool    s_dirty;
static uint8_t s_overflow;                  /* 同时按超过 6 个键的次数（调试用） */

/*==========================================================================
 * 默认键位表
 *========================================================================*/
static const uint8_t s_default_usage[BT_HID_KEY_COUNT] =
{
    BT_HID_KEY_1, BT_HID_KEY_2, BT_HID_KEY_3,
    BT_HID_KEY_4, BT_HID_KEY_5, BT_HID_KEY_6,
    BT_HID_KEY_7, BT_HID_KEY_8, BT_HID_KEY_9,
    BT_HID_VOL_UP, BT_HID_VOL_DOWN, BT_HID_KEY_ENTER
};

void BT_HID_Init(void)
{
    uint8_t i;

    for (i = 0; i < (uint8_t)BT_HID_KEY_COUNT; i++)
    {
        s_usage[i]  = s_default_usage[i];
        s_modbit[i] = 0u;
        s_down[i]   = false;
    }

    memset(s_report, 0, sizeof(s_report));
    s_dirty = false;
    s_overflow = 0u;
}

uint8_t BT_HID_GetUsage(BtHidKey_t key)
{
    if ((uint8_t)key >= (uint8_t)BT_HID_KEY_COUNT)
    {
        return 0u;
    }
    return s_usage[(uint8_t)key];
}

void BT_HID_SetUsage(BtHidKey_t key, uint8_t usage)
{
    if ((uint8_t)key < (uint8_t)BT_HID_KEY_COUNT)
    {
        s_usage[(uint8_t)key] = usage;
    }
}

void BT_HID_SetModifierBit(BtHidKey_t key, uint8_t mod)
{
    if ((uint8_t)key < (uint8_t)BT_HID_KEY_COUNT)
    {
        s_modbit[(uint8_t)key] = mod;
    }
}

/*==========================================================================
 * 按键事件
 *========================================================================*/
void BT_HID_KeyEvent(BtHidKey_t key, bool down)
{
    if ((uint8_t)key >= (uint8_t)BT_HID_KEY_COUNT)
    {
        return;
    }
    if (s_down[(uint8_t)key] == down)
    {
        return;                             /* 状态没变 */
    }
    s_down[(uint8_t)key] = down;
    s_dirty = true;
}

bool BT_HID_IsDown(BtHidKey_t key)
{
    if ((uint8_t)key >= (uint8_t)BT_HID_KEY_COUNT)
    {
        return false;
    }
    return s_down[(uint8_t)key];
}

void BT_HID_ReleaseAll(void)
{
    uint8_t i;

    for (i = 0; i < (uint8_t)BT_HID_KEY_COUNT; i++)
    {
        if (s_down[i])
        {
            s_down[i] = false;
            s_dirty = true;
        }
    }
}

/*==========================================================================
 * 生成报文
 *========================================================================*/
static void bt_hid_build(void)
{
    uint8_t i, n = 0u, mods = 0u;

    s_report[0] = 0u;
    s_report[1] = 0u;
    for (i = 2u; i < BT_HID_REPORT_LEN; i++)
    {
        s_report[i] = 0u;
    }

    for (i = 0; i < (uint8_t)BT_HID_KEY_COUNT; i++)
    {
        if (!s_down[i])
        {
            continue;
        }

        mods |= s_modbit[i];

        if (s_usage[i] == 0u)
        {
            continue;                       /* 只当修饰键用 */
        }

        if (n < BT_HID_MAX_KEYS)
        {
            s_report[2u + n] = s_usage[i];
            n++;
        }
        else
        {
            s_overflow++;                   /* 超过 6 键，多出来的丢掉 */
        }
    }

    s_report[0] = mods;
}

const uint8_t *BT_HID_Report(void)
{
    bt_hid_build();
    return s_report;
}

bool BT_HID_IsDirty(void) { return s_dirty; }

void BT_HID_MarkDirty(void) { s_dirty = true; }

uint8_t BT_HID_OverflowCount(void) { return s_overflow; }

bool BT_HID_Flush(void)
{
    if (!s_dirty)
    {
        return false;
    }
    if (!BT_IsConnected())
    {
        return false;                       /* 没连上就先留着，连上后再发 */
    }

    bt_hid_build();

    if (!BT_SendHid(s_report, BT_HID_REPORT_LEN))
    {
        return false;
    }

    s_dirty = false;
    return true;
}

/******************* (C) CH592X BLE HID 键盘 *****************/
