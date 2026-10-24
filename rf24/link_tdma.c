/**************************************************************************
 * link_tdma.c —— 超帧 / 时隙调度实现
 **************************************************************************/

#include "link_tdma.h"
#include "hal_rf.h"

/*==========================================================================
 * 初始化
 *========================================================================*/
void Tdma_Init(tdma_t *t)
{
    if (t == 0)
    {
        return;
    }

    t->sf_id         = 0u;
    t->slot          = RC_SLOT_SYNC;
    t->overflow      = 0u;
    t->slot_bitmap   = 0u;
    t->sf_start_us   = 0u;
    t->slot_start_us = 0u;
    t->sf_total      = 0u;
}

void Tdma_BeginSuperframe(tdma_t *t, uint32_t now_us)
{
    if (t == 0)
    {
        return;
    }

    t->sf_start_us   = now_us;
    t->slot_start_us = now_us;
    t->slot          = RC_SLOT_SYNC;
    t->overflow      = 0u;
    t->sf_total++;
}

uint8_t Tdma_CurrentSlot(const tdma_t *t)
{
    return (t != 0) ? t->slot : RC_SLOT_END;
}

uint32_t Tdma_SlotDeadlineUs(const tdma_t *t)
{
    if (t == 0)
    {
        return 0u;
    }
    return t->slot_start_us + (uint32_t)RC_SLOT_US;
}

bool Tdma_HadOverflow(const tdma_t *t)
{
    return (t != 0) && (t->overflow != 0u);
}

/*==========================================================================
 * 时隙推进
 *
 * 时间轴是"挂在超帧起点上的网格"：第 n 个时隙永远从 sf_start + n*800us
 * 开始。这样一个时隙偶尔超时（比如射频重试），后面的时隙也不会被推着走，
 * 精度不会累积漂移；超时本身记在 overflow 里给上层看。
 *========================================================================*/
uint8_t Tdma_NextSlot(tdma_t *t, uint32_t now_us)
{
    uint32_t grid;

    if (t == 0)
    {
        return RC_SLOT_END;
    }

    if (t->slot == RC_SLOT_END)
    {
        return RC_SLOT_END;
    }

    if (now_us > Tdma_SlotDeadlineUs(t))
    {
        if (t->overflow < 0xFFu)
        {
            t->overflow++;
        }
    }

    if (t->slot >= (uint8_t)(RC_SLOT_COUNT - 1u))
    {
        t->slot = RC_SLOT_END;
        return RC_SLOT_END;
    }

    t->slot++;

    /* 对齐到网格 */
    grid = t->sf_start_us + Tdma_SlotOffsetUs(t->slot);

    if (grid > now_us)
    {
        HALRF_DelayUs(grid - now_us);
        t->slot_start_us = grid;
    }
    else
    {
        t->slot_start_us = now_us;          /* 已经晚了，就从现在算 */
    }

    return t->slot;
}

void Tdma_WaitSlotEnd(const tdma_t *t, uint32_t now_us)
{
    uint32_t deadline;

    if (t == 0)
    {
        return;
    }

    deadline = Tdma_SlotDeadlineUs(t);

    if (deadline > now_us)
    {
        HALRF_DelayUs(deadline - now_us);
    }
}

/*==========================================================================
 * 枚举窗口
 *========================================================================*/
bool Tdma_EnumWindow(const tdma_t *t)
{
    if (t == 0)
    {
        return false;
    }

    /* Slot 17 是常规枚举时隙；另外每 16 个超帧插一次全量扫描 */
    if (t->slot == RC_SLOT_ENUM)
    {
        return true;
    }

    return ((t->sf_id % (uint16_t)RC_ENUM_EVERY_SF) == 0u);
}

/*==========================================================================
 * 槽位
 *========================================================================*/
void Tdma_SetActive(tdma_t *t, uint8_t dev_id, bool on)
{
    uint16_t bit;

    if ((t == 0) || (dev_id < 1u) || (dev_id > (uint8_t)RF_RC_MAX_DEV))
    {
        return;
    }

    bit = (uint16_t)(1u << (dev_id - 1u));

    if (on)
    {
        t->slot_bitmap |= bit;
    }
    else
    {
        t->slot_bitmap &= (uint16_t)(~bit);
    }
}

bool Tdma_IsActive(const tdma_t *t, uint8_t dev_id)
{
    if ((t == 0) || (dev_id < 1u) || (dev_id > (uint8_t)RF_RC_MAX_DEV))
    {
        return false;
    }
    return ((t->slot_bitmap & (uint16_t)(1u << (dev_id - 1u))) != 0u);
}

uint8_t Tdma_ActiveCount(const tdma_t *t)
{
    uint8_t  i;
    uint8_t  n = 0u;

    if (t == 0)
    {
        return 0u;
    }

    for (i = 1u; i <= (uint8_t)RF_RC_MAX_DEV; i++)
    {
        if (Tdma_IsActive(t, i))
        {
            n++;
        }
    }
    return n;
}

uint8_t Tdma_AllocSlot(tdma_t *t)
{
    uint8_t i;

    if (t == 0)
    {
        return 0u;
    }

    for (i = 1u; i <= (uint8_t)RF_RC_MAX_DEV; i++)
    {
        if (!Tdma_IsActive(t, i))
        {
            Tdma_SetActive(t, i, true);
            return i;
        }
    }

    return 0u;                              /* 满了 */
}

/*==========================================================================
 * 从机侧：睡到自己的时隙
 *========================================================================*/
uint32_t Tdma_DevSleepUs(uint8_t dev_id, uint32_t sf_start_us, uint32_t now_us)
{
    uint32_t due;
    uint32_t elapsed;

    if ((dev_id < 1u) || (dev_id > (uint8_t)RF_RC_MAX_DEV))
    {
        return 0u;
    }

    due     = sf_start_us + Tdma_SlotOffsetUs(dev_id);
    elapsed = now_us - sf_start_us;

    if (elapsed >= Tdma_SlotOffsetUs(dev_id))
    {
        return 0u;                          /* 已经过点了，别再睡 */
    }

    return (due - now_us);
}
