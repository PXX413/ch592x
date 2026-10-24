/**************************************************************************
 * link_hop.c —— 跳频实现
 **************************************************************************/

#include "link_hop.h"

/*==========================================================================
 * 频点表（规范 7.1）
 *   键盘：避开 WiFi CH1/6/11 中心（2.412/2.437/2.462 GHz）
 *   遥控：步长 7 个信道（14 MHz），频谱铺得更开
 *========================================================================*/
/* 【修复 P0-2】原来的 50/47/54 会发到 2500/2494/2508MHz，全部超出 ISM
 * 上限 2483.5MHz。现在一律落在 0..41（2400~2482MHz）内，且尽量保留原值：
 *     键盘：50 -> 41，20/35 不动
 *     遥控：47 -> 41、54 -> 2（2404MHz），5/12/19/26/33/40 不动
 * 注意：从机的跳频表必须与本表逐项一致，改这里要同步改从机固件。 */
const uint8_t hop_kb_table[HOP_KB_COUNT] = { 20u, 35u, 41u };
const uint8_t hop_rc_table[HOP_RC_COUNT] = { 2u, 5u, 12u, 19u, 26u, 33u, 40u, 41u };

#define HOP_SEED_DEFAULT        0x0Bu       /* 种子 0 换成这个（不能是 0） */

/*==========================================================================
 * LFSR（规范 7.2）
 *
 *   5-bit Galois LFSR，右移，移出的最低位决定要不要异或抽头
 *       状态 = (状态 >> 1) ^ (最低位 ? 0x14 : 0)
 *   非 0 状态周期 = 31
 *========================================================================*/
uint8_t Hop_LfsrNext(uint8_t state)
{
    uint8_t lsb;

    state &= 0x1Fu;
    if (state == 0u)
    {
        state = (uint8_t)HOP_SEED_DEFAULT;
    }

    lsb = (uint8_t)(state & 0x01u);
    state = (uint8_t)(state >> 1);

    if (lsb != 0u)
    {
        state ^= 0x14u;
    }

    state &= 0x1Fu;
    if (state == 0u)
    {
        /* 理论上不会走到（多项式是本原的），兜一下底 */
        state = (uint8_t)HOP_SEED_DEFAULT;
    }

    return state;
}

/*==========================================================================
 * 初始化
 *========================================================================*/
void Hop_Init(hop_ctx_t *h, uint8_t seed, uint8_t count, const uint8_t *table)
{
    uint8_t i;

    if (h == 0)
    {
        return;
    }

    for (i = 0; i < HOP_RC_COUNT; i++)
    {
        h->table[i]  = 0u;
        h->bad_cnt[i] = 0u;
    }

    if (count == 0u)
    {
        count = (uint8_t)HOP_RC_COUNT;
    }
    if (count > (uint8_t)HOP_RC_COUNT)
    {
        count = (uint8_t)HOP_RC_COUNT;
    }
    h->count = count;

    if (table == 0)
    {
        table = (count == (uint8_t)HOP_KB_COUNT) ? hop_kb_table : hop_rc_table;
    }

    for (i = 0; i < count; i++)
    {
        h->table[i] = table[i];
    }

    h->seed      = (seed == 0u) ? (uint8_t)HOP_SEED_DEFAULT : (uint8_t)(seed & 0x1Fu);
    if (h->seed == 0u)
    {
        h->seed = (uint8_t)HOP_SEED_DEFAULT;
    }

    /* 抽稀步长 = ceil(31 / count)
     *
     * 规范 7.2 直接写 rc_hop_tbl[seed % 8]，实测（见 README 的实测表）会出现
     * "7 7 7" / "0 0 0 0 0" 这种连续停在同一频点的情况 —— 因为 5 bit 的
     * LFSR 走 1..31，对 3 或 8 取模本来就不均匀。
     * 这里先让 LFSR 一次多走几步（31 是素数，只要步长 <31 就仍然是满周期），
     * 再把状态等比映射到 0..count-1，并把"连着两次同一频点"挡掉。
     * 实测：count=3 最长同频从 7 段降到 1 段，count=8 从 3 段降到 1 段。 */
    h->step      = (uint8_t)((31u + (uint32_t)count - 1u) / (uint32_t)count);

    h->pos       = 0u;
    h->blacklist = 0u;
    h->tick      = 0u;
}

/*==========================================================================
 * 频点查找 / 屏蔽
 *========================================================================*/
uint8_t Hop_Index(const hop_ctx_t *h, uint8_t ch)
{
    uint8_t i;

    if (h == 0)
    {
        return 0xFFu;
    }

    for (i = 0; i < h->count; i++)
    {
        if (h->table[i] == ch)
        {
            return i;
        }
    }
    return 0xFFu;
}

bool Hop_IsBlacklisted(const hop_ctx_t *h, uint8_t ch)
{
    uint8_t idx;

    if (h == 0)
    {
        return false;
    }

    idx = Hop_Index(h, ch);
    if (idx == 0xFFu)
    {
        return false;
    }
    return ((h->blacklist & (uint8_t)(1u << idx)) != 0u);
}

void Hop_BlacklistAdd(hop_ctx_t *h, uint8_t ch)
{
    uint8_t idx;

    if (h == 0)
    {
        return;
    }

    idx = Hop_Index(h, ch);
    if (idx != 0xFFu)
    {
        h->blacklist |= (uint8_t)(1u << idx);
    }
}

void Hop_BlacklistClear(hop_ctx_t *h)
{
    uint8_t i;

    if (h == 0)
    {
        return;
    }

    h->blacklist = 0u;
    for (i = 0; i < HOP_RC_COUNT; i++)
    {
        h->bad_cnt[i] = 0u;
    }
}

uint8_t Hop_BlacklistCount(const hop_ctx_t *h)
{
    uint8_t i;
    uint8_t n = 0u;

    if (h == 0)
    {
        return 0u;
    }

    for (i = 0; i < h->count; i++)
    {
        if ((h->blacklist & (uint8_t)(1u << i)) != 0u)
        {
            n++;
        }
    }
    return n;
}

bool Hop_NeedResync(const hop_ctx_t *h)
{
    return (Hop_BlacklistCount(h) >= (uint8_t)HOP_BLACKLIST_MAX);
}

/*==========================================================================
 * 劣化判定（规范 7.3）
 *
 *   规范原文是 g_ch_blacklist |= (1 << (ch/7))，按"信道号/7"来索引；
 *   这样 5..54 会落到 bit0..bit7，但同一段里的两个频点会互相牵连。
 *   本实现改成**按频点表下标**索引，行为更直观也更好测。
 *========================================================================*/
bool Hop_QualityUpdate(hop_ctx_t *h, uint8_t ch, int8_t rssi)
{
    uint8_t idx;
    bool    newly = false;

    if (h == 0)
    {
        return false;
    }

    idx = Hop_Index(h, ch);
    if (idx == 0xFFu)
    {
        return false;
    }

    if (rssi < (int8_t)HOP_RSSI_BAD)
    {
        if (h->bad_cnt[idx] < 0xFFu)
        {
            h->bad_cnt[idx]++;
        }

        if ((h->bad_cnt[idx] >= (uint8_t)HOP_BAD_CNT_TH)
            && ((h->blacklist & (uint8_t)(1u << idx)) == 0u))
        {
            h->blacklist |= (uint8_t)(1u << idx);
            newly = true;
        }
    }
    else
    {
        h->bad_cnt[idx] = 0u;
    }

    return newly;
}

/*==========================================================================
 * 跳频推进
 *========================================================================*/
uint8_t Hop_Current(const hop_ctx_t *h)
{
    if ((h == 0) || (h->count == 0u))
    {
        return 0xFFu;
    }
    return h->table[h->pos % h->count];
}

uint8_t Hop_Next(hop_ctx_t *h)
{
    uint8_t i;
    uint8_t k;
    uint8_t idx;
    uint8_t avail = 0u;
    uint8_t step;
    uint8_t tries;

    if ((h == 0) || (h->count == 0u))
    {
        return 0xFFu;
    }

    for (i = 0; i < h->count; i++)
    {
        if ((h->blacklist & (uint8_t)(1u << i)) == 0u)
        {
            avail++;
        }
    }

    if (avail == 0u)
    {
        return 0xFFu;               /* 全黑了，NeedResync 会先触发 */
    }

    step  = (h->step != 0u) ? h->step : 1u;
    tries = (uint8_t)(h->count * 2u + 4u);

    for (i = 0; i < tries; i++)
    {
        for (k = 0; k < step; k++)
        {
            h->seed = Hop_LfsrNext(h->seed);
        }

        /* 把 5 bit 的状态等比映射到 0..count-1 */
        idx = (uint8_t)(((uint32_t)h->seed * (uint32_t)h->count) >> 5);
        if (idx >= h->count)
        {
            idx = (uint8_t)(h->count - 1u);
        }

        /* 跳过被拉黑的频点 */
        if ((h->blacklist & (uint8_t)(1u << idx)) != 0u)
        {
            continue;
        }

        /* 频点不止一个时，不允许连着两次停在同一频点 */
        if ((avail >= 2u) && (h->tick > 0u) && (idx == h->pos))
        {
            continue;
        }

        h->pos = idx;
        h->tick++;
        return h->table[idx];
    }

    return h->table[h->pos];        /* 兜底，正常走不到这里 */
}

void Hop_SetIndex(hop_ctx_t *h, uint8_t idx)
{
    if ((h == 0) || (h->count == 0u))
    {
        return;
    }
    h->pos = (uint8_t)(idx % h->count);
}

/*********************************************************************
 * @fn      Hop_SyncTo
 * @brief   从机按 SYNC 帧硬对齐跳频状态
 * @param   h   状态
 * @param   idx  SYNC.hop_channel（频点表下标）
 * @param   seed SYNC.seed_lfsr（LFSR 当前值，0 = 只对齐下标）
 * @note    规范 5.3.1 里 seed_lfsr 只写了"种子增量"，语义很含糊；
 *          这里直接塞 LFSR 当前值，从机可以一次对齐，比"增量"更可靠。
 *********************************************************************/
void Hop_SyncTo(hop_ctx_t *h, uint8_t idx, uint8_t seed)
{
    if ((h == 0) || (h->count == 0u))
    {
        return;
    }

    h->pos = (uint8_t)(idx % h->count);

    if (seed != 0u)
    {
        h->seed = (uint8_t)(seed & 0x1Fu);
        if (h->seed == 0u)
        {
            h->seed = (uint8_t)HOP_SEED_DEFAULT;
        }
    }
}

uint8_t Hop_Seed(const hop_ctx_t *h)
{
    if (h == 0)
    {
        return 0u;
    }
    return h->seed;
}
