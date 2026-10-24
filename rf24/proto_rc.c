/**************************************************************************
 * proto_rc.c —— 遥控模式协议实现
 **************************************************************************/

#include "proto_rc.h"
#include "hal_rf.h"
#include <string.h>

/*==========================================================================
 * 帧偏移
 *========================================================================*/
#define SYNC_OFF_CTRL           0u
#define SYNC_OFF_SF_L           1u
#define SYNC_OFF_SF_H           2u
#define SYNC_OFF_BITMAP_L       3u
#define SYNC_OFF_BITMAP_H       4u
#define SYNC_OFF_HOP            5u
#define SYNC_OFF_SEED           6u
#define SYNC_OFF_FLAGS          7u

#define CMD_OFF_CTRL            0u
#define CMD_OFF_DEV             1u
#define CMD_OFF_CMD             2u
#define CMD_OFF_PARAM           3u
#define CMD_PARAM_LEN           5u

#define EACK_OFF_CTRL           0u
#define EACK_OFF_DEV            1u
#define EACK_OFF_ADDR           2u

#define ERSP_OFF_CTRL           0u
#define ERSP_OFF_HWID           1u
#define ERSP_OFF_HWVER          5u
#define ERSP_OFF_SWVER          6u
#define ERSP_OFF_NAMELEN        7u
#define ERSP_OFF_NAME           8u
#define ERSP_OFF_CAPS           27u
#define ERSP_OFF_KEYMASK        28u
#define ERSP_OFF_RSSI           29u
#define ERSP_OFF_KEYMASK_HI     30u         /* 规范里的 reserved，借来放 key_mask 第 9 位 */
#define ERSP_OFF_CRC            31u

#define RPT_OFF_CTRL            0u
#define RPT_OFF_STATUS          1u
#define RPT_OFF_KEYSTATE_L      2u
#define RPT_OFF_KEYSTATE_H      3u
#define RPT_OFF_EDGE            4u
#define RPT_OFF_ACTION          5u
#define RPT_OFF_BATT            6u
#define RPT_OFF_VER             7u

/*==========================================================================
 * 帧组包 / 解包
 *========================================================================*/
void Rc_BuildSync(uint8_t *out8, uint16_t sf_id, uint16_t slot_bitmap,
                  uint8_t hop_channel, uint8_t lfsr_seed, uint8_t flags)
{
    if (out8 == 0)
    {
        return;
    }

    out8[SYNC_OFF_CTRL]     = (uint8_t)RC_SYNC;
    Rf_PutU16LE(&out8[SYNC_OFF_SF_L], sf_id);
    Rf_PutU16LE(&out8[SYNC_OFF_BITMAP_L], slot_bitmap);
    out8[SYNC_OFF_HOP]      = hop_channel;
    out8[SYNC_OFF_SEED]     = lfsr_seed;
    out8[SYNC_OFF_FLAGS]    = flags;
}

uint8_t Rc_ParseSync(const uint8_t *in8, uint16_t *sf_id, uint16_t *slot_bitmap,
                     uint8_t *hop_channel, uint8_t *lfsr_seed, uint8_t *flags)
{
    if (in8 == 0)
    {
        return RC_E_PARAM;
    }
    if (FRAME_TYPE(in8[SYNC_OFF_CTRL]) != (uint8_t)RC_SYNC)
    {
        return RC_E_PARAM;
    }

    if (sf_id != 0)        { *sf_id        = Rf_GetU16LE(&in8[SYNC_OFF_SF_L]); }
    if (slot_bitmap != 0)  { *slot_bitmap  = Rf_GetU16LE(&in8[SYNC_OFF_BITMAP_L]); }
    if (hop_channel != 0)  { *hop_channel  = in8[SYNC_OFF_HOP]; }
    if (lfsr_seed != 0)    { *lfsr_seed    = in8[SYNC_OFF_SEED]; }
    if (flags != 0)        { *flags        = in8[SYNC_OFF_FLAGS]; }

    return RC_OK;
}

void Rc_BuildCmd(uint8_t *out8, uint8_t dev_id, uint8_t cmd,
                 const uint8_t *param, uint8_t len)
{
    if (out8 == 0)
    {
        return;
    }

    memset(out8, 0, RC_CMD_LEN);

    out8[CMD_OFF_CTRL] = (uint8_t)RC_CMD;
    out8[CMD_OFF_DEV]  = dev_id;
    out8[CMD_OFF_CMD]  = cmd;

    if ((param != 0) && (len > 0u))
    {
        if (len > (uint8_t)CMD_PARAM_LEN)
        {
            len = (uint8_t)CMD_PARAM_LEN;
        }
        memcpy(&out8[CMD_OFF_PARAM], param, len);
    }
}

uint8_t Rc_ParseCmd(const uint8_t *in8, uint8_t *dev_id, uint8_t *cmd,
                    uint8_t *param, uint8_t *len)
{
    uint8_t n;

    if (in8 == 0)
    {
        return RC_E_PARAM;
    }
    if (FRAME_TYPE(in8[CMD_OFF_CTRL]) != (uint8_t)RC_CMD)
    {
        return RC_E_PARAM;
    }

    if (dev_id != 0) { *dev_id = in8[CMD_OFF_DEV]; }
    if (cmd != 0)    { *cmd    = in8[CMD_OFF_CMD]; }

    if (len != 0)    { *len    = (uint8_t)CMD_PARAM_LEN; }
    if (param != 0)
    {
        for (n = 0u; n < (uint8_t)CMD_PARAM_LEN; n++)
        {
            param[n] = in8[CMD_OFF_PARAM + n];
        }
    }

    if ((in8[CMD_OFF_DEV] < 1u) || (in8[CMD_OFF_DEV] > (uint8_t)RF_RC_MAX_DEV))
    {
        return RC_E_PARAM;
    }

    return RC_OK;
}

void Rc_BuildEnumAck(uint8_t *out8, uint8_t dev_id, const uint8_t addr[RF_ADDR_LEN],
                     uint8_t hw_id_low)
{
    if ((out8 == 0) || (addr == 0))
    {
        return;
    }

    memset(out8, 0, RC_ENUM_ACK_LEN);
    out8[EACK_OFF_CTRL] = (uint8_t)RC_ENUM_ACK;
    out8[EACK_OFF_DEV]  = dev_id;
    memcpy(&out8[EACK_OFF_ADDR], addr, RF_ADDR_LEN);
    out8[RC_ENUM_ACK_OFF_HWID] = hw_id_low;
}

uint8_t Rc_ParseEnumAck(const uint8_t *in8, uint8_t *dev_id,
                        uint8_t addr[RF_ADDR_LEN], uint8_t *hw_id_low)
{
    if ((in8 == 0) || (addr == 0))
    {
        return RC_E_PARAM;
    }
    if (FRAME_TYPE(in8[EACK_OFF_CTRL]) != (uint8_t)RC_ENUM_ACK)
    {
        return RC_E_PARAM;
    }

    if (dev_id != 0)     { *dev_id     = in8[EACK_OFF_DEV]; }
    memcpy(addr, &in8[EACK_OFF_ADDR], RF_ADDR_LEN);
    if (hw_id_low != 0)  { *hw_id_low  = in8[RC_ENUM_ACK_OFF_HWID]; }

    if ((in8[EACK_OFF_DEV] < 1u) || (in8[EACK_OFF_DEV] > (uint8_t)RF_RC_MAX_DEV))
    {
        return RC_E_FULL;
    }

    return RC_OK;
}

void Rc_BuildReport(uint8_t *out8, uint8_t status, uint16_t key_state,
                    uint8_t key_edge, uint8_t key_action,
                    uint8_t adc_batt, uint8_t config_ver)
{
    if (out8 == 0)
    {
        return;
    }

    out8[RPT_OFF_CTRL]     = (uint8_t)RC_REPORT;
    out8[RPT_OFF_STATUS]   = status;
    Rf_PutU16LE(&out8[RPT_OFF_KEYSTATE_L], key_state);
    out8[RPT_OFF_EDGE]     = key_edge;
    out8[RPT_OFF_ACTION]   = key_action;
    out8[RPT_OFF_BATT]     = adc_batt;
    out8[RPT_OFF_VER]      = config_ver;
}

uint8_t Rc_ParseReport(const uint8_t *in8, uint8_t *status, uint16_t *key_state,
                       uint8_t *key_edge, uint8_t *key_action,
                       uint8_t *adc_batt, uint8_t *config_ver)
{
    if (in8 == 0)
    {
        return RC_E_PARAM;
    }
    if (FRAME_TYPE(in8[RPT_OFF_CTRL]) != (uint8_t)RC_REPORT)
    {
        return RC_E_PARAM;
    }

    if (status != 0)     { *status     = in8[RPT_OFF_STATUS]; }
    if (key_state != 0)  { *key_state  = Rf_GetU16LE(&in8[RPT_OFF_KEYSTATE_L]); }
    if (key_edge != 0)   { *key_edge   = in8[RPT_OFF_EDGE]; }
    if (key_action != 0) { *key_action = in8[RPT_OFF_ACTION]; }
    if (adc_batt != 0)   { *adc_batt   = in8[RPT_OFF_BATT]; }
    if (config_ver != 0) { *config_ver = in8[RPT_OFF_VER]; }

    return RC_OK;
}
/*==========================================================================
 * 主机侧：内部工具
 *========================================================================*/
static rc_host_dev_t *rc_host_free_slot(rc_host_t *h)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)RC_HOST_MAX_DEV; i++)
    {
        if (h->dev[i].used == 0u)
        {
            return &h->dev[i];
        }
    }
    return 0;
}

static rc_host_dev_t *rc_host_by_id(rc_host_t *h, uint8_t dev_id)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)RC_HOST_MAX_DEV; i++)
    {
        if ((h->dev[i].used != 0u) && (h->dev[i].dev_id == dev_id))
        {
            return &h->dev[i];
        }
    }
    return 0;
}
static void rc_host_handle_report(rc_host_t *h, rc_host_dev_t *d, const uint8_t *rpt)
{
    uint8_t  status = 0u;
    uint16_t ks     = 0u;
    uint8_t  edge   = 0u;
    uint8_t  action = 0u;
    uint8_t  batt   = 0u;
    uint8_t  ver    = 0u;

    if (Rc_ParseReport(rpt, &status, &ks, &edge, &action, &batt, &ver) != RC_OK)
    {
        return;
    }

    d->status     = status;
    d->adc_batt   = batt;
    d->batt_mv    = (uint16_t)(((uint32_t)batt * 3300u) / 255u);

    /* 【协议瘦身】从机不再上报按键：CH570Q 是纯执行端，按键都在 CH592X 自己身上。
     * REPORT 里的 key_state / key_edge / key_action 三个字段保留在帧格式里
     * （帧长不变，向后兼容），但主机不再解析、不再驻留 RAM。 */
    (void)h; (void)ks; (void)edge; (void)action; (void)ver;

    d->last_seen_ms = h->ms;
    d->miss_cnt     = 0u;

    if (d->phase == RC_DEV_OFFLINE)
    {
        d->phase = RC_DEV_READY;
    }

}
/*==========================================================================
 * 主机：初始化
 *========================================================================*/
void Rc_HostInit(rc_host_t *h, uint8_t pair_seed)
{
    uint8_t i;

    if (h == 0)
    {
        return;
    }

    memset(h, 0, sizeof(*h));

    h->pair_seed = (pair_seed == 0u) ? 0x0Bu : pair_seed;

    /* 本网地址：CH592 只有一个 accessAddress，所有从机共用 */
    Rf_MakeAddr(h->rc_addr, 0x5F435244u /* "DRC_" */, 0x00u);
    Rf_MakeRcHostAddr(h->enum_addr);
    /* 枚举用固定的 0xAABBCCDD01，这是从机开机就知道的唯一地址 */
    h->enum_addr[0] = (uint8_t)RF_ADDR_RC_ENUM0;
    h->enum_addr[1] = (uint8_t)RF_ADDR_RC_ENUM1;
    h->enum_addr[2] = (uint8_t)RF_ADDR_RC_ENUM2;
    h->enum_addr[3] = (uint8_t)RF_ADDR_RC_ENUM3;
    h->enum_addr[4] = (uint8_t)RF_ADDR_RC_ENUM4;

    Tdma_Init(&h->tdma);
    Hop_Init(&h->hop, h->pair_seed, (uint8_t)HOP_RC_COUNT, hop_rc_table);
    Link_StatInit(&h->stat, 0u);

    for (i = 0u; i <= (uint8_t)RF_RC_MAX_DEV; i++)
    {
        h->cmdq[i].valid = 0u;
        h->last_rpt_ms[i] = 0u;
    }

    h->state = RC_HOST_RUNNING;
}

void Rc_HostTickMs(rc_host_t *h)
{
    uint8_t i;

    if (h == 0)
    {
        return;
    }

    h->ms++;

    /* 从机离线判定：3 个超帧（48ms）没消息就标离线（规范 RC-13） */
    for (i = 0u; i < (uint8_t)RC_HOST_MAX_DEV; i++)
    {
        rc_host_dev_t *d = &h->dev[i];

        if ((d->used == 0u) || (d->phase == RC_DEV_OFFLINE))
        {
            continue;
        }

        if ((uint32_t)(h->ms - d->last_seen_ms) > (uint32_t)(RC_SUPERFRAME_MS * 3u))
        {
            d->phase    = RC_DEV_OFFLINE;
            d->miss_cnt = 0xFFu;
        }
    }
}

void Rc_HostClear(rc_host_t *h)
{
    if (h != 0)
    {
        memset(h->dev, 0, sizeof(h->dev));
        Tdma_Init(&h->tdma);
    }
}

uint8_t Rc_HostCount(const rc_host_t *h)
{
    uint8_t i;
    uint8_t n = 0u;

    if (h == 0)
    {
        return 0u;
    }

    for (i = 0u; i < (uint8_t)RC_HOST_MAX_DEV; i++)
    {
        if (h->dev[i].used != 0u)
        {
            n++;
        }
    }
    return n;
}

rc_host_dev_t *Rc_HostDev(rc_host_t *h, uint8_t idx)
{
    uint8_t i;
    uint8_t n = 0u;

    if (h == 0)
    {
        return 0;
    }

    for (i = 0u; i < (uint8_t)RC_HOST_MAX_DEV; i++)
    {
        if (h->dev[i].used != 0u)
        {
            if (n == idx)
            {
                return &h->dev[i];
            }
            n++;
        }
    }
    return 0;
}

rc_host_dev_t *Rc_HostFind(rc_host_t *h, uint8_t dev_id)
{
    return (h != 0) ? rc_host_by_id(h, dev_id) : 0;
}

rc_host_state_t Rc_HostState(const rc_host_t *h)
{
    return (h != 0) ? h->state : RC_HOST_RESYNC;
}
bool Rc_HostSendFrame(rc_host_t *h, uint8_t dev_id, const uint8_t *frame, uint8_t len)
{
    rc_cmd_t *q;

    if ((h == 0) || (frame == 0) || (dev_id < 1u) || (dev_id > (uint8_t)RF_RC_MAX_DEV))
    {
        return false;
    }

    if ((len == 0u) || (len > (uint8_t)RF_MTU))
    {
        return false;
    }

    q = &h->cmdq[dev_id];

    memset(q->data, 0, sizeof(q->data));
    memcpy(q->data, frame, len);
    q->len   = len;
    q->retry = 0u;
    q->valid = 1u;

    return true;
}

bool Rc_HostSendCmd(rc_host_t *h, uint8_t dev_id, uint8_t cmd,
                    const uint8_t *param, uint8_t len)
{
    uint8_t frame[RC_CMD_LEN];

    if (h == 0)
    {
        return false;
    }

    Rc_BuildCmd(frame, dev_id, cmd, param, len);

    return Rc_HostSendFrame(h, dev_id, frame, (uint8_t)RC_CMD_LEN);
}
/*==========================================================================
 * 主机：超帧里的各个时隙
 *========================================================================*/
static void rc_host_slot_sync(rc_host_t *h)
{
    uint8_t sync[RC_SYNC_LEN];
    uint8_t ch = Hop_Current(&h->hop);
    uint8_t flags = 0u;

    if (ch == 0xFFu)
    {
        ch = hop_rc_table[0];
    }

    HALRF_SetChannel(ch);

    if ((h->tdma.sf_id % (uint16_t)RC_ENUM_EVERY_SF) == 0u)
    {
        flags |= (uint8_t)RC_SYNC_FLAG_ENUM_OPEN;
    }

    Rc_BuildSync(sync, h->tdma.sf_id, h->tdma.slot_bitmap,
                 h->hop.pos, h->hop.seed, flags);

    if (HALRF_TxWait(sync, (uint8_t)RC_SYNC_LEN, 2000u) != HALRF_OK)
    {
        Link_OnTxFail(&h->stat);
    }
    else
    {
        Link_OnTxOk(&h->stat);
    }
}

static void rc_host_slot_dev(rc_host_t *h, uint8_t dev_id)
{
    rc_host_dev_t *d = rc_host_by_id(h, dev_id);
    rc_cmd_t *q = &h->cmdq[dev_id];

    uint8_t frame[RF_MTU];
    uint8_t flen;
    uint8_t rx[RF_PLOAD_MAX];
    uint8_t rlen = 0u;

    if (d == 0)
    {
        return;
    }

    if (q->valid != 0u)
    {
        flen = q->len;
        memcpy(frame, q->data, flen);
    }
    else
    {
        /* 没事就发个 NOP，从机照样要回 REPORT */
        Rc_BuildCmd(frame, dev_id, (uint8_t)RC_CMD_NOP, 0, 0u);
        flen = (uint8_t)RC_CMD_LEN;
    }

    if (HALRF_TxRx(frame, flen, rx, &rlen, RF_RC_RX_WINDOW_US) != HALRF_OK)
    {
        Link_OnTxFail(&h->stat);

        if (d->miss_cnt < 0xFFu)
        {
            d->miss_cnt++;
        }

        if (q->valid != 0u)
        {
            q->retry++;
            if (q->retry >= 3u)
            {
                q->valid = 0u;              /* 3 个超帧都没成，放弃（规范 8.2） */
            }
        }
        return;
    }

    Link_OnTxOk(&h->stat);

    /* 从机在自己的时隙里回话了 —— 不管回的是 REPORT 还是 KEYMAP 分片，
     * 都说明这次下发的东西它收到了，队列可以清掉。
     * （如果漏了这一步，排队的命令会每个超帧重发一次，
     *   比如 GET_KEYMAP 会让从机每帧都从第 0 片重来。） */
    if (q->valid != 0u)
    {
        q->valid = 0u;
    }

    /* 从机在自己的时隙里可能回 REPORT，也可能回 KEYMAP 分片 */
    switch (FRAME_TYPE(rx[0]))
    {
        case RC_REPORT:
            rc_host_handle_report(h, d, rx);
            d->rssi = HALRF_ReadRssi();
            break;

        default:
            /* 不认识的应答：当丢包处理 */
            if (d->miss_cnt < 0xFFu)
            {
                d->miss_cnt++;
            }
            break;
    }
}

static void rc_host_slot_enum(rc_host_t *h)
{
    uint8_t  sync[RC_SYNC_LEN];
    uint8_t  rx[RF_PLOAD_MAX];
    uint8_t  rlen = 0u;
    uint8_t  dev_id;
    uint8_t  addr[RF_ADDR_LEN];
    rc_host_dev_t *d;

    /* 1) 切到枚举信道 + 枚举地址 */
    HALRF_SetLink(h->enum_addr, (uint8_t)RF_RC_ENUM_CHANNEL);

    /* 2) 广播 ENUM_REQ（= SYNC + enum_open，见头文件注释） */
    Rc_BuildSync(sync, h->tdma.sf_id, h->tdma.slot_bitmap, 0u, h->pair_seed,
                 (uint8_t)RC_SYNC_FLAG_ENUM_OPEN);

    if (HALRF_TxWait(sync, (uint8_t)RC_SYNC_LEN, 2000u) != HALRF_OK)
    {
        Link_OnTxFail(&h->stat);
        goto restore;
    }

    /* 3) 等 ENUM_RSP（从机会随机退避 0..3 档 ×100μs） */
    if (HALRF_RxWait(rx, &rlen, 600u) != HALRF_OK)
    {
        goto restore;
    }

    if ((rlen < (uint8_t)RC_ENUM_RSP_LEN) || (FRAME_TYPE(rx[0]) != (uint8_t)RC_ENUM_RSP))
    {
        goto restore;
    }

    /* 4) 分配 dev_id + 地址 + 时隙 */
    dev_id = Tdma_AllocSlot(&h->tdma);

    d = rc_host_free_slot(h);
    if ((dev_id == 0u) || (d == 0))
    {
        Tdma_SetActive(&h->tdma, dev_id, false);
        goto restore;                       /* RC_E_FULL：这轮不收 */
    }

    memset(d, 0, sizeof(*d));
    d->used      = 1u;
    d->dev_id    = dev_id;
    d->phase     = RC_DEV_READY;
    d->last_seen_ms = h->ms;

    memcpy(d->hw_id, &rx[ERSP_OFF_HWID], 4u);
    d->hw_ver  = rx[ERSP_OFF_HWVER];
    d->sw_ver  = rx[ERSP_OFF_SWVER];
    d->caps    = rx[ERSP_OFF_CAPS];
    d->key_mask = (uint16_t)((uint16_t)rx[ERSP_OFF_KEYMASK]
                             | ((uint16_t)(rx[ERSP_OFF_KEYMASK_HI] & 0x01u) << 8));
    d->rssi    = (int8_t)rx[ERSP_OFF_RSSI];

    /* 设备名（规范 5.3.4：len 字段 + ASCII）
     * 【协议瘦身】主机只留设备名 —— 它是查"设备绑定表 -> 动作编号表"的钥匙，
     * 9 个键名不再从从机传上来，主机也不为键名驻留 RAM。 */
    {
        uint8_t nlen = rx[ERSP_OFF_NAMELEN];

        if (nlen > (uint8_t)RC_DEV_NAME_MAX)
        {
            nlen = (uint8_t)RC_DEV_NAME_MAX;
        }
        memcpy(d->dev_name, &rx[ERSP_OFF_NAME], nlen);
        d->dev_name[nlen] = '\0';
        d->dev_name_len   = nlen;
    }

#if (RF24_RC_PER_DEVICE_ADDR)
    /* 规范原方案：每个从机一个独立 accessAddress，主机每个时隙都要重配射频 */
    Rf_MakeRcDevAddr(addr, dev_id);
#else
    /* 单地址方案（默认）：所有从机共用主机的 accessAddress，从机之间靠
     * TDMA 时隙 + 帧里的 dev_id 区分。所以 ENUM_ACK 里发的是"本网地址"，
     * 而不是每个设备各自的地址 —— 不然从机切过去之后就跟主机对不上了。 */
    memcpy(addr, h->rc_addr, RF_ADDR_LEN);
#endif

    /* 5) 发 ENUM_ACK（带 hw_id 低字节，从机对一下才认领） */
    {
        uint8_t ack[RC_ENUM_ACK_LEN];
        Rc_BuildEnumAck(ack, dev_id, addr, d->hw_id[0]);
        (void)HALRF_TxWait(ack, (uint8_t)RC_ENUM_ACK_LEN, 2000u);
    }

    /* 6) 【协议瘦身】不再索要 9 键名称 —— 设备名已经拿到，
     *    动作编号表是主机本地的东西。枚举完直接进正常超帧。 */
    h->state = RC_HOST_RUNNING;

restore:
    /* 7) 切回本网信道 + 本网地址 */
    HALRF_SetLink(h->rc_addr, Hop_Current(&h->hop));
}

/*==========================================================================
 * 主机：跑一个超帧
 *========================================================================*/
void Rc_HostSuperframe(rc_host_t *h)
{
    uint8_t slot;
    uint8_t ch;
    bool    enum_needed;

    if (h == 0)
    {
        return;
    }

    /* 先把本超帧用哪个频点定下来，再发 SYNC。
     * 放在开头（而不是超帧末尾）是为了让 Hop_Current() 在整个超帧内
     * 都等于 SYNC 里广播的那个频点 —— 从机照着 SYNC 对齐，
     * 两边就不会差一拍。 */
    (void)Hop_Next(&h->hop);
    if (Hop_NeedResync(&h->hop))
    {
        h->state = RC_HOST_RESYNC;
        Hop_BlacklistClear(&h->hop);        /* 重枚举时先把黑名单清了 */
    }

    Tdma_BeginSuperframe(&h->tdma, HALRF_Micros());

    /* ---- Slot 0：SYNC ---- */
    rc_host_slot_sync(h);
    Tdma_WaitSlotEnd(&h->tdma, HALRF_Micros());

    /* ---- Slot 1..19 ---- */
    for (;;)
    {
        slot = Tdma_NextSlot(&h->tdma, HALRF_Micros());
        if (slot == RC_SLOT_END)
        {
            break;
        }

        if ((slot >= (uint8_t)RC_SLOT_DEV_FIRST) && (slot <= (uint8_t)RC_SLOT_DEV_LAST))
        {
            if (Tdma_IsActive(&h->tdma, slot))
            {
                rc_host_slot_dev(h, slot);
            }
        }
        else if (slot == (uint8_t)RC_SLOT_ENUM)
        {
            enum_needed = Tdma_EnumWindow(&h->tdma)
                          && (Rc_HostCount(h) < (uint8_t)RC_HOST_MAX_DEV);
            if (enum_needed)
            {
                rc_host_slot_enum(h);
            }
        }

        Tdma_WaitSlotEnd(&h->tdma, HALRF_Micros());
    }

    (void)ch;
    h->tdma.sf_id++;
    Link_UpdatePer(&h->stat);
}

/*==========================================================================
 * 从机侧
 *========================================================================*/
void Rc_DevInit(rc_dev_t *d, const rc_keymap_t *km, uint8_t pair_seed)
{
    if (d == 0)
    {
        return;
    }

    memset(d, 0, sizeof(*d));

    if (km != 0)
    {
        d->km = *km;
    }
    else
    d->pair_seed = (pair_seed == 0u) ? 0x0Bu : pair_seed;
    d->dev_id    = 0u;
    d->channel   = (uint8_t)RF_RC_ENUM_CHANNEL;
    d->state     = RC_DEV_IDLE;

    Rf_MakeRcDevAddr(d->tx_addr, 0u);       /* 先听枚举地址 */
}

void Rc_DevSetIdentity(rc_dev_t *d, const uint8_t hw_id[4], uint8_t hw_ver, uint8_t sw_ver)
{
    if ((d == 0) || (hw_id == 0))
    {
        return;
    }

    memcpy(d->hw_id, hw_id, 4u);
    d->hw_ver = hw_ver;
    d->sw_ver = sw_ver;
    d->backoff = (uint8_t)(hw_id[0] % (uint8_t)RC_ENUM_BACKOFF_STEPS);
}

uint32_t Rc_DevEnumBackoffUs(const rc_dev_t *d)
{
    if (d == 0)
    {
        return 0u;
    }
    return (uint32_t)d->backoff * (uint32_t)RC_ENUM_BACKOFF_US;
}
void Rc_DevSetKeys(rc_dev_t *d, uint16_t key_state)
{
    if (d != 0)
    {
        d->key_state = (uint16_t)(key_state & 0x01FFu);
    }
}

void Rc_DevSetBattery(rc_dev_t *d, uint8_t adc_batt, bool low)
{
    if (d != 0)
    {
        d->adc_batt = adc_batt;
        d->batt_low = (low != 0) ? 1u : 0u;
    }
}

void Rc_DevSetChannel(rc_dev_t *d, uint8_t ch)
{
    if ((d != 0) && (ch <= (uint8_t)RF_MAX_CHANNEL))
    {
        d->channel = ch;
    }
}

uint8_t Rc_DevBuildEnumRsp(const rc_dev_t *d, uint8_t *out32)
{
    uint8_t nlen;

    if ((d == 0) || (out32 == 0))
    {
        return 0u;
    }

    memset(out32, 0, RC_ENUM_RSP_LEN);
    out32[ERSP_OFF_CTRL] = (uint8_t)RC_ENUM_RSP;
    memcpy(&out32[ERSP_OFF_HWID], d->hw_id, 4u);
    out32[ERSP_OFF_HWVER] = d->hw_ver;
    out32[ERSP_OFF_SWVER] = d->sw_ver;

    nlen = d->km.dev_name_len;
    if (nlen > (uint8_t)RC_DEV_NAME_MAX)
    {
        nlen = (uint8_t)RC_DEV_NAME_MAX;
    }
    out32[ERSP_OFF_NAMELEN] = nlen;
    memcpy(&out32[ERSP_OFF_NAME], d->km.dev_name, nlen);

    out32[ERSP_OFF_CAPS]    = (uint8_t)(RC_CAP_HAS_9KEY | RC_CAP_HAS_BATT);
    /* The spec only gives key_mask one byte, but 9 keys need 9 bits.
     * Offset 29 is rssi, so the 9th bit goes into offset 30 (reserved). */
    out32[ERSP_OFF_KEYMASK]     = (uint8_t)(d->km.key_mask & 0xFFu);
    out32[ERSP_OFF_KEYMASK_HI]  = (uint8_t)((d->km.key_mask >> 8) & 0x01u);
    out32[ERSP_OFF_RSSI]    = 0u;           /* 由射频层回填 */
    out32[ERSP_OFF_CRC]     = Rf_Crc8(out32, (uint16_t)ERSP_OFF_CRC);

    return (uint8_t)RC_ENUM_RSP_LEN;
}

bool Rc_DevOnSync(rc_dev_t *d, const uint8_t *sync8, uint32_t now_us, uint32_t now_ms)
{
    uint16_t sf_id = 0u;
    uint16_t bitmap = 0u;
    uint8_t  hop_ch = 0u;
    uint8_t  seed = 0u;
    uint8_t  flags = 0u;

    if ((d == 0) || (sync8 == 0))
    {
        return false;
    }
    (void)seed;

    if (Rc_ParseSync(sync8, &sf_id, &bitmap, &hop_ch, &seed, &flags) != RC_OK)
    {
        return false;
    }

    d->miss_sf     = 0u;
    d->sf_id       = sf_id;
    d->sf_start_us = now_us;
    d->last_sync_ms = now_ms;

    /* --- 还没被枚举：只在枚举窗口里回应 --- */
    if (d->dev_id == 0u)
    {
        if ((flags & (uint8_t)RC_SYNC_FLAG_ENUM_OPEN) == 0u)
        {
            return false;
        }
        d->state = RC_DEV_ENUM_SENT;
        return true;                        /* 上层负责退避 + 发 ENUM_RSP */
    }

    /* --- 已枚举：跟着 SYNC 换频点 ---
     * SYNC.hop_channel 是**频点表里的下标**（规范 5.3.1 写的就是"索引"），
     * 不是 CH 号，所以这里要查一次表。 */
    if (hop_ch < (uint8_t)HOP_RC_COUNT)
    {
        d->hop_index = hop_ch;
        Rc_DevSetChannel(d, hop_rc_table[hop_ch]);
    }

    /* 只要 SYNC 里点了我，就说明主机还在 */
    if ((bitmap & (uint16_t)(1u << (d->dev_id - 1u))) == 0u)
    {
        return false;                       /* 主机把我踢了 */
    }

    /* 收到 SYNC 就锁上；RESYNC / IDLE 都回到 RUNNING */
    if (d->state != RC_DEV_KEYMAP_SEND)
    {
        d->state = RC_DEV_RUNNING;
    }

    return false;
}

void Rc_DevBuildReport(rc_dev_t *d, uint8_t *out8)
{
    uint8_t  status = 0u;
    uint8_t  edge   = 0u;
    uint8_t  action = 0u;
    uint16_t diff;
    uint8_t  i;

    if ((d == 0) || (out8 == 0))
    {
        return;
    }

    /* 边沿检测：只报"新按下"的那个键（规范 5.3.3 / 22.2） */
    diff = (uint16_t)(d->key_state & (uint16_t)(~d->key_last));

    for (i = 0u; i < (uint8_t)RC_KEY_COUNT; i++)
    {
        if ((diff & (uint16_t)(1u << i)) != 0u)
        {
            edge   = (uint8_t)(i + 1u);
            action = RC_ACT_GET(d->km.key[i].act);
            break;
        }
    }

    status |= (uint8_t)RC_ST_ONLINE;

    if (d->batt_low != 0u)
    {
        status |= (uint8_t)RC_ST_BATT_LOW;
    }
    if (edge != 0u)
    {
        status |= (uint8_t)RC_ST_KEY_CHANGED;
        d->key_evt_cnt++;
    }
    if (d->state == RC_DEV_RUNNING)
    {
        status |= (uint8_t)RC_ST_ENUM_COMPLETE;
    }

    Rc_BuildReport(out8, status, d->key_state, edge, action,
                   d->adc_batt, d->km.config_ver);

    d->key_last = d->key_state;
}

/*********************************************************************
 * @fn      Rc_DevServeSlot
 * @brief   从机在自己的时隙里服务一次：收一帧 → 回一帧
 *
 * 主机在一个时隙里只会发**一帧**，但它可能是：
 *   RC_CMD（NOP / GET_KEYMAP / SLEEP / RESET）
 *   RC_KEYMAP_MISS（要我补哪几片）
 *   RC_KEYMAP_ACK（主机收齐了）
 *   RC_RENAME_KEY（改名 + 改行为）
 * 所以这里按 frame_ctrl 分发，而不是只认 RC_CMD。
 *********************************************************************/
bool Rc_DevServeSlot(rc_dev_t *d, const uint8_t *rxBuf, uint8_t rxLen,
                     uint8_t *out32, uint8_t *outlen)
{
    uint8_t type;

    if ((d == 0) || (rxBuf == 0) || (out32 == 0) || (outlen == 0))
    {
        return false;
    }
    if (rxLen == 0u)
    {
        return false;
    }

    type = FRAME_TYPE(rxBuf[0]);

    /* 【协议瘦身】原来的"改名 / 补片 / 分片确认"三个分支已删除 ——
     * 名称传输机制整个没有了，从机不再上传键名。
     * 只保留通用命令（RC_CMD）。 */
    if (type == (uint8_t)RC_CMD)
    {
        uint8_t dev_id = 0u;
        uint8_t cmd    = 0u;

        if (Rc_ParseCmd(rxBuf, &dev_id, &cmd, 0, 0) != RC_OK)
        {
            return false;
        }
        if (dev_id != d->dev_id)
        {
            return false;                   /* 不是给我的 */
        }

        switch (cmd)
        {
            case RC_CMD_SLEEP:
                d->state = RC_DEV_SLEEP;
                break;

            case RC_CMD_RESET:
                d->dev_id  = 0u;
                d->state   = RC_DEV_IDLE;
                d->channel = (uint8_t)RF_RC_ENUM_CHANNEL;
                Rf_MakeRcDevAddr(d->tx_addr, 0u);
                return false;

            default:
                break;
        }
    }
    else
    {
        return false;
    }

    /* ---- 6) 常规上报 ---- */
    Rc_DevBuildReport(d, out32);
    *outlen = (uint8_t)RC_REPORT_LEN;
    return true;
}

uint8_t Rc_DevOnEnumAck(rc_dev_t *d, const uint8_t *ack8)
{
    uint8_t dev_id = 0u;
    uint8_t hw_low = 0u;
    uint8_t addr[RF_ADDR_LEN];

    if ((d == 0) || (ack8 == 0))
    {
        return RC_E_PARAM;
    }

    if (Rc_ParseEnumAck(ack8, &dev_id, addr, &hw_low) != RC_OK)
    {
        return RC_E_PARAM;
    }

    /* 单地址方案下所有未枚举从机都会收到这帧，靠 hw_id 低字节认领 */
    if (hw_low != d->hw_id[0])
    {
        return RC_E_PARAM;
    }

    d->dev_id = dev_id;
    memcpy(d->tx_addr, addr, RF_ADDR_LEN);
    d->state  = RC_DEV_RUNNING;         /* 【协议瘦身】不再有键表上传阶段 */

    return RC_OK;
}
void Rc_DevTickMs(rc_dev_t *d, uint32_t now_ms)
{
    if (d == 0)
    {
        return;
    }

    if ((d->state == RC_DEV_RUNNING) || (d->state == RC_DEV_KEYMAP_SEND))
    {
        if ((uint32_t)(now_ms - d->last_sync_ms) > (uint32_t)(RC_SUPERFRAME_MS * RC_SYNC_LOST_LIMIT))
        {
            d->miss_sf = RC_SYNC_LOST_LIMIT;
            d->state   = RC_DEV_RESYNC;
        }
    }

    if (d->state == RC_DEV_RESYNC)
    {
        if ((uint32_t)(now_ms - d->last_sync_ms) > (uint32_t)RC_DEV_SCAN_MS)
        {
            /* 256ms 都没找回来：释放 dev_id，回 IDLE 重新枚举 */
            d->dev_id  = 0u;
            d->channel = (uint8_t)RF_RC_ENUM_CHANNEL;
            d->state   = RC_DEV_IDLE;
            Rf_MakeRcDevAddr(d->tx_addr, 0u);
        }
    }
}
