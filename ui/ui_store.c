/**************************************************************************
 * ui_store.c —— UI 设置持久化实现
 *
 * 记录格式（16 字节，放在数据 Flash 偏移 0x0300）：
 *     0  magic  'U','I' (0x5549, 小端)
 *     2  ver    1
 *     3  led_on
 *     4  led_bright
 *     5  sleep_idx
 *     6  map_group
 *     7  reserved
 *     8..13      保留（凑 16 字节，擦除最小单位是页，这里只占一页的一部分）
 *     14 crc16   CRC-16/CCITT-FALSE，覆盖 0..13
 **************************************************************************/

#include "ui_store.h"
#include "CH59x_common.h"
#include <string.h>

/*==========================================================================
 * Flash 位置
 *========================================================================*/
#define UI_STORE_OFF        0x0300u     /* 数据 Flash 内偏移（见 ui_store.h 注释） */
#define UI_STORE_ERASE_LEN  256u        /* 擦除最小 256 字节 */

#define UI_STORE_MAGIC      0x5549u     /* 'U' | 'I'<<8 */
#define UI_STORE_VER        0x0001u
#define UI_STORE_LEN        16u
#define UI_STORE_CRC_OFF    14u

#define UI_STORE_ERASE(off) FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, (off), 0, UI_STORE_ERASE_LEN)

/*==========================================================================
 * 内存副本
 *========================================================================*/
static ui_cfg_t s_cfg;
static bool     s_loaded = false;

/*==========================================================================
 * CRC-16/CCITT-FALSE（和工程里 game_score / rf_cfg 用同一套算法，
 * 但这里自带一份，免得 ui 层反过来依赖别的模块）
 *========================================================================*/
static uint16_t ui_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t  b;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (b = 0; b < 8u; b++)
        {
            if ((crc & 0x8000u) != 0u)
            {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

/*==========================================================================
 * 打包 / 解包
 *========================================================================*/
static void ui_pack(const ui_cfg_t *c, uint8_t *out)
{
    memset(out, 0, UI_STORE_LEN);

    out[0] = (uint8_t)(UI_STORE_MAGIC & 0xFFu);
    out[1] = (uint8_t)((UI_STORE_MAGIC >> 8) & 0xFFu);
    out[2] = (uint8_t)(UI_STORE_VER & 0xFFu);
    out[3] = c->led_on;
    out[4] = c->led_bright;
    out[5] = c->sleep_idx;
    out[6] = c->map_group;
    out[7] = c->scr_bright;             /* 原来保留的字节 7，现在放屏幕亮度 */
    out[8] = c->led_fx;                 /* 原来空着的 8/9 字节，见下 */
    out[9] = c->key_hint;

    {
        uint16_t crc = ui_crc16(out, UI_STORE_CRC_OFF);
        out[UI_STORE_CRC_OFF]      = (uint8_t)(crc & 0xFFu);
        out[UI_STORE_CRC_OFF + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    }
}

static bool ui_unpack(const uint8_t *in, ui_cfg_t *c)
{
    uint16_t magic;
    uint16_t crc;

    magic = (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    if (magic != UI_STORE_MAGIC)
    {
        return false;
    }
    if (in[2] != (uint8_t)(UI_STORE_VER & 0xFFu))
    {
        return false;                   /* 版本对不上：当没有记录 */
    }

    crc = (uint16_t)((uint16_t)in[UI_STORE_CRC_OFF]
                     | ((uint16_t)in[UI_STORE_CRC_OFF + 1u] << 8));
    if (crc != ui_crc16(in, UI_STORE_CRC_OFF))
    {
        return false;
    }

    memset(c, 0, sizeof(*c));
    c->led_on     = (in[3] != 0u) ? 1u : 0u;
    c->led_bright = in[4];
    c->sleep_idx  = in[5];
    c->map_group  = in[6];
    c->scr_bright = in[7];
    c->led_fx     = in[8];
    c->key_hint   = (in[9] != 0u) ? 1u : 0u;

    /* 字段越界一律纠正，绝不把坏值放出去 */
    if (c->led_bright > UI_BRIGHT_MAX)
    {
        c->led_bright = UI_BRIGHT_MAX;
    }
    /* 屏幕亮度：可调范围 10~100。
     * 老记录里这个字节还是 0（保留位）-> 当成"没设过"给满亮度，
     * 否则升级后一开机会是一片黑屏、摸不回来。 */
    if ((c->scr_bright < (uint8_t)UI_SCR_BRIGHT_MIN) ||
        (c->scr_bright > (uint8_t)UI_SCR_BRIGHT_MAX))
    {
        c->scr_bright = (uint8_t)UI_SCR_BRIGHT_MAX;
    }
    if (c->sleep_idx >= UI_SLEEP_COUNT)
    {
        c->sleep_idx = UI_SLEEP_5MIN;
    }
    if (c->map_group >= UI_MAPGROUP_COUNT)
    {
        c->map_group = 0u;
    }
    if (c->led_fx >= UI_LEDFX_COUNT)
    {
        c->led_fx = 0u;
    }

    return true;
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
void UiStore_Default(void)
{
    s_cfg.led_on     = 1u;
    s_cfg.led_bright = 60u;
    s_cfg.scr_bright = 100u;
    s_cfg.sleep_idx  = UI_SLEEP_10MIN;
    s_cfg.map_group  = 0u;
    s_cfg.led_fx     = 0u;              /* 默认第 1 种灯效（呼吸） */
    s_cfg.key_hint   = 0u;              /* 按键提示默认关 */
}

void UiStore_Init(void)
{
    if (!UiStore_Load())
    {
        UiStore_Default();              /* 没记录 / 坏了：用默认值，不写 Flash */
    }
    s_loaded = true;
}

bool UiStore_Load(void)
{
    uint8_t buf[UI_STORE_LEN];

    memset(buf, 0, sizeof(buf));
    EEPROM_READ(UI_STORE_OFF, buf, (uint32_t)UI_STORE_LEN);

    if (ui_unpack(buf, &s_cfg))
    {
        s_loaded = true;
        return true;
    }

    /* 记录无效（没写过 / CRC 坏 / 版本对不上）：回到出厂默认。
     * 不能只是"返回 false"就走人 —— 那样内存里会留着上一次的旧值，
     * 调用方以为拿到的是默认值，实际是脏数据。 */
    UiStore_Default();
    s_loaded = true;
    return false;
}

uint8_t UiStore_Save(void)
{
    uint8_t buf[UI_STORE_LEN];
    uint8_t chk[UI_STORE_LEN];
    uint32_t irq;

    ui_pack(&s_cfg, buf);

    irq = __risc_v_disable_irq();
    UI_STORE_ERASE(UI_STORE_OFF);
    EEPROM_WRITE(UI_STORE_OFF, (void *)(uintptr_t)buf, (uint32_t)UI_STORE_LEN);
    __risc_v_enable_irq(irq);

    /* 回读校验（Flash 写失败就如实报错） */
    memset(chk, 0, sizeof(chk));
    EEPROM_READ(UI_STORE_OFF, chk, (uint32_t)UI_STORE_LEN);
    if (memcmp(chk, buf, (size_t)UI_STORE_LEN) != 0)
    {
        return UI_STORE_E_FLASH;
    }

    s_loaded = true;
    return UI_STORE_OK;
}

ui_cfg_t *UiStore_Cfg(void)
{
    if (!s_loaded)
    {
        UiStore_Init();
    }
    return &s_cfg;
}

/*==========================================================================
 * 小工具
 *========================================================================*/
uint16_t UiStore_SleepMinutes(uint8_t idx)
{
    switch (idx)
    {
        case UI_SLEEP_5MIN:   return 5u;
        case UI_SLEEP_10MIN:  return 10u;
        case UI_SLEEP_20MIN:  return 20u;
        case UI_SLEEP_60MIN:  return 60u;
        default:              return 0u;    /* 永不 / 越界 */
    }
}

uint8_t UiStore_NextSleep(uint8_t idx, int8_t dir)
{
    if (idx >= UI_SLEEP_COUNT)
    {
        idx = 0u;
    }

    if (dir > 0)
    {
        idx = (uint8_t)((idx + 1u) % UI_SLEEP_COUNT);
    }
    else if (dir < 0)
    {
        idx = (uint8_t)((idx + UI_SLEEP_COUNT - 1u) % UI_SLEEP_COUNT);
    }

    return idx;
}

uint8_t UiStore_BrightStep(uint8_t cur, int8_t dir)
{
    int16_t v = (int16_t)cur;

    if (dir > 0)
    {
        v += UI_BRIGHT_STEP;
    }
    else if (dir < 0)
    {
        v -= UI_BRIGHT_STEP;
    }

    if (v < (int16_t)UI_BRIGHT_MIN) { v = (int16_t)UI_BRIGHT_MIN; }
    if (v > (int16_t)UI_BRIGHT_MAX) { v = (int16_t)UI_BRIGHT_MAX; }

    return (uint8_t)v;
}

/* 屏幕亮度：范围 10~100、步长 5，和键盘灯亮度分开一档一档走。
 * 先把当前值对齐到 5 的整数倍再加减，避免存进来的历史值不是 5 的倍数时
 * 越调越"歪"（比如 97 -> 92 -> 87...）。 */
uint8_t UiStore_ScrBrightStep(uint8_t cur, int8_t dir)
{
    int16_t v = (int16_t)cur;

    v = (int16_t)(((v + (UI_SCR_BRIGHT_STEP / 2)) / UI_SCR_BRIGHT_STEP)
                  * UI_SCR_BRIGHT_STEP);            /* 对齐到 5 的倍数 */

    if (dir > 0)
    {
        v += UI_SCR_BRIGHT_STEP;
    }
    else if (dir < 0)
    {
        v -= UI_SCR_BRIGHT_STEP;
    }

    if (v < (int16_t)UI_SCR_BRIGHT_MIN) { v = (int16_t)UI_SCR_BRIGHT_MIN; }
    if (v > (int16_t)UI_SCR_BRIGHT_MAX) { v = (int16_t)UI_SCR_BRIGHT_MAX; }

    return (uint8_t)v;
}

uint8_t UiStore_NextMapGroup(uint8_t idx, int8_t dir)
{
    if (idx >= UI_MAPGROUP_COUNT)
    {
        idx = 0u;
    }

    if (dir > 0)
    {
        idx = (uint8_t)((idx + 1u) % UI_MAPGROUP_COUNT);
    }
    else if (dir < 0)
    {
        idx = (uint8_t)((idx + UI_MAPGROUP_COUNT - 1u) % UI_MAPGROUP_COUNT);
    }

    return idx;
}

uint8_t UiStore_NextLedFx(uint8_t idx, int8_t dir)
{
    int16_t n = (int16_t)UI_LEDFX_COUNT;

    if (n <= 0) { return 0u; }
    if (idx >= (uint8_t)n) { idx = 0u; }

    return (uint8_t)(((int16_t)idx + (int16_t)dir + n) % n);
}

uint16_t UiStore_RawSize(void)
{
    return (uint16_t)UI_STORE_LEN;
}

const void *UiStore_Raw(void)
{
    return (const void *)&s_cfg;
}
