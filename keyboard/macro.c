/**************************************************************************
 * macro.c —— 键盘宏：存储 + 播放
 *
 * 见 macro.h 的规格说明。这里只强调三件事：
 *
 *   1) 播放不复用"整张 6 键报文"，而是复用了 ui_kbd.c 的**额外键脉冲**
 *      （UiKbd_PulseMapping）。那个机制本来给编码器用：先按下、约 20ms
 *      后自动松开，而且会自动叠到物理按键的报文上。宏一步 = 一次脉冲，
 *      于是"按下→松开"的时序、修饰位合并、USB/BLE/2.4G 三条通道的选择
 *      全都交给已有的代码，这里只负责"什么时候该发下一步"。
 *
 *   2) 触发靠"按下沿"：每 1ms 轮询 9 个键的稳定状态，自己记上一次的位图，
 *      不去碰 key_matrix.c 的事件队列（那是给页面切换用的，乱读会抢事件）。
 *
 *   3) Flash 记录自带 magic + 版本 + CRC8，坏记录一律当"全空"处理，
 *      不会因为半块数据把键盘搞死。
 **************************************************************************/

#include "macro.h"
#include "key_matrix.h"
#include "ui_kbd.h"
#include "kmap_proto.h"     /* Kmap_Crc8 */
#include "game_port.h"      /* Game_Millis */

#include <string.h>

/*==========================================================================
 * Flash 记录布局（120 字节，偏移 0x1600）
 *
 *   0   2   magic  = 0x414D
 *   2   2   ver    = 0x0001
 *   4   45  bind[5][9]      （KEY_MAP_GROUPS × 9）
 *   49  4   len[4]          （每个宏的步数，0..8）
 *   53  64  step[4][8] ×2   （每步 16 位）
 *   117 1   crc8（覆盖前 117 字节）
 *   118 2   保留（凑到 4 的倍数，方便 ROM 写）
 *========================================================================*/
#define MACRO_OFF_MAGIC     0u
#define MACRO_OFF_VER       2u
#define MACRO_OFF_BIND      4u
#define MACRO_OFF_LEN       49u
#define MACRO_OFF_STEP      53u
#define MACRO_OFF_CRC       117u

#define MACRO_BIND_N        (KEY_MAP_GROUPS * 9u)

/*==========================================================================
 * RAM 副本
 *========================================================================*/
static uint8_t  s_bind[KEY_MAP_GROUPS][9];      /* 0 = 不绑，1..4 = 宏号 */
static uint8_t  s_len[MACRO_SLOTS];             /* 0..MACRO_STEPS         */
static uint16_t s_step[MACRO_SLOTS][MACRO_STEPS];

/* 播放状态 */
static uint8_t  s_run;                          /* 0 = 没在放，否则 = 宏号 1..4 */
static uint8_t  s_pos;                          /* 下一步的下标 */
static uint32_t s_next_ms;                      /* 下一步最早什么时候发 */

/* 按键按下沿检测 */
static uint16_t s_last_bits;                    /* 上一次轮询到的按下位图 */

/*==========================================================================
 * 工具
 *========================================================================*/
static uint8_t macro_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    return 2u;
}

static uint16_t macro_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 把内存里的宏/绑定打包成 Flash 那一页的样子 */
static void macro_pack(uint8_t *page)
{
    uint8_t  g;
    uint8_t  k;
    uint8_t  i;
    uint16_t off;

    memset(page, 0, MACRO_FLASH_LEN);

    (void)macro_put_u16(&page[MACRO_OFF_MAGIC], (uint16_t)MACRO_MAGIC);
    (void)macro_put_u16(&page[MACRO_OFF_VER],   (uint16_t)MACRO_VER);

    off = MACRO_OFF_BIND;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 0u; k < 9u; k++)
        {
            page[off] = (uint8_t)(s_bind[g][k] & 0x0Fu);
            off++;
        }
    }

    off = MACRO_OFF_LEN;
    for (i = 0u; i < (uint8_t)MACRO_SLOTS; i++)
    {
        page[off] = (uint8_t)((s_len[i] > (uint8_t)MACRO_STEPS) ? (uint8_t)MACRO_STEPS : s_len[i]);
        off++;
    }

    off = MACRO_OFF_STEP;
    for (i = 0u; i < (uint8_t)MACRO_SLOTS; i++)
    {
        for (k = 0u; k < (uint8_t)MACRO_STEPS; k++)
        {
            off = (uint16_t)(off + macro_put_u16(&page[off], s_step[i][k]));
        }
    }

    page[MACRO_OFF_CRC] = Kmap_Crc8(page, (uint16_t)MACRO_OFF_CRC);
}

/* 解包：先校验 magic / 版本 / CRC，不通过就当没记录（保持全空） */
static bool macro_unpack(const uint8_t *page)
{
    uint8_t  g;
    uint8_t  k;
    uint8_t  i;
    uint16_t off;

    if (macro_get_u16(&page[MACRO_OFF_MAGIC]) != (uint16_t)MACRO_MAGIC)
    {
        return false;
    }
    if (macro_get_u16(&page[MACRO_OFF_VER]) != (uint16_t)MACRO_VER)
    {
        return false;
    }
    if (page[MACRO_OFF_CRC] != Kmap_Crc8(page, (uint16_t)MACRO_OFF_CRC))
    {
        return false;
    }

    off = MACRO_OFF_BIND;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 0u; k < 9u; k++)
        {
            uint8_t v = (uint8_t)(page[off] & 0x0Fu);

            s_bind[g][k] = ((v >= 1u) && (v <= (uint8_t)MACRO_SLOTS)) ? v : 0u;
            off++;
        }
    }

    off = MACRO_OFF_LEN;
    for (i = 0u; i < (uint8_t)MACRO_SLOTS; i++)
    {
        uint8_t v = page[off];

        s_len[i] = (v > (uint8_t)MACRO_STEPS) ? (uint8_t)MACRO_STEPS : v;
        off++;
    }

    off = MACRO_OFF_STEP;
    for (i = 0u; i < (uint8_t)MACRO_SLOTS; i++)
    {
        for (k = 0u; k < (uint8_t)MACRO_STEPS; k++)
        {
            s_step[i][k] = macro_get_u16(&page[off]);
            off = (uint16_t)(off + 2u);
        }
    }

    return true;
}

/*==========================================================================
 * 对外：初始化 / 存盘
 *========================================================================*/
void Macro_Init(void)
{
    uint8_t page[MACRO_FLASH_LEN];

    memset(s_bind, 0, sizeof(s_bind));
    memset(s_len,  0, sizeof(s_len));
    memset(s_step, 0, sizeof(s_step));
    s_run      = 0u;
    s_pos      = 0u;
    s_next_ms  = 0u;
    s_last_bits = 0u;

    memset(page, 0, sizeof(page));
    EEPROM_READ(MACRO_FLASH_OFF, page, (uint32_t)MACRO_FLASH_LEN);

    (void)macro_unpack(page);       /* 校验不过就当全空，不算错误 */
}

uint8_t Macro_Save(void)
{
    uint8_t  page[MACRO_FLASH_LEN];
    uint8_t  chk[MACRO_FLASH_LEN];

    macro_pack(page);

    /* 擦整块（256B），只写前 120 字节；剩下的是 0xFF，读回来也是 0xFF，
     * 所以这里校验只比前 120 字节。 */
    FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, MACRO_FLASH_OFF, 0, 256u);

    if (EEPROM_WRITE(MACRO_FLASH_OFF, (void *)(uintptr_t)page, (uint32_t)MACRO_FLASH_LEN) != 0u)
    {
        return 1u;                  /* Flash 写失败 */
    }

    memset(chk, 0, sizeof(chk));
    EEPROM_READ(MACRO_FLASH_OFF, chk, (uint32_t)MACRO_FLASH_LEN);
    if (memcmp(page, chk, sizeof(page)) != 0)
    {
        return 3u;                  /* 回读校验不一致 */
    }

    return 0u;
}

/*==========================================================================
 * 对外：绑定表 / 宏内容
 *========================================================================*/
uint8_t Macro_GetBind(uint8_t group, uint8_t key)
{
    if ((group >= (uint8_t)KEY_MAP_GROUPS) || (key < 1u) || (key > 9u))
    {
        return 0u;
    }
    return s_bind[group][key - 1u];
}

void Macro_SetBind(uint8_t group, uint8_t key, uint8_t idx)
{
    if ((group >= (uint8_t)KEY_MAP_GROUPS) || (key < 1u) || (key > 9u))
    {
        return;
    }
    s_bind[group][key - 1u] = (idx <= (uint8_t)MACRO_SLOTS) ? idx : 0u;
}

void Macro_GetBindRow(uint8_t group, uint8_t *out9)
{
    uint8_t k;

    if ((out9 == 0) || (group >= (uint8_t)KEY_MAP_GROUPS))
    {
        return;
    }
    for (k = 0u; k < 9u; k++)
    {
        out9[k] = s_bind[group][k];
    }
}

void Macro_SetBindRow(uint8_t group, const uint8_t *in9)
{
    uint8_t k;

    if ((in9 == 0) || (group >= (uint8_t)KEY_MAP_GROUPS))
    {
        return;
    }
    for (k = 0u; k < 9u; k++)
    {
        uint8_t v = (uint8_t)(in9[k] & 0x0Fu);

        s_bind[group][k] = ((v >= 1u) && (v <= (uint8_t)MACRO_SLOTS)) ? v : 0u;
    }
}

uint8_t Macro_GetLen(uint8_t idx)
{
    if ((idx < 1u) || (idx > (uint8_t)MACRO_SLOTS))
    {
        return 0u;
    }
    return s_len[idx - 1u];
}

uint16_t Macro_GetStep(uint8_t idx, uint8_t n)
{
    if ((idx < 1u) || (idx > (uint8_t)MACRO_SLOTS) || (n >= (uint8_t)MACRO_STEPS))
    {
        return 0u;
    }
    return s_step[idx - 1u][n];
}

void Macro_SetLen(uint8_t idx, uint8_t len)
{
    if ((idx < 1u) || (idx > (uint8_t)MACRO_SLOTS))
    {
        return;
    }
    s_len[idx - 1u] = (len > (uint8_t)MACRO_STEPS) ? (uint8_t)MACRO_STEPS : len;
}

void Macro_SetStep(uint8_t idx, uint8_t n, uint16_t value)
{
    if ((idx < 1u) || (idx > (uint8_t)MACRO_SLOTS) || (n >= (uint8_t)MACRO_STEPS))
    {
        return;
    }
    s_step[idx - 1u][n] = value;
}

/*==========================================================================
 * 播放
 *========================================================================*/
bool Macro_IsRunning(void)
{
    return (s_run != 0u) ? true : false;
}

void Macro_Stop(void)
{
    s_run = 0u;
    s_pos = 0u;
}

void Macro_Trigger(uint8_t idx)
{
    if ((idx < 1u) || (idx > (uint8_t)MACRO_SLOTS))
    {
        return;
    }
    if (s_len[idx - 1u] == 0u)
    {
        return;                     /* 空宏不播 */
    }

    s_run     = idx;
    s_pos     = 0u;
    s_next_ms = Game_Millis();      /* 第一步立刻发 */
}

/* 当前组里绑了宏、并且此刻按着的键（给 ui_kbd.c 屏蔽原映射用） */
uint16_t Macro_BoundMask(void)
{
    uint16_t mask = 0u;
    uint8_t  k;
    uint8_t  g = KEY_GetMapGroup();

    for (k = 1u; k <= 9u; k++)
    {
        if (KEY_IsPressed(k) && (Macro_GetBind(g, k) != 0u))
        {
            mask |= (uint16_t)(1u << (k - 1u));
        }
    }
    return mask;
}

/* 每 1ms 调一次（UiKbd_Task 里） */
void Macro_Task(void)
{
    uint8_t  k;
    uint16_t bits = 0u;

    /* ---- 1. 扫按下沿 ---- */
    for (k = 1u; k <= 9u; k++)
    {
        if (KEY_IsPressed(k))
        {
            bits |= (uint16_t)(1u << (k - 1u));
        }
    }

    if (bits != s_last_bits)
    {
        uint16_t newly = (uint16_t)(bits & (uint16_t)(~s_last_bits));

        if (newly != 0u)
        {
            /* 有新键按下：先停掉正在放的宏（防止宏卡在中间） */
            if (s_run != 0u)
            {
                Macro_Stop();
            }

            for (k = 1u; k <= 9u; k++)
            {
                if ((newly & (uint16_t)(1u << (k - 1u))) != 0u)
                {
                    uint8_t idx = Macro_GetBind(KEY_GetMapGroup(), k);

                    if (idx != 0u)
                    {
                        Macro_Trigger(idx);
                        break;          /* 一次只放一个宏 */
                    }
                }
            }
        }
        s_last_bits = bits;
    }

    /* ---- 2. 推进播放 ---- */
    if (s_run == 0u)
    {
        return;
    }
    if ((int32_t)(Game_Millis() - s_next_ms) < 0)
    {
        return;                     /* 还没到下一步 */
    }

    if (s_pos >= s_len[s_run - 1u])
    {
        Macro_Stop();               /* 放完了 */
        return;
    }

    UiKbd_PulseMapping(s_step[s_run - 1u][s_pos]);
    s_pos++;
    s_next_ms = Game_Millis() + (uint32_t)MACRO_STEP_MS;
}
