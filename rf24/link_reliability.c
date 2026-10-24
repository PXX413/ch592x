/**************************************************************************
 * link_reliability.c —— 可靠性统计 / 补帧 / 链路自适应 实现
 **************************************************************************/

#include "link_reliability.h"
#include <string.h>

/*==========================================================================
 * 重传参数表（规范 8.2）
 *========================================================================*/
const link_param_t link_param_kb =
{
    /* hw_retry_max    */ 3u,
    /* retry_gap_us    */ 300u,
    /* sw_retry_max    */ 10u,
    /* link_reset_limit*/ 10u,
    /* watchdog_ms     */ 50u
};

const link_param_t link_param_rc =
{
    /* hw_retry_max    */ 2u,
    /* retry_gap_us    */ 400u,
    /* sw_retry_max    */ 3u,
    /* link_reset_limit*/ 3u,
    /* watchdog_ms     */ 500u
};

/*==========================================================================
 * 统计
 *========================================================================*/
void Link_StatInit(link_stat_t *s, uint8_t rate_cur)
{
    if (s == 0)
    {
        return;
    }

    memset(s, 0, sizeof(*s));
    s->rate_cur = (rate_cur != 0u) ? 1u : 0u;
}

void Link_OnTxOk(link_stat_t *s)
{
    if (s != 0)
    {
        s->tx_total++;
        s->miss_cnt = 0u;
    }
}

void Link_OnTxFail(link_stat_t *s)
{
    if (s == 0)
    {
        return;
    }

    s->tx_total++;
    s->tx_fail++;

    if (s->miss_cnt < 0xFFu)
    {
        s->miss_cnt++;
    }
    if (s->miss_cnt > s->miss_max)
    {
        s->miss_max = s->miss_cnt;
    }
}

void Link_OnRxOk(link_stat_t *s)
{
    if (s != 0)
    {
        s->rx_total++;
        s->miss_cnt = 0u;
    }
}

void Link_OnRxCrcErr(link_stat_t *s)
{
    if (s != 0)
    {
        s->rx_crc_err++;
    }
}

void Link_OnRetry(link_stat_t *s)
{
    if (s != 0)
    {
        s->retry_cnt++;
    }
}

void Link_UpdatePer(link_stat_t *s)
{
    uint32_t total;

    if (s == 0)
    {
        return;
    }

    total = s->tx_total;
    if (total == 0u)
    {
        s->per_cent = 0u;
        return;
    }

    s->per_cent = (uint8_t)((s->tx_fail * 100u) / total);
    if (s->per_cent > 100u)
    {
        s->per_cent = 100u;
    }
}

uint8_t Link_Percent(const link_stat_t *s)
{
    return (s != 0) ? s->per_cent : 0u;
}

/*********************************************************************
 * @fn      Link_AdaptiveStep
 * @brief   规范 8.4：PER > 15% 降速，PER < 3% 升速
 * @note    规范里是"和当前速率比"，这里加了"统计样本够不够"的判断：
 *          总共还没发够 32 帧就不动速率，免得刚开机就被一两次失败带偏。
 *========================================================================*/
uint8_t Link_AdaptiveStep(link_stat_t *s)
{
    if (s == 0)
    {
        return 0xFFu;
    }

    Link_UpdatePer(s);

    if (s->tx_total < 32u)
    {
        return 0xFFu;                       /* 样本太少，先不调 */
    }

    if ((s->per_cent > 15u) && (s->rate_cur == 1u))
    {
        s->rate_cur = 0u;
        s->tx_total = 0u;                   /* 换了速率，统计重新来 */
        s->tx_fail  = 0u;
        return 0u;
    }

    if ((s->per_cent < 3u) && (s->rate_cur == 0u))
    {
        s->rate_cur = 1u;
        s->tx_total = 0u;
        s->tx_fail  = 0u;
        return 1u;
    }

    return 0xFFu;
}

bool Link_NeedReset(const link_stat_t *s, uint8_t miss_limit)
{
    if (s == 0)
    {
        return false;
    }
    return (s->miss_cnt >= miss_limit);
}

/*==========================================================================
 * 序列号检查
 *========================================================================*/
seq_result_t Link_CheckSeq(link_stat_t *s, uint8_t seq, uint8_t *gap)
{
    uint8_t  diff;
    uint8_t  expect;

    if (gap != 0)
    {
        *gap = 0u;
    }

    if (s == 0)
    {
        return SEQ_OK;
    }

    if (s->seq_valid == 0u)
    {
        s->seq_valid = 1u;
        s->last_seq  = seq;
        return SEQ_OK;
    }

    if (seq == s->last_seq)
    {
        s->dup_cnt++;
        return SEQ_DUP;
    }

    expect = (uint8_t)(s->last_seq + 1u);

    /* 用无符号差算"往前走了几格"，天然处理 8-bit 回绕 */
    diff = (uint8_t)(seq - expect);

    if (seq == expect)
    {
        s->last_seq = seq;
        return SEQ_OK;
    }

    if (diff <= 127u)
    {
        /* 往前跳了 diff 格 → 中间丢了 diff 帧 */
        if (gap != 0)
        {
            *gap = diff;
        }
        if (s->miss_cnt < 0xFFu)
        {
            s->miss_cnt = (uint8_t)(s->miss_cnt + diff);
        }
        if (s->miss_cnt > s->miss_max)
        {
            s->miss_max = s->miss_cnt;
        }
        s->last_seq = seq;
        return SEQ_GAP;
    }

    /* 落在后面 → 迟到/乱序 */
    s->ooo_cnt++;
    return SEQ_OLD;
}

/*==========================================================================
 * 键盘报告缓存
 *========================================================================*/
void KbCache_Init(kb_report_cache_t *c)
{
    if (c != 0)
    {
        memset(c, 0, sizeof(*c));
    }
}

void KbCache_Put(kb_report_cache_t *c, uint8_t seq, const uint8_t *report, uint8_t len)
{
    if ((c == 0) || (report == 0))
    {
        return;
    }

    if (len > (uint8_t)sizeof(c->report))
    {
        len = (uint8_t)sizeof(c->report);
    }

    c->last_seq = seq;
    c->rlen     = len;
    c->valid    = 1u;
    c->counter++;
    memcpy(c->report, report, len);
}

bool KbCache_Get(const kb_report_cache_t *c, uint8_t *seq, uint8_t *report, uint8_t *len)
{
    if ((c == 0) || (c->valid == 0u))
    {
        return false;
    }

    if (seq != 0)
    {
        *seq = c->last_seq;
    }
    if (report != 0)
    {
        memcpy(report, c->report, c->rlen);
    }
    if (len != 0)
    {
        *len = c->rlen;
    }

    return true;
}
