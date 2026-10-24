/**************************************************************************
 * link_tdma.h —— 遥控模式超帧 / 时隙调度
 *
 * 依据：规范 第 5.2 章（TDMA 超帧）、第 9.2 章（从机睡到自己的时隙）、第 16.5 章
 *
 *   超帧 16 ms，切成 20 个 0.8 ms 的时隙：
 *
 *     Slot 0   广播 SYNC（Host→All）
 *     Slot 1..16   dev_id=1..16 的 CMD + REPORT
 *     Slot 17   枚举时隙（接纳新上电的从机）
 *     Slot 18   大块数据 / 紧急 REPORT 抢占
 *     Slot 19   保留（跳频同步）
 *
 *   每 16 个超帧（256 ms）额外开一次枚举窗口，用来发现中途上电的设备。
 *
 * 本模块只管"什么时间该做哪个时隙"，不做任何射频 I/O ——
 * 收发在 proto_rc.c 里，这样 PC 上用虚拟时钟就能把时序完整测出来。
 **************************************************************************/

#ifndef __LINK_TDMA_H__
#define __LINK_TDMA_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 超帧参数（规范 5.2）
 *========================================================================*/
#define RC_SUPERFRAME_US        16000u      /* 16 ms                      */
#define RC_SUPERFRAME_MS        16u
#define RC_SLOT_US              800u        /* 0.8 ms                     */
#define RC_SLOT_COUNT           20u

#define RC_SLOT_SYNC            0u
#define RC_SLOT_DEV_FIRST       1u
#define RC_SLOT_DEV_LAST        16u
#define RC_SLOT_ENUM            17u
#define RC_SLOT_BULK            18u
#define RC_SLOT_RSV             19u
#define RC_SLOT_END             0xFFu       /* 超帧走完了                 */

#define RC_ENUM_EVERY_SF        16u         /* 每 16 超帧插一次全枚举     */
#define RC_SYNC_LOST_LIMIT      3u          /* 从机 3 超帧收不到 SYNC     */
#define RC_DEV_SCAN_MS          256u        /* 从机失步扫描超时           */
#define RC_HOST_RESYNC_MS       500u        /* 主机看门狗（规范 18.4）    */

/*==========================================================================
 * 调度器状态
 *========================================================================*/
typedef struct
{
    uint16_t sf_id;             /* 本超帧编号（0..65535 循环）      */
    uint8_t  slot;              /* 当前时隙 0..19                   */
    uint8_t  overflow;          /* 本超帧里时隙超时的次数           */
    uint16_t slot_bitmap;       /* bit0..15 = dev_id 1..16 是否在线 */
    uint32_t sf_start_us;       /* 本超帧起点（HALRF_Micros）       */
    uint32_t slot_start_us;     /* 本时隙起点                       */
    uint32_t sf_total;          /* 一共跑了多少超帧（不截断）       */
} tdma_t;

/*==========================================================================
 * 主机侧
 *========================================================================*/
void Tdma_Init(tdma_t *t);

/*********************************************************************
 * @fn      Tdma_BeginSuperframe
 * @brief   开一个新超帧，回到 Slot 0
 *********************************************************************/
void Tdma_BeginSuperframe(tdma_t *t, uint32_t now_us);

/*********************************************************************
 * @fn      Tdma_NextSlot
 * @brief   推进到下一个时隙
 * @param   now_us  当前时间
 * @return  新的时隙号；超帧走完返回 RC_SLOT_END
 * @note    如果上一个时隙已经超时，会记一次 overflow 并把起点对齐到
 *          本来的时间轴上，不让误差一直累积。
 *********************************************************************/
uint8_t Tdma_NextSlot(tdma_t *t, uint32_t now_us);

/* 当前时隙 */
uint8_t Tdma_CurrentSlot(const tdma_t *t);

/* 本时隙的绝对截止时间（起点 + 0.8ms） */
uint32_t Tdma_SlotDeadlineUs(const tdma_t *t);

/*********************************************************************
 * @fn      Tdma_WaitSlotEnd
 * @brief   在本时隙剩下的时间里空转（调用 HALRF_DelayUs）
 * @param   now_us  当前时间
 *********************************************************************/
void Tdma_WaitSlotEnd(const tdma_t *t, uint32_t now_us);

/* 本超帧还该开枚举窗口吗（Slot 17 或者每 16 超帧） */
bool Tdma_EnumWindow(const tdma_t *t);

/*==========================================================================
 * 从机位图 / 槽位分配
 *========================================================================*/
void Tdma_SetActive(tdma_t *t, uint8_t dev_id, bool on);
bool Tdma_IsActive(const tdma_t *t, uint8_t dev_id);
uint8_t Tdma_ActiveCount(const tdma_t *t);

/*********************************************************************
 * @fn      Tdma_AllocSlot
 * @brief   给新枚举上来的从机分一个 dev_id
 * @return  1..16；0 表示没空位了（对应错误码 RC_E_FULL）
 *********************************************************************/
uint8_t Tdma_AllocSlot(tdma_t *t);

/* 超帧号 → 毫秒（用来算 256ms / 500ms 的看门狗） */
static inline uint32_t Tdma_SfToMs(uint16_t sf_id)
{
    return (uint32_t)sf_id * (uint32_t)RC_SUPERFRAME_MS;
}

/*==========================================================================
 * 从机侧：算"睡到自己的时隙要睡多久"
 *========================================================================*/

/*********************************************************************
 * @fn      Tdma_SlotOffsetUs
 * @brief   某个时隙相对本超帧起点的偏移（微秒）
 *********************************************************************/
static inline uint32_t Tdma_SlotOffsetUs(uint8_t slot)
{
    return (uint32_t)slot * (uint32_t)RC_SLOT_US;
}

/*********************************************************************
 * @fn      Tdma_DevSleepUs
 * @brief   从机算：从现在起还要睡多久才轮到自己
 * @param   dev_id      自己的编号 1..16
 * @param   sf_start_us 本超帧起点（收到 SYNC 的那一刻）
 * @param   now_us      现在
 * @return  要睡的微秒数；已经过点了返回 0
 * @note    规范 9.2 的伪代码是 sleep_us = slots_until_me * 800，
 *          这里按"本超帧内的绝对偏移"算，避免了取模带来的边界问题。
 *********************************************************************/
uint32_t Tdma_DevSleepUs(uint8_t dev_id, uint32_t sf_start_us, uint32_t now_us);

/*********************************************************************
 * @fn      Tdma_FrameOk
 * @brief   主机侧：这一超帧里的收发有没有超时
 * @return  true = 有超时，调度器已经做了对齐
 *********************************************************************/
bool Tdma_HadOverflow(const tdma_t *t);

#ifdef __cplusplus
}
#endif

#endif /* __LINK_TDMA_H__ */
