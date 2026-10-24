/**************************************************************************
 * hal_rf_ch592.c —— hal_rf.h 的 CH592 真机后端
 *
 * 把规范里那套 nRF24 风格的寄存器操作，落到 CH59x BLE 库真正给的
 * RF PHY 接口上：
 *
 *     RF_Config(rfConfig_t*)      配 accessAddress / CRCInit / 信道 / 速率
 *     RF_SetFrequency(freq, ch)   快速换信道（不用重写 accessAddress）
 *     RF_Tx(buf, len, a, b)       发一帧
 *     RF_Rx(buf, len, a, b)       开收（auto 模式下会被 TX 自动带起来）
 *     RF_Shut()                   回 idle
 *
 * 收包缓冲格式（LLE_MODE 的 BIT7 = 0 时）：
 *     rxBuf[0] = RSSI
 *     rxBuf[1] = 长度
 *     rxBuf[2..] = 有效载荷
 *
 * 注意：等待函数必须在 RAM 里跑（__HIGH_CODE），否则发/收期间取指会
 * 把射频时序带歪 —— 这是 WCH 官方 RF_PHY 例程里特别强调的。
 **************************************************************************/

#include "hal_rf.h"
#include "CH59x_common.h"
#include "CH59xBLE_LIB.h"

/*==========================================================================
 * 全局上下文（hal_rf.h 里 extern 了）
 *========================================================================*/
halrf_ctx_t g_halrf;

/*==========================================================================
 * 内部状态
 *========================================================================*/
/* SDK 射频配置（这是 CH59x BLE 库自己的 rfConfig_t，和 hal_rf.h 里的
 * rf_config_t 差一个下划线；本文件同时包含两个头，不会撞） */
static rfConfig_t s_cfg;
static halrf_cb_t  s_cb;

/* 前向声明：halrf_ch592_apply() 在任何配置之前会兜底填这个回调。 */
void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf);
static uint8_t     s_channel   = 20u;
static rf_rate_e   s_rate      = RF_RATE_1M;
static int8_t      s_rssi      = 0;
static uint8_t     s_inited    = 0u;

/* ---- 【P0 修复】射频归属 --------------------------------------------------
 * s_bt_owns_phy = 1 表示 BLE 协议栈独占射频，本模块**只写软件缓存、不碰 PHY**
 * （完整说明见 hal_rf.h）。CH592 只有一套射频前端：2.4G 驱动在协议栈建链后
 * 再调 RF_Config()，会把 LLE 赖以工作的 accessAddress / 频点 / CRC /
 * rfStatusCB 整体改写 —— 这正是"广播状态显示正常、主机却永远扫不到"的根因。
 * ------------------------------------------------------------------------ */
static uint8_t     s_bt_owns_phy = 0u;

/* 当前链路 = 完整 5 字节地址 + 信道，用来判断 HALRF_SetLink() 是否真需要重配。
 * 原来只拿 addr[0] 当指纹：地址首字节恰为 0xFF 时永远判成"没变"（换了地址也不
 * 重配），而 s_inited 又被 HALRF_PowerDown() 清零，导致每次 PowerDown→PowerUp
 * （切页、休眠唤醒都会走到）都白重配一次射频 —— 两个方向都不对。 */
static uint8_t     s_link_addr[RF_ADDR_LEN];
static uint8_t     s_link_ch    = 0u;
static uint8_t     s_link_valid = 0u;

/* 收包时把数据搬到 g_halrf.buf，避免回调里直接暴露射频缓冲 */
#define CH592_RX_HDR_LEN    2u

/* 【修复 P2-1】接收原始缓冲必须装得下 SDK 允许的最大帧：
 * RxMaxlen = RF_PLOAD_MAX + CH592_RX_HDR_LEN + 4（当前 32+2+4 = 38）。
 * 原来把只有 RF_PLOAD_MAX(32) 字节的 g_halrf.buf 直接交给 RF_Rx()，
 * SDK 最多会写 38 字节 —— 越界 6 字节，正好踩到 g_halrf 里紧跟其后的
 * len/rssi/busy/done 字段。 */
#define RX_RAW_MAX   ((uint8_t)(RF_PLOAD_MAX + CH592_RX_HDR_LEN + 4u))
static uint8_t     s_rx_raw[RX_RAW_MAX];

/*==========================================================================
 * 频率换算
 *   CH = (f_MHz - 2400) / 2   →   f_kHz = 2400000 + CH * 2000
 *========================================================================*/
static uint32_t ch_to_freq(uint8_t ch)
{
    return 2400000u + ((uint32_t)ch * 2000u);
}

/*********************************************************************
 * @fn      halrf_ch592_apply
 * @brief   把缓存的配置真正写进射频（比较贵，只在换 accessAddress 时调）
 *********************************************************************/
static void halrf_ch592_apply(void)
{
    bStatus_t st;

    /* 【P0 修复】射频归 BLE 时绝不动硬件：只保留软件缓存，
     * 等 HALRF_BtOwnsPhy(false) 交还时再一次性装回去。 */
    if (s_bt_owns_phy != 0u)
    {
        return;
    }

    /* 【2026-10-19 防御】正常路径一定先走 HALRF_Init()，由它填好回调、
     * RxMaxlen / TxMaxlen。但若某个新页面在 HALRF_Init() 之前就调了
     * HALRF_PowerUp()，s_cfg 里这些字段全是 0；RF 中断随后会通过
     * rfStatusCB 回调，空指针会直接跑飞。这里补上安全默认值，让
     * "配置没准备好"退化成收发失败，而不是死机。 */
    if (s_cfg.rfStatusCB == 0)
    {
        s_cfg.rfStatusCB = RF_2G4StatusCallBack;
    }
    if (s_cfg.RxMaxlen == 0u)
    {
        s_cfg.RxMaxlen = (uint8_t)(RF_PLOAD_MAX + CH592_RX_HDR_LEN + 4u);
    }
    if (s_cfg.TxMaxlen == 0u)
    {
        s_cfg.TxMaxlen = (uint8_t)(RF_PLOAD_MAX + 4u);
    }
    if (s_cfg.CRCInit == 0u)
    {
        s_cfg.CRCInit = (uint32_t)RF_CRC16_INIT;
    }

    s_cfg.LLEMode = (uint8_t)((s_cfg.LLEMode & 0xF0u) | (uint8_t)LLE_MODE_AUTO);

    if (s_rate == RF_RATE_2M)
    {
        s_cfg.LLEMode |= (uint8_t)LLE_MODE_PHY_2M;
    }
    else
    {
        s_cfg.LLEMode |= (uint8_t)LLE_MODE_PHY_1M;
    }

    /* 用 raw frequency（BIT6=1），这样 CH 号能直接按规范算 */
    s_cfg.LLEMode |= (uint8_t)LLE_MODE_EX_CHANNEL;

    s_cfg.Channel   = s_channel;
    s_cfg.Frequency = ch_to_freq(s_channel);

    st = RF_Config(&s_cfg);
    (void)st;
}

/*==========================================================================
 * 射频状态回调（中断上下文）
 *
 * 规矩和 WCH 例程一样：回调里**只置标志、拷数据**，不要在回调里
 * 再调 RF_Tx/RF_Rx。
 *========================================================================*/
void RF_2G4StatusCallBack(uint8_t sta, uint8_t crc, uint8_t *rxBuf)
{
    switch (sta)
    {
        case TX_MODE_TX_FINISH:
            g_halrf.ok   = 1u;
            g_halrf.done = 1u;
                g_halrf.busy = 0u;          /* 【修复 P2-2】一次收发到此结束 */
            break;

        case TX_MODE_RX_DATA:
            /* auto 模式：发完之后收到了对方的应答 */
            if (crc == 0u)
            {
                /* 【修复 P2-1】载荷长度双重钳位：既要装进 g_halrf.buf，
                 * 也不能超出原始缓冲 s_rx_raw 的载荷容量。 */
                uint8_t n = rxBuf[1];
                if (n > (uint8_t)RF_PLOAD_MAX)
                {
                    n = (uint8_t)RF_PLOAD_MAX;
                }
                if (n > (uint8_t)(RX_RAW_MAX - CH592_RX_HDR_LEN))
                {
                    n = (uint8_t)(RX_RAW_MAX - CH592_RX_HDR_LEN);
                }
                g_halrf.rssi = (int8_t)rxBuf[0];
                s_rssi       = (int8_t)rxBuf[0];
                g_halrf.len  = n;
                {
                    uint8_t i;
                    for (i = 0u; i < n; i++)
                    {
                        g_halrf.buf[i] = rxBuf[i + CH592_RX_HDR_LEN];
                    }
                }
                g_halrf.ok = 1u;
            }
            else
            {
                g_halrf.ok = 0u;
            }
            g_halrf.done = 1u;
                g_halrf.busy = 0u;          /* 【修复 P2-2】一次收发到此结束 */
            break;

        case TX_MODE_RX_TIMEOUT:
            g_halrf.ok   = 0u;              /* 窗口内没人应答 */
            g_halrf.len  = 0u;
            g_halrf.done = 1u;
                g_halrf.busy = 0u;          /* 【修复 P2-2】一次收发到此结束 */
            break;

        case TX_MODE_TX_FAIL:
            g_halrf.ok   = 0u;
            g_halrf.done = 1u;
                g_halrf.busy = 0u;          /* 【修复 P2-2】一次收发到此结束 */
            break;

        case RX_MODE_RX_DATA:
            if (crc == 0u)
            {
                /* 【修复 P2-1】载荷长度双重钳位：既要装进 g_halrf.buf，
                 * 也不能超出原始缓冲 s_rx_raw 的载荷容量。 */
                uint8_t n = rxBuf[1];
                if (n > (uint8_t)RF_PLOAD_MAX)
                {
                    n = (uint8_t)RF_PLOAD_MAX;
                }
                if (n > (uint8_t)(RX_RAW_MAX - CH592_RX_HDR_LEN))
                {
                    n = (uint8_t)(RX_RAW_MAX - CH592_RX_HDR_LEN);
                }
                g_halrf.rssi = (int8_t)rxBuf[0];
                s_rssi       = (int8_t)rxBuf[0];
                g_halrf.len  = n;
                {
                    uint8_t i;
                    for (i = 0u; i < n; i++)
                    {
                        g_halrf.buf[i] = rxBuf[i + CH592_RX_HDR_LEN];
                    }
                }
                g_halrf.ok = 1u;
            }
            else
            {
                g_halrf.ok = 0u;
            }
            g_halrf.done = 1u;
                g_halrf.busy = 0u;          /* 【修复 P2-2】一次收发到此结束 */
            break;

        default:
            break;
    }

    /* RX_MODE 下的回包（auto rx 时我们回过 ACK） */
    if (s_cb != 0)
    {
        halrf_evt_t e;

        if (g_halrf.ok != 0u)
        {
            e = (sta == RX_MODE_TX_FINISH) ? HALRF_EVT_TX_DONE : HALRF_EVT_RX_DATA;
        }
        else
        {
            e = (sta == TX_MODE_RX_TIMEOUT) ? HALRF_EVT_RX_TIMEOUT : HALRF_EVT_TX_FAIL;
        }

        s_cb(e, g_halrf.rssi, g_halrf.buf, g_halrf.len);
    }
}

/*==========================================================================
 * 微秒时基
 *
 * CH592 的 SysTick 以 FREQ_SYS/8 递增（60MHz 主频 → 7.5MHz）。
 * 这里用 64 位除法换算，调用频率只有"每超帧几十次"，开销可以忽略。
 *========================================================================*/
#ifndef HALRF_SYSTICK_HZ
#define HALRF_SYSTICK_HZ        7500000u
#endif

uint32_t HALRF_Micros(void)
{
    uint32_t t = SYS_GetSysTickCnt();

    return (uint32_t)(((uint64_t)t * 1000000u) / (uint64_t)HALRF_SYSTICK_HZ);
}

__HIGH_CODE
__attribute__((noinline))
void HALRF_DelayUs(uint32_t us)
{
    uint32_t t0 = SYS_GetSysTickCnt();
    uint32_t need;

    if (us == 0u)
    {
        return;
    }

    need = (uint32_t)(((uint64_t)us * (uint64_t)HALRF_SYSTICK_HZ) / 1000000u);
    if (need == 0u)
    {
        need = 1u;
    }

    while ((uint32_t)(SYS_GetSysTickCnt() - t0) < need)
    {
        __nop();
    }
}

/*==========================================================================
 * 初始化 / 配置
 *========================================================================*/
/*********************************************************************
 * @fn      halrf_ch592_cache
 * @brief   只更新软件侧配置缓存，不碰射频硬件
 * @note    HALRF_Init() 可能落在"射频归 BLE"的时间窗里（进键盘模式时
 *          UiKbdCh_Init() 就是在协议栈建链之后才跑的），这时只能记参数，
 *          等 HALRF_BtOwnsPhy(false) 交还射频再 apply。
 *********************************************************************/
static void halrf_ch592_cache(const rf_config_t *cfg)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.accessAddress = Rf_GetU32LE(cfg->tx_addr);
    s_cfg.CRCInit       = (uint32_t)RF_CRC16_INIT;
    s_cfg.rfStatusCB    = RF_2G4StatusCallBack;
    s_cfg.RxMaxlen      = (uint8_t)(RF_PLOAD_MAX + CH592_RX_HDR_LEN + 4u);
    s_cfg.TxMaxlen      = (uint8_t)(RF_PLOAD_MAX + 4u);

    s_rate       = cfg->rate;
    s_channel    = cfg->channel;
    memcpy(s_link_addr, cfg->tx_addr, RF_ADDR_LEN);
    s_link_ch    = cfg->channel;
    s_link_valid = 1u;
}

void HALRF_Init(const rf_config_t *cfg)
{
    if (cfg == 0)
    {
        return;
    }

    halrf_ch592_cache(cfg);                 /* 先记账：不管归谁，参数都要留住 */

    /* 【P0 修复】射频归 BLE 时连 RF_Shut() 都不能调：
     * 原来这里一句 RF_Shut() 就把协议栈刚建好的 LLE 打回 idle，
     * 之后 RF_Config() 再补一刀 —— BLE 从此发不出东西。 */
    if (s_bt_owns_phy != 0u)
    {
        s_inited = 1u;                      /* 记成"已配置"，交还时按缓存 apply */
        return;
    }

    if (s_inited != 0u)
    {
        RF_Shut();
    }

    s_inited = 1u;

    g_halrf.busy = 0u;
    g_halrf.done = 0u;
    g_halrf.ok   = 0u;
    g_halrf.len  = 0u;

    halrf_ch592_apply();
}

void HALRF_SetCallback(halrf_cb_t cb)
{
    s_cb = cb;
}

/*********************************************************************
 * @fn      halrf_link_same
 * @brief   缓存里的链路和要设的是不是同一个
 * @return  true = 地址、信道都没变且射频没掉过电，不用重配
 *********************************************************************/
static bool halrf_link_same(const uint8_t addr[RF_ADDR_LEN], uint8_t channel)
{
    if ((s_inited == 0u) || (s_link_valid == 0u))
    {
        return false;                       /* 没配过 / 掉过电：必须重配 */
    }
    if (s_link_ch != channel)
    {
        return false;
    }
    return (memcmp(s_link_addr, addr, RF_ADDR_LEN) == 0);
}

void HALRF_SetLink(const uint8_t addr[RF_ADDR_LEN], uint8_t channel)
{
    if (addr == 0)
    {
        return;
    }

    /* 【P0 修复】判据换成"完整地址 + 信道 + 是否掉过电"。
     * 原来的 `(addr[0] != s_addr_key) || (s_inited == 0)` 有两个坑：
     *   1) 只比首字节 —— 地址首字节是 0xFF 时永远判为"没变"，换了地址也不重配；
     *   2) s_inited 被 HALRF_PowerDown() 清零，于是每次 PowerDown→PowerUp
     *      循环都会无条件重配一次射频（每次都是一次完整的 RF_Config）。
     * 现在只在真的需要时写 RF_Config，其余情况走便宜的换频点。 */
    if (!halrf_link_same(addr, channel))
    {
        memcpy(s_link_addr, addr, RF_ADDR_LEN);
        s_link_ch           = channel;
        s_link_valid        = 1u;
        s_cfg.accessAddress = Rf_GetU32LE(addr);
        s_channel           = channel;
        halrf_ch592_apply();                /* 换地址必须重写 RF_Config */
    }
    else
    {
        HALRF_SetChannel(channel);
    }
}

void HALRF_SetChannel(uint8_t ch)
{
    if (ch > (uint8_t)RF_MAX_CHANNEL)
    {
        return;
    }

    s_channel = ch;
    if (s_link_valid != 0u)
    {
        s_link_ch = ch;                     /* 缓存跟着走，免得下次误判成"要重配" */
    }

    /* 射频归 BLE 时不写频点寄存器（同样是改 PHY） */
    if (s_bt_owns_phy != 0u)
    {
        return;
    }

    /* LLE_MODE_EX_CHANNEL 下换频点是便宜的，不用重写 accessAddress */
    RF_SetFrequency(ch_to_freq(ch), ch);
}

void HALRF_SetRate(rf_rate_e rate)
{
    if (rate == s_rate)
    {
        return;
    }

    s_rate = rate;
    halrf_ch592_apply();
}

void HALRF_SetTxAddr(const uint8_t addr[RF_ADDR_LEN])
{
    if (addr != 0)
    {
        HALRF_SetLink(addr, s_channel);
    }
}

void HALRF_SetRxAddr(const uint8_t addr[RF_ADDR_LEN])
{
    /* CH592 的 RF PHY 只有一个 accessAddress，收发共用 */
    HALRF_SetTxAddr(addr);
}

uint8_t HALRF_Channel(void)
{
    return s_channel;
}

rf_rate_e HALRF_Rate(void)
{
    return s_rate;
}

/*==========================================================================
 * 射频归属（实现见 hal_rf.h 的说明）
 *========================================================================*/
void HALRF_BtOwnsPhy(bool on)
{
    if (on)
    {
        if (s_bt_owns_phy == 0u)
        {
            /* 交给 BLE 之前把遗留的收发状态清干净：协议栈接管后会重写 LLE，
             * 2.4G 的回调不会再来了，busy 若不清就会把后面所有 HALRF_Tx()
             * 卡在 HALRF_E_BUSY 上（切回 2.4G 通道后表现为"收发全哑"）。 */
            g_halrf.busy = 0u;
            g_halrf.done = 0u;
            g_halrf.ok   = 0u;
            g_halrf.len  = 0u;
        }
        s_bt_owns_phy = 1u;
        return;
    }

    if (s_bt_owns_phy == 0u)
    {
        return;                             /* 本来就是 2.4G 在用 */
    }

    s_bt_owns_phy = 0u;

    /* 交还：把缓存的 2.4G 配置真正装回 PHY。
     * s_inited == 0 表示 2.4G 已被 HALRF_PowerDown() 主动断电，那就不动射频，
     * 等调用方 HALRF_PowerUp() 时再 apply。 */
    if (s_inited != 0u)
    {
        halrf_ch592_apply();
    }
}

bool HALRF_IsBtPhyOwner(void)
{
    return (s_bt_owns_phy != 0u);
}

/*==========================================================================
 * 收发
 *========================================================================*/
int HALRF_Tx(const uint8_t *buf, uint8_t len)
{
    if ((buf == 0) || (len == 0u) || (len > (uint8_t)RF_PLOAD_MAX))
    {
        return HALRF_E_PARAM;
    }

    /* 射频归 BLE：不能用协议栈的 PHY 去发 2.4G 帧（帧格式/地址都不对，
     * 发出去只会污染空口，还会把协议栈的收发状态带偏） */
    if (s_bt_owns_phy != 0u)
    {
        return HALRF_E_FAIL;
    }

    /* 【修复 P2-2】重入互斥：上一次收发还没结束就不要再启动，
     * 否则 g_halrf 里的缓冲/长度会被两次操作互相覆盖。
     * busy 由 RF 回调在收尾时清零（含超时兜底），所以"发完不管"的
     * 调用方式也不会把自己锁死。 */
    if (g_halrf.busy != 0u)
    {
        return HALRF_E_BUSY;
    }

    g_halrf.busy = 1u;
    g_halrf.done = 0u;
    g_halrf.ok   = 0u;
    g_halrf.len  = 0u;

    if (RF_Tx((uint8_t *)buf, len, 0xFFu, 0xFFu) != SUCCESS)
    {
        g_halrf.busy = 0u;                  /* 启动失败：自己收回 busy */
        return HALRF_E_FAIL;
    }

    return HALRF_OK;
}

__HIGH_CODE
__attribute__((noinline))
static void halrf_wait_done(uint32_t timeout_us)
{
    uint32_t t0 = SYS_GetSysTickCnt();
    uint32_t need;

    if (timeout_us == 0u)
    {
        timeout_us = 5000u;                 /* 默认 5ms 兜底 */
    }

    need = (uint32_t)(((uint64_t)timeout_us * (uint64_t)HALRF_SYSTICK_HZ) / 1000000u);
    if (need == 0u)
    {
        need = 1u;
    }

    while (g_halrf.done == 0u)
    {
        if ((uint32_t)(SYS_GetSysTickCnt() - t0) >= need)
        {
            g_halrf.done = 1u;
                g_halrf.busy = 0u;          /* 【修复 P2-2】一次收发到此结束 */              /* 超时兜底 */
            g_halrf.ok   = 0u;
            break;
        }
    }
}

int HALRF_TxWait(const uint8_t *buf, uint8_t len, uint32_t timeout_us)
{
    int rc = HALRF_Tx(buf, len);

    if (rc != HALRF_OK)
    {
        return rc;
    }

    halrf_wait_done(timeout_us);

    return (g_halrf.ok != 0u) ? HALRF_OK : HALRF_E_FAIL;
}

int HALRF_RxStart(void)
{
    /* 射频归 BLE：开收也是在动 PHY */
    if (s_bt_owns_phy != 0u)
    {
        return HALRF_E_FAIL;
    }

    if (g_halrf.busy != 0u)
    {
        return HALRF_E_BUSY;                /* 【修复 P2-2】上一次收发还没结束 */
    }

    g_halrf.busy = 1u;
    g_halrf.done = 0u;
    g_halrf.ok   = 0u;
    g_halrf.len  = 0u;

    /* 【修复 P2-1】用能装下整帧的 s_rx_raw 接（见上面的 RX_RAW_MAX） */
    if (RF_Rx(s_rx_raw, (uint8_t)RX_RAW_MAX, 0xFFu, 0xFFu) != SUCCESS)
    {
        g_halrf.busy = 0u;                  /* 启动失败：自己收回 busy */
        return HALRF_E_FAIL;
    }

    return HALRF_OK;
}

int HALRF_RxWait(uint8_t *buf, uint8_t *len, uint32_t timeout_us)
{
    int rc = HALRF_RxStart();

    if (rc != HALRF_OK)
    {
        return rc;
    }

    halrf_wait_done(timeout_us);

    if (g_halrf.ok == 0u)
    {
        return HALRF_E_TIMEOUT;
    }

    if (buf != 0)
    {
        uint8_t i;
        for (i = 0u; i < g_halrf.len; i++)
        {
            buf[i] = g_halrf.buf[i];
        }
    }
    if (len != 0)
    {
        *len = g_halrf.len;
    }

    return HALRF_OK;
}

/*********************************************************************
 * @fn      HALRF_TxRx
 * @brief   发一帧，然后在窗口内等应答（auto 模式下硬件自动转收）
 @note     CH592 上用 LLE_MODE_AUTO：一次 RF_Tx 就把"发→收"一条龙做完，
 *          所以这里只等一次回调；窗口长度由硬件决定，
 *          规范的 150μs / 400μs 是**上层**的调度约束（时隙对齐），
 *          这里只保证不早退。
 *********************************************************************/
int HALRF_TxRx(const uint8_t *txbuf, uint8_t txlen,
               uint8_t *rxbuf, uint8_t *rxlen, uint32_t rx_window_us)
{
    int rc;

    rc = HALRF_Tx(txbuf, txlen);
    if (rc != HALRF_OK)
    {
        return rc;
    }

    /* 发送 + 等应答，窗口按参数兜底 */
    halrf_wait_done(rx_window_us + 2000u);

    if (g_halrf.ok == 0u)
    {
        return HALRF_E_TIMEOUT;
    }

    if (rxbuf != 0)
    {
        uint8_t i;
        for (i = 0u; i < g_halrf.len; i++)
        {
            rxbuf[i] = g_halrf.buf[i];
        }
    }
    if (rxlen != 0)
    {
        *rxlen = g_halrf.len;
    }

    return HALRF_OK;
}

/*==========================================================================
 * 电源 / 复位
 *========================================================================*/
void HALRF_Shut(void)
{
    /* 射频归 BLE 时不能 Shut：协议栈的 LLE 正靠这颗射频工作，
     * 一句 RF_Shut() 就等于把广播/连接掐断。 */
    if (s_bt_owns_phy != 0u)
    {
        return;
    }

    RF_Shut();
}

void HALRF_FlushTx(void)
{
    /* CH592 的 RF PHY 没有 FIFO 概念，出了错直接 Shut 重来 */
    HALRF_Shut();
}

void HALRF_FlushRx(void)
{
    HALRF_Shut();
}

void HALRF_PowerDown(void)
{
    s_inited = 0u;                          /* 掉电标记照记（交还时会重新 apply） */

    if (s_bt_owns_phy != 0u)
    {
        return;                             /* 归 BLE：不碰射频 */
    }

    RF_Shut();
    /* 低功耗部分（RTC 唤醒 + __WFI）见 README 的"低功耗"一节 */
}

void HALRF_PowerUp(void)
{
    if (s_inited == 0u)
    {
        s_inited = 1u;
        halrf_ch592_apply();                /* 归 BLE 时 apply 内部直接返回，安全 */
    }
}

/*==========================================================================
 * RSSI / CRC
 *========================================================================*/
int8_t HALRF_ReadRssi(void)
{
    int8_t r = BLE_ReadRssi();

    if (r == 0)
    {
        r = s_rssi;
    }
    return r;
}

uint8_t HALRF_Crc8(const uint8_t *d, uint16_t n)
{
    return Rf_Crc8(d, n);
}

uint16_t HALRF_Crc16(const uint8_t *d, uint16_t n)
{
    return Rf_Crc16(d, n);
}
