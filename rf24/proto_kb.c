/**************************************************************************
 * proto_kb.c —— 键盘模式协议实现
 **************************************************************************/

#include "proto_kb.h"
#include "hal_rf.h"
#include <string.h>

/*==========================================================================
 * HID 报告辅助
 *========================================================================*/
void Hid_Clear(hid_report_t *r)
{
    if (r != 0)
    {
        memset(r, 0, sizeof(*r));
    }
}

bool Hid_IsEmpty(const hid_report_t *r)
{
    uint8_t i;

    if (r == 0)
    {
        return true;
    }

    if (r->modifiers != 0u)
    {
        return false;
    }

    for (i = 0; i < HID_6KRO_KEYS; i++)
    {
        if (r->keys[i] != 0u)
        {
            return false;
        }
    }

    for (i = 0; i < HID_NKRO_BYTES; i++)
    {
        if (r->nkro[i] != 0u)
        {
            return false;
        }
    }

    return true;
}

/* NKRO 位图：bit[N] = HID 键码 N 是否按下（覆盖 224 个键码） */
void Hid_NkroSet(hid_report_t *r, uint8_t usage, bool down)
{
    if (r == 0)
    {
        return;
    }
    if (usage >= (uint8_t)(HID_NKRO_BYTES * 8u))
    {
        return;
    }

    if (down)
    {
        r->nkro[usage >> 3] |= (uint8_t)(1u << (usage & 7u));
    }
    else
    {
        r->nkro[usage >> 3] &= (uint8_t)~(1u << (usage & 7u));
    }
}

bool Hid_NkroTest(const hid_report_t *r, uint8_t usage)
{
    if ((r == 0) || (usage >= (uint8_t)(HID_NKRO_BYTES * 8u)))
    {
        return false;
    }
    return ((r->nkro[usage >> 3] & (uint8_t)(1u << (usage & 7u))) != 0u);
}

/* 6KRO：最多 6 个同时按下的非修饰键，多的丢掉 */
void Hid_6KroSet(hid_report_t *r, uint8_t usage, bool down)
{
    uint8_t i;

    if (r == 0)
    {
        return;
    }

    if (down)
    {
        for (i = 0; i < HID_6KRO_KEYS; i++)
        {
            if (r->keys[i] == usage)
            {
                return;                     /* 已经在了 */
            }
        }
        for (i = 0; i < HID_6KRO_KEYS; i++)
        {
            if (r->keys[i] == 0u)
            {
                r->keys[i] = usage;
                return;
            }
        }
        /* 满了：丢掉（6KRO 的固有限制） */
        return;
    }

    for (i = 0; i < HID_6KRO_KEYS; i++)
    {
        if (r->keys[i] == usage)
        {
            /* 后面的往前挪，保持紧凑 */
            uint8_t j;
            for (j = (uint8_t)(i + 1u); j < HID_6KRO_KEYS; j++)
            {
                r->keys[j - 1u] = r->keys[j];
            }
            r->keys[HID_6KRO_KEYS - 1u] = 0u;
            return;
        }
    }
}

bool Hid_6KroTest(const hid_report_t *r, uint8_t usage)
{
    uint8_t i;

    if (r == 0)
    {
        return false;
    }

    for (i = 0; i < HID_6KRO_KEYS; i++)
    {
        if (r->keys[i] == usage)
        {
            return true;
        }
    }
    return false;
}

/*==========================================================================
 * 配对
 *========================================================================*/
void Kb_PairDefault(kb_pair_info_t *p)
{
    if (p == 0)
    {
        return;
    }

    p->addr[0] = (uint8_t)RF_ADDR_KB_PREFIX0;
    p->addr[1] = (uint8_t)RF_ADDR_KB_PREFIX1;
    p->addr[2] = (uint8_t)RF_ADDR_KB_PREFIX2;
    p->addr[3] = (uint8_t)RF_ADDR_KB_PREFIX3;
    p->addr[4] = 0x3Fu;                     /* 出厂默认后缀，配对时会被随机覆盖 */

    p->channel = hop_kb_table[0];
    p->hop[0]  = hop_kb_table[0];
    p->hop[1]  = hop_kb_table[1];
    p->hop[2]  = hop_kb_table[2];
    p->nkro_en = false;
}

void Kb_BuildPair(uint8_t *out16, uint8_t stage, const kb_pair_info_t *p, uint32_t rnd)
{
    if ((out16 == 0) || (p == 0))
    {
        return;
    }

    memset(out16, 0, KB_PAIR_LEN);
    out16[0] = (uint8_t)KB_PAIR;
    out16[1] = stage;
    memcpy(&out16[2], p->addr, RF_ADDR_LEN);
    out16[7] = p->channel;
    out16[8] = p->hop[0];
    out16[9] = p->hop[1];
    out16[10] = p->hop[2];
    Rf_PutU32LE(&out16[11], rnd);
    out16[15] = Rf_Crc8(out16, 15u);
}

uint8_t Kb_ParsePair(const uint8_t *in16, uint8_t *stage, kb_pair_info_t *p, uint32_t *rnd)
{
    if ((in16 == 0) || (p == 0))
    {
        return RC_E_PARAM;
    }

    if (FRAME_TYPE(in16[0]) != (uint8_t)KB_PAIR)
    {
        return RC_E_PARAM;
    }

    if (in16[15] != Rf_Crc8(in16, 15u))
    {
        return RC_E_CRC;
    }

    if (stage != 0)
    {
        *stage = in16[1];
    }

    memcpy(p->addr, &in16[2], RF_ADDR_LEN);
    p->channel = in16[7];
    p->hop[0]  = in16[8];
    p->hop[1]  = in16[9];
    p->hop[2]  = in16[10];

    if (rnd != 0)
    {
        *rnd = Rf_GetU32LE(&in16[11]);
    }

    if ((p->channel > (uint8_t)RF_MAX_CHANNEL)
        || (p->hop[0] > (uint8_t)RF_MAX_CHANNEL)
        || (p->hop[1] > (uint8_t)RF_MAX_CHANNEL)
        || (p->hop[2] > (uint8_t)RF_MAX_CHANNEL))
    {
        return RC_E_PARAM;
    }

    return RC_OK;
}

/*==========================================================================
 * 帧组包 / 解包
 *========================================================================*/
void Kb_BuildPoll(uint8_t *out5, uint8_t seq, uint8_t host_ts_l,
                  uint8_t link_quality, uint8_t flags)
{
    if (out5 == 0)
    {
        return;
    }

    out5[0] = (uint8_t)KB_POLL;
    out5[1] = seq;
    out5[2] = host_ts_l;
    out5[3] = (link_quality > 100u) ? 100u : link_quality;
    out5[4] = flags;
}

uint8_t Kb_ParsePoll(const uint8_t *in5, uint8_t inlen, uint8_t *seq,
                     uint8_t *host_ts_l, uint8_t *link_quality, uint8_t *flags)
{
    if (in5 == 0)
    {
        return RC_E_PARAM;
    }

    /* 【修复 P2-3】长度先过关：POLL 是定长 5 字节，短帧一律拒收，
     * 绝不越过实际长度去读 in5[1..4]（原来无条件读，短帧/噪声会读出垃圾）。 */
    if (inlen < (uint8_t)KB_POLL_LEN)
    {
        return RC_E_PARAM;
    }

    if (FRAME_TYPE(in5[0]) != (uint8_t)KB_POLL)
    {
        return RC_E_PARAM;
    }

    if (seq != 0)          { *seq          = in5[1]; }
    if (host_ts_l != 0)    { *host_ts_l    = in5[2]; }
    if (link_quality != 0) { *link_quality = in5[3]; }
    if (flags != 0)        { *flags        = in5[4]; }

    return RC_OK;
}

uint8_t Kb_BuildHid6(uint8_t *out8, uint8_t seq, const hid_report_t *r)
{
    if ((out8 == 0) || (r == 0))
    {
        return 0u;
    }

    out8[0] = (uint8_t)KB_HID_6KRO;
    out8[1] = seq;
    out8[2] = r->modifiers;
    out8[3] = 0u;                           /* reserved */
    memcpy(&out8[4], r->keys, HID_6KRO_KEYS);

    return (uint8_t)KB_HID_6KRO_LEN;
}

uint8_t Kb_BuildHidNkro(uint8_t *out32, uint8_t seq, const hid_report_t *r)
{
    if ((out32 == 0) || (r == 0))
    {
        return 0u;
    }

    memset(out32, 0, KB_HID_NKRO_LEN);
    out32[0] = (uint8_t)KB_HID_NKRO;
    out32[1] = seq;
    out32[2] = r->modifiers;
    out32[3] = 0x01u;                       /* bit0 = 6kro_compat */
    memcpy(&out32[4], r->nkro, HID_NKRO_BYTES);

    return (uint8_t)KB_HID_NKRO_LEN;
}

uint8_t Kb_ParseHid(const uint8_t *in, uint8_t len, uint8_t *seq, hid_report_t *r)
{
    if ((in == 0) || (r == 0))
    {
        return RC_E_PARAM;
    }

    if (FRAME_TYPE(in[0]) == (uint8_t)KB_HID_6KRO)
    {
        if (len < (uint8_t)KB_HID_6KRO_LEN)
        {
            return RC_E_LEN;
        }

        if (seq != 0)
        {
            *seq = in[1];
        }

        r->modifiers = in[2];
        memcpy(r->keys, &in[4], HID_6KRO_KEYS);

        /* 6KRO 报文也顺便更新 NKRO 位图，方便上层统一处理 */
        {
            uint8_t i;
            for (i = 0; i < HID_6KRO_KEYS; i++)
            {
                if (r->keys[i] != 0u)
                {
                    Hid_NkroSet(r, r->keys[i], true);
                }
            }
        }

        return RC_OK;
    }

    if (FRAME_TYPE(in[0]) == (uint8_t)KB_HID_NKRO)
    {
        if (len < (uint8_t)KB_HID_NKRO_LEN)
        {
            return RC_E_LEN;
        }

        if (seq != 0)
        {
            *seq = in[1];
        }

        r->modifiers = in[2];
        memcpy(r->nkro, &in[4], HID_NKRO_BYTES);

        return RC_OK;
    }

    return RC_E_PARAM;
}

void Kb_BuildRetrieve(uint8_t *out2, uint8_t lost_seq)
{
    if (out2 == 0)
    {
        return;
    }
    out2[0] = (uint8_t)KB_RETRIEVE;
    out2[1] = lost_seq;
}

uint8_t Kb_ParseRetrieve(const uint8_t *in2, uint8_t *lost_seq)
{
    if (in2 == 0)
    {
        return RC_E_PARAM;
    }
    if (FRAME_TYPE(in2[0]) != (uint8_t)KB_RETRIEVE)
    {
        return RC_E_PARAM;
    }
    if (lost_seq != 0)
    {
        *lost_seq = in2[1];
    }
    return RC_OK;
}

void Kb_BuildLed(uint8_t *out2, uint8_t led)
{
    if (out2 == 0)
    {
        return;
    }
    out2[0] = (uint8_t)KB_LED;
    out2[1] = led;
}

uint8_t Kb_ParseLed(const uint8_t *in2, uint8_t *led)
{
    if (in2 == 0)
    {
        return RC_E_PARAM;
    }
    if (FRAME_TYPE(in2[0]) != (uint8_t)KB_LED)
    {
        return RC_E_PARAM;
    }
    if (led != 0)
    {
        *led = in2[1];
    }
    return RC_OK;
}

/*==========================================================================
 * 跳频（规范 4.4：每 32 帧切一次）
 *
 * 规范伪代码：kb_hop_tbl[(seq >> 5) & 0x03] —— 表只有 3 个元素，& 0x03
 * 会算出下标 3，数组越界。这里改成 % 3。
 *========================================================================*/
uint8_t Kb_HopChannelFor(uint8_t seq, const kb_pair_info_t *p)
{
    uint8_t idx;

    if (p == 0)
    {
        return (uint8_t)RF_RC_ENUM_CHANNEL;
    }

    idx = (uint8_t)((seq >> 5) % (uint8_t)HOP_KB_COUNT);
    return p->hop[idx];
}

/*==========================================================================
 * 主机
 *========================================================================*/
void Kb_HostInit(kb_host_t *h, const kb_pair_info_t *pair)
{
    if (h == 0)
    {
        return;
    }

    memset(h, 0, sizeof(*h));

    if (pair != 0)
    {
        h->pair = *pair;
    }
    else
    {
        Kb_PairDefault(&h->pair);
    }

    Link_StatInit(&h->stat, (h->pair.nkro_en) ? 1u : 1u);
    Hop_Init(&h->hop, 0x0Bu, (uint8_t)HOP_KB_COUNT, hop_kb_table);
    Hid_Clear(&h->last_report);
}

void Kb_HostTickMs(kb_host_t *h)
{
    if (h != 0)
    {
        h->host_ms++;
    }
}

/*********************************************************************
 * @fn      Kb_HostPoll
 * @brief   跑一个 1 ms 周期
 *
 * 补帧逻辑（规范 4.5 层 2）：
 *   * 正常每 1 ms 发一个 POLL(seq)，seq 每轮 +1
 *   * 某一轮没收到应答 → 记下 retrieve_seq = seq，seq 照样 +1
 *   * 连续丢满 4 帧 → 下一轮不发 POLL，改发 RETRIEVE(retrieve_seq)
 *   * 直到补回来才恢复发 POLL（期间 seq 不前进，免得越差越远）
 *********************************************************************/
int Kb_HostPoll(kb_host_t *h, hid_report_t *out)
{
    uint8_t tx[KB_POLL_LEN];
    uint8_t txlen;
    uint8_t expect;
    uint8_t rx[RF_PLOAD_MAX];
    uint8_t rlen = 0u;
    uint8_t rseq = 0u;
    uint8_t ch;
    int     rc;

    if (h == 0)
    {
        return HALRF_E_PARAM;
    }

    /* 1) 跳频：每 32 帧换一次 */
    ch = Kb_HopChannelFor(h->seq, &h->pair);
    HALRF_SetChannel(ch);

    /* 2) 这一轮发 POLL 还是 RETRIEVE */
    if (h->retrieving != 0u)
    {
        Kb_BuildRetrieve(tx, h->retrieve_seq);
        txlen  = (uint8_t)KB_RETRIEVE_LEN;
        expect = h->retrieve_seq;
    }
    else
    {
        uint8_t flags = (h->pair.nkro_en != 0) ? (uint8_t)KB_FLAG_REQ_NKRO : 0u;
        Kb_BuildPoll(tx, h->seq, h->host_ms, Link_Percent(&h->stat), flags);
        txlen  = (uint8_t)KB_POLL_LEN;
        expect = h->seq;
    }

    /* 3) 发 + 150 μs 内等应答 */
    rc = HALRF_TxRx(tx, txlen, rx, &rlen, RF_KB_RX_WINDOW_US);

    if (rc != HALRF_OK)
    {
        h->timeouts++;
        Link_OnTxFail(&h->stat);
        Link_UpdatePer(&h->stat);

        if (h->retrieving == 0u)
        {
            h->retrieve_seq = h->seq;               /* 记下这一帧丢了 */
            h->seq++;
            if (h->stat.miss_cnt >= 4u)
            {
                h->retrieving = 1u;                 /* 丢够了，下一轮开始补 */
            }
        }
        return rc;
    }

    /* 4) 应答必须是这一轮要的那一帧，否则当失败 */
    if ((Kb_ParseHid(rx, rlen, &rseq, &h->last_report) != RC_OK) || (rseq != expect))
    {
        h->timeouts++;
        Link_OnTxFail(&h->stat);
        Link_UpdatePer(&h->stat);
        return HALRF_E_FAIL;
    }

    /* 5) 成功 */
    Link_OnTxOk(&h->stat);
    Link_UpdatePer(&h->stat);

    if (h->retrieving != 0u)
    {
        h->retrieving = 0u;                         /* 补回来了 */
    }
    else
    {
        h->seq++;
        h->polls++;
    }

    if (out != 0)
    {
        *out = h->last_report;
    }

    return HALRF_OK;
}

bool Kb_HostIsRetrieving(const kb_host_t *h, uint8_t *retrieve_seq)
{
    if ((h == 0) || (h->retrieving == 0u))
    {
        return false;
    }

    if (retrieve_seq != 0)
    {
        *retrieve_seq = h->retrieve_seq;
    }
    return true;
}

/*==========================================================================
 * 从机
 *========================================================================*/
void Kb_DevInit(kb_dev_t *d)
{
    if (d == 0)
    {
        return;
    }

    memset(d, 0, sizeof(*d));
    Kb_PairDefault(&d->pair);
    d->pair.nkro_en = false;

    Link_StatInit(&d->stat, 1u);
    Hop_Init(&d->hop, 0x0Bu, (uint8_t)HOP_KB_COUNT, hop_kb_table);
    Hid_Clear(&d->report);
    KbCache_Init(&d->cache);

    d->state = KB_DEV_UNPAIRED;
}

void Kb_DevSetReport(kb_dev_t *d, const hid_report_t *r)
{
    if ((d == 0) || (r == 0))
    {
        return;
    }

    if (memcmp(&d->report, r, sizeof(*r)) != 0)
    {
        d->report = *r;
        d->dirty  = true;
    }
}

bool Kb_DevOnPoll(kb_dev_t *d, const uint8_t *in5, uint8_t inlen,
                  uint8_t *out32, uint8_t *outlen)
{
    uint8_t seq;
    uint8_t flags;
    uint8_t n;

    if ((d == 0) || (in5 == 0) || (out32 == 0) || (outlen == 0))
    {
        return false;
    }

    if (Kb_ParsePoll(in5, inlen, &seq, 0, 0, &flags) != RC_OK)
    {
        return false;
    }

    /* 主机在要 NKRO，从机跟着切 */
    if ((flags & (uint8_t)KB_FLAG_REQ_NKRO) != 0u)
    {
        d->pair.nkro_en = true;
    }

    if (d->pair.nkro_en != 0)
    {
        n = Kb_BuildHidNkro(out32, seq, &d->report);
    }
    else
    {
        n = Kb_BuildHid6(out32, seq, &d->report);
    }

    if (n == 0u)
    {
        return false;
    }

    /* 存一份缓存，等主机来 RETRIEVE（规范 4.5 层 2） */
    {
        uint8_t body[8];
        body[0] = d->report.modifiers;
        memcpy(&body[1], d->report.keys, HID_6KRO_KEYS);
        body[7] = 0u;
        KbCache_Put(&d->cache, seq, body, sizeof(body));
    }

    *outlen = n;
    d->last_poll_ms = 0u;                   /* 喂狗：5ms 没 POLL 才进低功耗 */
    d->miss         = 0u;
    d->dirty        = false;
    d->reports_sent++;
    Link_OnRxOk(&d->stat);

    if ((d->state == KB_DEV_LOWPOWER) || (d->state == KB_DEV_RECOVER))
    {
        d->state = KB_DEV_CONNECTED;        /* 主机又开始 POLL 了 */
    }
    else if (d->state == KB_DEV_UNPAIRED)
    {
        d->state = KB_DEV_CONNECTED;        /* 1:1 模式，收到 POLL 就算连上 */
    }

    return true;
}

bool Kb_DevOnRetrieve(kb_dev_t *d, const uint8_t *in2, uint8_t *out32, uint8_t *outlen)
{
    uint8_t lost;
    uint8_t cached_seq;

    if ((d == 0) || (in2 == 0) || (out32 == 0) || (outlen == 0))
    {
        return false;
    }

    if (Kb_ParseRetrieve(in2, &lost) != RC_OK)
    {
        return false;
    }

    if (!KbCache_Get(&d->cache, &cached_seq, 0, 0))
    {
        return false;                       /* 还没发过任何报告 */
    }
    (void)cached_seq;

    /* 用主机要的那一帧的序号重发（主机是按 seq 匹配的，不能换号） */
    if (d->pair.nkro_en != 0)
    {
        *outlen = Kb_BuildHidNkro(out32, lost, &d->report);
    }
    else
    {
        *outlen = Kb_BuildHid6(out32, lost, &d->report);
    }

    Link_OnRetry(&d->stat);
    return (*outlen != 0u);
}

void Kb_DevTickMs(kb_dev_t *d, uint8_t miss_limit)
{
    if (d == 0)
    {
        return;
    }

    d->last_poll_ms++;

    switch (d->state)
    {
        case KB_DEV_CONNECTED:
            /* 超过 5 ms 没 POLL → 低功耗（规范 12.1 的 LOW_POWER） */
            if (d->last_poll_ms > 5u)
            {
                d->state = KB_DEV_LOWPOWER;
            }
            break;

        case KB_DEV_LOWPOWER:
            /* 主机又开始 POLL 了（在 OnPoll 里会复位 last_poll_ms） */
            if (d->last_poll_ms <= 5u)
            {
                d->state = KB_DEV_CONNECTED;
            }
            break;

        case KB_DEV_RECOVER:
            if (Link_NeedReset(&d->stat, miss_limit))
            {
                d->state = KB_DEV_UNPAIRED;
            }
            break;

        default:
            break;
    }
}
