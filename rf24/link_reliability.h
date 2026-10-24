/**************************************************************************
 * link_reliability.h —— 可靠性统计 + 补帧 + 链路自适应
 *
 * 依据：规范 第 4.5 章（键盘双层可靠性）、第 8 章（可靠性与重传）、第 8.4 章
 *
 * 规范里的"层 1 硬件重传"在 nRF24 上是 Auto-ACK/Auto-Retry，
 * 到 CH592 上其实只有 LLE_MODE_AUTO（发完自动转收）。所以本模块把所有
 * 重传、去重、丢帧统计都放在软件层，硬件层只负责"发完能收到就收到"。
 **************************************************************************/

#ifndef __LINK_RELIABILITY_H__
#define __LINK_RELIABILITY_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 链路统计（规范 8.4）
 *========================================================================*/
typedef struct
{
    uint32_t tx_total;
    uint32_t tx_fail;
    uint32_t rx_total;
    uint32_t rx_crc_err;
    uint32_t retry_cnt;
    uint8_t  per_cent;          /* 0..100 */
    uint8_t  rate_cur;          /* 0 = 1Mbps, 1 = 2Mbps */
    uint8_t  miss_cnt;          /* 连续丢了几帧       */
    uint8_t  miss_max;          /* 历史最长连续丢帧   */
    uint8_t  last_seq;
    uint8_t  seq_valid;
    uint32_t dup_cnt;           /* 重传导致的重复帧   */
    uint32_t ooo_cnt;           /* 乱序帧             */
} link_stat_t;

void Link_StatInit(link_stat_t *s, uint8_t rate_cur);

void Link_OnTxOk(link_stat_t *s);
void Link_OnTxFail(link_stat_t *s);
void Link_OnRxOk(link_stat_t *s);
void Link_OnRxCrcErr(link_stat_t *s);
void Link_OnRetry(link_stat_t *s);

/* 重新算 PER（每次调用都会更新 s->per_cent） */
void Link_UpdatePer(link_stat_t *s);

/* 当前 PER（百分比整数，0..100） */
uint8_t Link_Percent(const link_stat_t *s);

/*********************************************************************
 * @fn      Link_AdaptiveStep
 * @brief   链路自适应：PER 高就降速，PER 低就升速（规范 8.4）
 * @return  新的速率（0=1M, 1=2M）；没有变化返回 0xFF
 *********************************************************************/
uint8_t Link_AdaptiveStep(link_stat_t *s);

/* 链路是不是该重建了（连续丢帧超过阈值） */
bool Link_NeedReset(const link_stat_t *s, uint8_t miss_limit);

/*==========================================================================
 * 2. 序列号去重 / 补帧（规范 4.5 层 2）
 *
 *   主机收到 HID 报告时，先看 seq：
 *     * 和期望的一样        → 正常
 *     * 和上一次一样        → 重传，丢掉
 *     * 跳号              → 中间丢了，记下来，发 KB_RETRIEVE 要求补
 *========================================================================*/
typedef enum
{
    SEQ_OK      = 0,    /* 正好是期望的下一帧   */
    SEQ_DUP     = 1,    /* 和上一帧重复（重传） */
    SEQ_OLD     = 2,    /* 更早的帧（乱序）     */
    SEQ_GAP     = 3     /* 跳号了，丢了几帧     */
} seq_result_t;

/*********************************************************************
 * @fn      Link_CheckSeq
 * @brief   检查一个收到的 seq
 * @param   s     统计（内部会更新 last_seq / dup / ooo / miss）
 * @param   seq   收到的序列号
 * @param   gap   输出：丢了几帧（SEQ_GAP 时有意义）
 * @return  seq_result_t
 * @note    8-bit 回绕按有符号差算，所以 255→0 不会被当成跳号
 *********************************************************************/
seq_result_t Link_CheckSeq(link_stat_t *s, uint8_t seq, uint8_t *gap);

/*==========================================================================
 * 3. 键盘模式补帧缓存（规范 4.5）
 *========================================================================*/
typedef struct
{
    uint8_t  last_seq;
    uint8_t  report[8];         /* 最近一次 6KRO 报告 */
    uint8_t  rlen;
    uint8_t  valid;
    uint32_t counter;           /* 单调递增，用来估算回报率 */
} kb_report_cache_t;

void    KbCache_Init(kb_report_cache_t *c);

/*********************************************************************
 * @fn      KbCache_Put
 * @brief   从机侧存一份最近报告，等主机来 RETRIEVE
 *********************************************************************/
void    KbCache_Put(kb_report_cache_t *c, uint8_t seq, const uint8_t *report, uint8_t len);

/*********************************************************************
 * @fn      KbCache_Get
 * @brief   主机/从机侧取缓存
 * @return  true = 有缓存
 *********************************************************************/
bool    KbCache_Get(const kb_report_cache_t *c, uint8_t *seq, uint8_t *report, uint8_t *len);

/*==========================================================================
 * 4. 重传参数（规范 8.2）
 *========================================================================*/
typedef struct
{
    uint8_t  hw_retry_max;      /* 硬件重试次数（CH592 用不上，留档） */
    uint16_t retry_gap_us;      /* 单次重试间隔 */
    uint8_t  sw_retry_max;      /* 软件重试上限 */
    uint8_t  link_reset_limit;  /* 链路重建阈值（连续丢几帧） */
    uint16_t watchdog_ms;       /* 看门狗 */
} link_param_t;

extern const link_param_t link_param_kb;    /* 键盘：3 / 300us / 10 帧 / 50ms  */
extern const link_param_t link_param_rc;    /* 遥控：2 / 400us / 3 超帧 / 500ms */

#ifdef __cplusplus
}
#endif

#endif /* __LINK_RELIABILITY_H__ */
