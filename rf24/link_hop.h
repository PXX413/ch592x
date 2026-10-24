/**************************************************************************
 * link_hop.h —— 跳频（LFSR 序列 + 频点表 + 劣化频点屏蔽）
 *
 * 依据：规范 第 7 章
 *   * 键盘模式 3 频点 {20, 35, 50}，每 32 帧切一次
 *   * 遥控模式 8 频点 {5, 12, 19, 26, 33, 40, 47, 54}，每超帧切一次
 *   * 5-bit Galois LFSR，多项式 x^5 + x^3 + 1
 *   * 连续 5 次 RSSI < -85 dBm → 把该频点拉黑
 **************************************************************************/

#ifndef __LINK_HOP_H__
#define __LINK_HOP_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 频点表（规范 7.1 / 附录 A）
 * CH = (f_MHz - 2400) / 2
 *========================================================================*/
#define HOP_KB_COUNT            3u
#define HOP_RC_COUNT            8u

extern const uint8_t hop_kb_table[HOP_KB_COUNT];    /* {20, 35, 50}                 */
extern const uint8_t hop_rc_table[HOP_RC_COUNT];    /* {5,12,19,26,33,40,47,54}     */

/* 质量门限（规范 7.3） */
#define HOP_RSSI_BAD            (-85)   /* dBm，低于这个算劣化      */
#define HOP_BAD_CNT_TH          5u      /* 连续几次算坏             */
#define HOP_BLACKLIST_MAX       3u      /* 拉黑 3 个 → 触发重枚举   */

/*==========================================================================
 * 跳频状态
 *========================================================================*/
typedef struct
{
    uint8_t  seed;                          /* LFSR 当前状态（1..31）       */
    uint8_t  step;                          /* 每次跳频抽稀几步（见 .c 注释）*/
    uint8_t  table[HOP_RC_COUNT];           /* 频点表副本（键盘模式只用前 3）*/
    uint8_t  count;                         /* 表里几个频点                 */
    uint8_t  pos;                           /* 当前在表里的下标             */
    uint8_t  blacklist;                     /* bit[i]=1 表示表里第 i 个被屏蔽*/
    uint8_t  bad_cnt[HOP_RC_COUNT];         /* 每个频点的连续劣化计数        */
    uint32_t tick;                          /* 跳了几次                     */
} hop_ctx_t;

/*==========================================================================
 * LFSR（规范 7.2）
 *========================================================================*/

/*********************************************************************
 * @fn      Hop_LfsrNext
 * @brief   5-bit Galois LFSR 走一步（周期 31）
 * @param   state  当前状态（0 会被换成种子）
 * @return  下一个状态，范围 1..31
 *********************************************************************/
uint8_t Hop_LfsrNext(uint8_t state);

/*********************************************************************
 * @fn      Hop_Init
 * @brief   初始化跳频器
 * @param   h      状态
 * @param   seed   LFSR 种子（双方必须一致，从 Flash 的 pair_seed 来）
 * @param   count  频点数（3 或 8）
 * @param   table  频点表（0 则用默认表）
 *********************************************************************/
void Hop_Init(hop_ctx_t *h, uint8_t seed, uint8_t count, const uint8_t *table);

/*********************************************************************
 * @fn      Hop_Next
 * @brief   跳到下一个可用频点
 * @return  频点（CH 号）
 * @note    会跳过被拉黑的频点；全黑时返回 -1 表示的 0xFF
 *********************************************************************/
uint8_t Hop_Next(hop_ctx_t *h);

/* 当前频点 */
uint8_t Hop_Current(const hop_ctx_t *h);

/* 直接把表里第 idx 个设成当前（用于失步后按 SYNC 给的索引对齐） */
void Hop_SetIndex(hop_ctx_t *h, uint8_t idx);

/*********************************************************************
 * @fn      Hop_SyncTo
 * @brief   从机按 SYNC 帧硬对齐跳频状态
 * @param   idx   SYNC.hop_channel（频点表下标）
 * @param   seed  SYNC.seed_lfsr（LFSR 当前值，0 = 只对齐下标）
 *********************************************************************/
void Hop_SyncTo(hop_ctx_t *h, uint8_t idx, uint8_t seed);

/* 取当前 LFSR 状态（主机填 SYNC.seed_lfsr 用） */
uint8_t Hop_Seed(const hop_ctx_t *h);

/*==========================================================================
 * 劣化频点屏蔽（规范 7.3）
 *========================================================================*/

/*********************************************************************
 * @fn      Hop_QualityUpdate
 * @brief   用一次收到的 RSSI 更新频点质量
 * @param   h    状态
 * @param   ch   这次用的频点
 * @param   rssi 收到的信号强度 dBm
 * @return  true = 新增了一个被拉黑的频点
 *********************************************************************/
bool Hop_QualityUpdate(hop_ctx_t *h, uint8_t ch, int8_t rssi);

/* 主动拉黑 / 解除 */
void Hop_BlacklistAdd(hop_ctx_t *h, uint8_t ch);
void Hop_BlacklistClear(hop_ctx_t *h);

bool    Hop_IsBlacklisted(const hop_ctx_t *h, uint8_t ch);
uint8_t Hop_BlacklistCount(const hop_ctx_t *h);

/* 黑名单满了（>=3）→ 上层应该触发全量重枚举 */
bool Hop_NeedResync(const hop_ctx_t *h);

/* 把某个频点在表里的下标找出来，找不到返回 0xFF */
uint8_t Hop_Index(const hop_ctx_t *h, uint8_t ch);

#ifdef __cplusplus
}
#endif

#endif /* __LINK_HOP_H__ */
