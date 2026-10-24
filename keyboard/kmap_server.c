/**************************************************************************
 * kmap_server.c —— 映射配置协议的业务处理（设备端）
 *
 * 每个命令的处理都很短：解析负载 -> 调用 key_matrix / kmap_store -> 组应答。
 * 参数一律做越界检查，错误用 KMAP_CMD_ERROR + 错误码回过去，
 * 绝不让一个畸形请求把设备搞挂。
 **************************************************************************/

#include "kmap_server.h"
#include "macro.h"
#include "rc_table.h"
#include "kmap_proto.h"
#include "kmap_store.h"
#include "key_matrix.h"

#include <string.h>

/*==========================================================================
 * 统计
 *========================================================================*/
static uint32_t s_rx_frames;
static uint32_t s_err_frames;

/* 固件版本标记：改协议行为时动一下这个数，上位机能识别 */
#define KMAP_FW_BUILD   0x0001u

/*==========================================================================
 * 小工具：组一个「只有一个错误码」的应答
 *========================================================================*/
static uint16_t kmap_reply_err(uint8_t seq, uint8_t errcode, uint8_t *out)
{
    uint8_t pay[1];

    pay[0] = errcode;
    s_err_frames++;
    return Kmap_BuildFrame(out, KMAP_CMD_ERROR, seq, pay, 1u);
}

/* 空应答（只有命令+序号，无负载） */
static uint16_t kmap_reply_ok(uint8_t cmd, uint8_t seq, uint8_t *out)
{
    return Kmap_BuildFrame(out, KMAP_ACK(cmd), seq, 0, 0u);
}

/*==========================================================================
 * 各命令处理
 *========================================================================*/

/* 0x01 PING -> PONG（回一个 4 字节标识，便于人意确认） */
static uint16_t kmap_do_ping(uint8_t seq, uint8_t *out)
{
    uint8_t pay[4];

    pay[0] = (uint8_t)(KMAP_INFO_MAGIC & 0xFFu);
    pay[1] = (uint8_t)((KMAP_INFO_MAGIC >> 8) & 0xFFu);
    pay[2] = KMAP_STORE_VER & 0xFFu;
    pay[3] = 1u;                        /* 协议主版本 */

    return Kmap_BuildFrame(out, KMAP_CMD_PONG, seq, pay, 4u);
}

/* 0x02 GET_INFO */
static uint16_t kmap_do_get_info(uint8_t seq, uint8_t *out)
{
    uint8_t pay[10];

    Kmap_PutU16(&pay[0], KMAP_INFO_MAGIC);
    pay[2] = 1u;                                        /* ver_major */
    pay[3] = 2u;                                        /* ver_minor：2 = 支持键盘宏 */
    pay[4] = (uint8_t)KEY_MAP_GROUPS;
    pay[5] = (uint8_t)KEY_MAP_KEYS;
    pay[6] = KEY_GetMapGroup();
    pay[7] = KmapStore_HasFlashRecord() ? 1u : 0u;
    Kmap_PutU16(&pay[8], KMAP_FW_BUILD);

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_GET_INFO), seq, pay, 10u);
}

/* 0x10 GET_MAP：负载 1 字节组号，应答 18 字节（9 个 uint16） */
static uint16_t kmap_do_get_map(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t  pay[KMAP_KEYS * 2u];
    uint8_t  g;
    uint8_t  k;
    uint16_t pos;

    if (len != 1u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g = p[0];
    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    pos = 0u;
    for (k = 1u; k <= (uint8_t)KEY_MAP_KEYS; k++)
    {
        Kmap_PutU16(&pay[pos], KEY_GetMapValue(g, k));
        pos = (uint16_t)(pos + 2u);
    }

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_GET_MAP), seq, pay, (uint16_t)sizeof(pay));
}

/* 0x11 SET_KEY：负载 4 字节 [group, key, val_lo, val_hi] */
static uint16_t kmap_do_set_key(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t g;
    uint8_t k;
    uint16_t v;

    if (len != 4u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g = p[0];
    k = p[1];
    v = Kmap_GetU16(&p[2]);

    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }
    if (k < 1u || k > (uint8_t)KEY_MAP_KEYS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    KEY_SetMapValue(g, k, v);

    /* 回的负载带上设置后的实际值，上位机可以据此确认 */
    {
        uint8_t pay[4];
        pay[0] = g;
        pay[1] = k;
        Kmap_PutU16(&pay[2], KEY_GetMapValue(g, k));
        return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_SET_KEY), seq, pay, 4u);
    }
}

/* 0x12 SET_GROUP：负载 19 字节 [group, 9×uint16] */
static uint16_t kmap_do_set_group(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t g;
    uint8_t k;
    uint16_t pos;

    if (len != (uint16_t)(1u + (KMAP_KEYS * 2u)))
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g = p[0];
    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    pos = 1u;
    for (k = 1u; k <= (uint8_t)KEY_MAP_KEYS; k++)
    {
        KEY_SetMapValue(g, k, Kmap_GetU16(&p[pos]));
        pos = (uint16_t)(pos + 2u);
    }

    return kmap_reply_ok(KMAP_CMD_SET_GROUP, seq, out);
}

/* 0x13 GET_NAME：负载 1 字节组号，应答 17 字节名字 */
static uint16_t kmap_do_get_name(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t  pay[KMAP_STORE_NAME_SLOT];
    const char *nm;
    uint8_t  g;

    if (len != 1u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g = p[0];
    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    memset(pay, 0, sizeof(pay));
    nm = KmapStore_GroupName(g);
    if (nm != 0)
    {
        strncpy((char *)pay, nm, (size_t)KMAP_STORE_NAME_MAX);
        pay[KMAP_STORE_NAME_MAX] = '\0';
    }

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_GET_NAME), seq, pay, (uint16_t)sizeof(pay));
}

/* 0x14 SET_NAME：负载 [group, name...]（name 不超过 16 字节，可无结尾 0） */
static uint16_t kmap_do_set_name(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    char    tmp[KMAP_STORE_NAME_SLOT];
    uint8_t g;
    uint16_t n;
    uint8_t rc;

    if (len < 1u || len > (uint16_t)(1u + KMAP_STORE_NAME_MAX))
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g = p[0];
    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    n = (uint16_t)(len - 1u);
    memset(tmp, 0, sizeof(tmp));

    /* 过滤掉不可打印字符，避免上位机塞控制字符进来 */
    {
        uint16_t i;
        uint16_t w = 0u;
        for (i = 0u; i < n; i++)
        {
            uint8_t ch = p[1u + i];
            if (ch >= 0x20u && ch < 0x7Fu)
            {
                tmp[w] = (char)ch;
                w++;
                if (w >= (uint16_t)KMAP_STORE_NAME_MAX)
                {
                    break;
                }
            }
        }
        tmp[KMAP_STORE_NAME_MAX] = '\0';
    }

    rc = KmapStore_SetGroupName(g, tmp);
    if (rc != KMAP_STORE_OK)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    /* 回读实际存下的名字 */
    {
        uint8_t pay[KMAP_STORE_NAME_SLOT];
        memset(pay, 0, sizeof(pay));
        strncpy((char *)pay, KmapStore_GroupName(g), (size_t)KMAP_STORE_NAME_MAX);
        return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_SET_NAME), seq, pay, (uint16_t)sizeof(pay));
    }
}

/* 0x15 GET_ENCMAP：负载 1 字节 [group]，应答 3×uint16 [正转, 反转, 单击] */
static uint16_t kmap_do_get_encmap(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t  pay[KMAP_ENC_SLOTS * 2u];
    uint8_t  g;
    uint8_t  i;
    uint16_t pos;

    if (len != 1u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g = p[0];
    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    pos = 0u;
    for (i = 1u; i <= (uint8_t)KEY_ENC_SLOTS; i++)
    {
        Kmap_PutU16(&pay[pos], KEY_GetEncMapValue(g, i));
        pos = (uint16_t)(pos + 2u);
    }

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_GET_ENCMAP), seq, pay, (uint16_t)sizeof(pay));
}

/* 0x16 SET_ENCKEY：负载 4 字节 [group, which, val_lo, val_hi] */
static uint16_t kmap_do_set_enckey(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t  g;
    uint8_t  which;
    uint16_t v;

    if (len != 4u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    g     = p[0];
    which = p[1];
    v     = Kmap_GetU16(&p[2]);

    if (g >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }
    if ((which < 1u) || (which > (uint8_t)KEY_ENC_SLOTS))
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    KEY_SetEncMapValue(g, which, v);

    {
        uint8_t pay[4];
        pay[0] = g;
        pay[1] = which;
        Kmap_PutU16(&pay[2], KEY_GetEncMapValue(g, which));
        return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_SET_ENCKEY), seq, pay, 4u);
    }
}

/* 0x20 SET_GROUP_IDX：负载 1 字节 */
static uint16_t kmap_do_set_group_idx(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t pay[1];

    if (len != 1u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }
    if (p[0] >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    KEY_SetMapGroup(p[0]);

    pay[0] = KEY_GetMapGroup();
    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_SET_GROUP_IDX), seq, pay, 1u);
}

/* 0x30 SAVE_FLASH */
static uint16_t kmap_do_save(uint8_t seq, uint8_t *out)
{
    uint8_t rc = KmapStore_Save();

    if (rc != KMAP_STORE_OK)
    {
        return kmap_reply_err(seq, KMAP_E_FLASH, out);
    }
    return kmap_reply_ok(KMAP_CMD_SAVE_FLASH, seq, out);
}

/* 0x31 LOAD_FLASH */
static uint16_t kmap_do_load(uint8_t seq, uint8_t *out)
{
    uint8_t pay[1];

    pay[0] = KmapStore_Load() ? 1u : 0u;
    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_LOAD_FLASH), seq, pay, 1u);
}

/* 0x32 RESET_DEFAULT */
static uint16_t kmap_do_reset(uint8_t seq, uint8_t *out)
{
    uint8_t rc = KmapStore_ResetDefault();

    if (rc != KMAP_STORE_OK)
    {
        return kmap_reply_err(seq, KMAP_E_FLASH, out);
    }
    return kmap_reply_ok(KMAP_CMD_RESET_DEFAULT, seq, out);
}

/*==========================================================================
 * 遥控模式三张表（0x40~0x47）
 *
 * 表本体在 rf24/rc_table.c（数据 Flash 双 bank）。这里只做参数搬运：
 * 一帧一条记录，不做分片（最长 31 B，远小于 KMAP_MAX_PAYLOAD=64）。
 *
 * 【性能提醒】SET_* 每条都会整块重写一个 bank（擦 + 8 页写，几十毫秒）。
 * 网页侧不要"边填边发"，攒到用户点保存时再写。
 *========================================================================*/

/* 0x40 GET_COUNTS：[] -> [cat_cnt, tbl_cnt, bind_cnt] */
static uint16_t kmap_do_rc_counts(uint8_t seq, uint8_t *out)
{
    uint8_t pay[3];

    pay[0] = RcTable_CatalogCount();
    pay[1] = RcTable_TblCount();
    pay[2] = RcTable_BindCount();
    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_GET_COUNTS), seq, pay, 3u);
}

/* 0x41 GET_CATALOG：[idx] -> [id, name[11]] */
static uint16_t kmap_do_rc_get_cat(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    rc_act_entry_t e;
    uint8_t        pay[1u + RC_ACT_NAME_MAX + 1u];

    if (len < 1u) { return kmap_reply_err(seq, KMAP_E_LEN, out); }
    if (RcTable_CatalogAt(p[0], &e) != 0u) { return kmap_reply_err(seq, KMAP_E_ARG, out); }

    memset(pay, 0, sizeof(pay));
    pay[0] = e.id;
    strncpy((char *)&pay[1], e.name, RC_ACT_NAME_MAX);

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_GET_CATALOG), seq,
                           pay, (uint16_t)sizeof(pay));
}

/* 0x42 SET_CATALOG：[id, name[11]] -> 回读 [id, name[11]] */
static uint16_t kmap_do_rc_set_cat(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    char    nm[RC_ACT_NAME_MAX + 1u];
    uint16_t n;

    if (len < 2u) { return kmap_reply_err(seq, KMAP_E_LEN, out); }

    n = (uint16_t)(len - 1u);
    if (n > (uint16_t)RC_ACT_NAME_MAX) { n = (uint16_t)RC_ACT_NAME_MAX; }

    memset(nm, 0, sizeof(nm));
    memcpy(nm, &p[1], n);
    nm[RC_ACT_NAME_MAX] = 0;

    if (RcTable_SetName(p[0], nm) != 0) { return kmap_reply_err(seq, KMAP_E_ARG, out); }

    {
        uint8_t pay[1u + RC_ACT_NAME_MAX + 1u];

        memset(pay, 0, sizeof(pay));
        pay[0] = p[0];
        (void)RcTable_NameOfId(p[0], (char *)&pay[1], (uint8_t)(RC_ACT_NAME_MAX + 1u));
        return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_SET_CATALOG), seq,
                               pay, (uint16_t)sizeof(pay));
    }
}

/* 0x43 GET_TBL：[idx] -> [name[11], key_num[12]] */
static uint16_t kmap_do_rc_get_tbl(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    rc_act_tbl_t t;
    uint8_t      pay[(RC_ACT_TBL_NAME_MAX + 1u) + RC_OP_COUNT];

    if (len < 1u) { return kmap_reply_err(seq, KMAP_E_LEN, out); }
    if (RcTable_TblAt(p[0], &t) != 0u) { return kmap_reply_err(seq, KMAP_E_ARG, out); }

    memset(pay, 0, sizeof(pay));
    strncpy((char *)pay, t.name, RC_ACT_TBL_NAME_MAX);
    memcpy(&pay[RC_ACT_TBL_NAME_MAX + 1u], t.key_num, RC_OP_COUNT);

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_GET_TBL), seq,
                           pay, (uint16_t)sizeof(pay));
}

/* 0x44 SET_TBL：[name[11], key_num[12]] -> 回读 同格式 */
static uint16_t kmap_do_rc_set_tbl(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    rc_act_tbl_t t;
    uint8_t      pay[(RC_ACT_TBL_NAME_MAX + 1u) + RC_OP_COUNT];

    if (len < (uint16_t)((RC_ACT_TBL_NAME_MAX + 1u) + RC_OP_COUNT))
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    memset(&t, 0, sizeof(t));
    memcpy(t.name, p, RC_ACT_TBL_NAME_MAX);
    t.name[RC_ACT_TBL_NAME_MAX] = 0;
    memcpy(t.key_num, &p[RC_ACT_TBL_NAME_MAX + 1u], RC_OP_COUNT);

    if (RcTable_SetTbl(&t) != 0) { return kmap_reply_err(seq, KMAP_E_ARG, out); }

    memset(pay, 0, sizeof(pay));
    memcpy(pay, t.name, RC_ACT_TBL_NAME_MAX);
    memcpy(&pay[RC_ACT_TBL_NAME_MAX + 1u], t.key_num, RC_OP_COUNT);
    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_SET_TBL), seq,
                           pay, (uint16_t)sizeof(pay));
}

/* 0x45 GET_BIND：[idx] -> [dev_name[20], tbl_name[11]] */
static uint16_t kmap_do_rc_get_bind(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    rc_dev_bind_t b;
    uint8_t       pay[(RC_DEV_NAME_MAX + 1u) + (RC_ACT_TBL_NAME_MAX + 1u)];

    if (len < 1u) { return kmap_reply_err(seq, KMAP_E_LEN, out); }
    if (RcTable_BindAt(p[0], &b) != 0u) { return kmap_reply_err(seq, KMAP_E_ARG, out); }

    memset(pay, 0, sizeof(pay));
    strncpy((char *)pay, b.dev_name, RC_DEV_NAME_MAX);
    strncpy((char *)&pay[RC_DEV_NAME_MAX + 1u], b.tbl_name, RC_ACT_TBL_NAME_MAX);

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_GET_BIND), seq,
                           pay, (uint16_t)sizeof(pay));
}

/* 0x46 SET_BIND：[dev_name[20], tbl_name[11]] -> 回读 同格式 */
static uint16_t kmap_do_rc_set_bind(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    char    dn[RC_DEV_NAME_MAX + 1u];
    char    tn[RC_ACT_TBL_NAME_MAX + 1u];
    uint8_t pay[(RC_DEV_NAME_MAX + 1u) + (RC_ACT_TBL_NAME_MAX + 1u)];

    if (len < (uint16_t)((RC_DEV_NAME_MAX + 1u) + (RC_ACT_TBL_NAME_MAX + 1u)))
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }

    memset(dn, 0, sizeof(dn));
    memset(tn, 0, sizeof(tn));
    memcpy(dn, p, RC_DEV_NAME_MAX);
    memcpy(tn, &p[RC_DEV_NAME_MAX + 1u], RC_ACT_TBL_NAME_MAX);

    if (RcTable_SetBind(dn, tn) != 0) { return kmap_reply_err(seq, KMAP_E_ARG, out); }

    memset(pay, 0, sizeof(pay));
    strncpy((char *)pay, dn, RC_DEV_NAME_MAX);
    strncpy((char *)&pay[RC_DEV_NAME_MAX + 1u], tn, RC_ACT_TBL_NAME_MAX);
    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_RC_SET_BIND), seq,
                           pay, (uint16_t)sizeof(pay));
}

/* 0x47 SAVE：[] -> 空应答（SET_* 其实已经落盘了，这条留给"整表写完再确认一次"） */
static uint16_t kmap_do_rc_save(uint8_t seq, uint8_t *out)
{
    return kmap_reply_ok(KMAP_CMD_RC_SAVE, seq, out);
}

/*==========================================================================
 * 入口
 *========================================================================*/
void KmapServer_Init(void)
{
    s_rx_frames  = 0u;
    s_err_frames = 0u;
}

/*==========================================================================
 * 键盘宏（0x50~0x54）
 *========================================================================*/
static uint16_t kmap_do_macro_get_bind(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t pay[9];

    if (len != 1u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }
    if (p[0] >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    Macro_GetBindRow(p[0], pay);
    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_MACRO_GET_BIND), seq, pay, 9u);
}

static uint16_t kmap_do_macro_set_bind(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    if (len != 10u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }
    if (p[0] >= (uint8_t)KEY_MAP_GROUPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    Macro_SetBindRow(p[0], &p[1]);
    return kmap_reply_ok(KMAP_CMD_MACRO_SET_BIND, seq, out);
}

static uint16_t kmap_do_macro_get(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t pay[1u + (MACRO_STEPS * 2u)];
    uint8_t i;

    if (len != 1u)
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }
    if ((p[0] < 1u) || (p[0] > (uint8_t)MACRO_SLOTS))
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    pay[0] = Macro_GetLen(p[0]);
    for (i = 0u; i < (uint8_t)MACRO_STEPS; i++)
    {
        Kmap_PutU16(&pay[1u + (i * 2u)], Macro_GetStep(p[0], i));
    }

    return Kmap_BuildFrame(out, KMAP_ACK(KMAP_CMD_MACRO_GET), seq, pay,
                           (uint16_t)(1u + (MACRO_STEPS * 2u)));
}

static uint16_t kmap_do_macro_set(uint8_t seq, const uint8_t *p, uint16_t len, uint8_t *out)
{
    uint8_t i;

    if (len != (uint16_t)(2u + (MACRO_STEPS * 2u)))
    {
        return kmap_reply_err(seq, KMAP_E_LEN, out);
    }
    if ((p[0] < 1u) || (p[0] > (uint8_t)MACRO_SLOTS))
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }
    if (p[1] > (uint8_t)MACRO_STEPS)
    {
        return kmap_reply_err(seq, KMAP_E_ARG, out);
    }

    Macro_SetLen(p[0], p[1]);
    for (i = 0u; i < (uint8_t)MACRO_STEPS; i++)
    {
        Macro_SetStep(p[0], i, Kmap_GetU16(&p[2u + (i * 2u)]));
    }

    return kmap_reply_ok(KMAP_CMD_MACRO_SET, seq, out);
}

static uint16_t kmap_do_macro_save(uint8_t seq, uint8_t *out)
{
    if (Macro_Save() != 0u)
    {
        return kmap_reply_err(seq, KMAP_E_FLASH, out);
    }
    return kmap_reply_ok(KMAP_CMD_MACRO_SAVE, seq, out);
}

uint16_t KmapServer_HandleFrame(uint8_t cmd, uint8_t seq,
                                const uint8_t *payload, uint16_t len,
                                uint8_t *out)
{
    if (out == 0)
    {
        return 0u;
    }

    s_rx_frames++;

    switch (cmd)
    {
        case KMAP_CMD_PING:
            return kmap_do_ping(seq, out);

        case KMAP_CMD_GET_INFO:
            return kmap_do_get_info(seq, out);

        case KMAP_CMD_GET_MAP:
            return kmap_do_get_map(seq, payload, len, out);

        case KMAP_CMD_SET_KEY:
            return kmap_do_set_key(seq, payload, len, out);

        case KMAP_CMD_SET_GROUP:
            return kmap_do_set_group(seq, payload, len, out);

        case KMAP_CMD_GET_NAME:
            return kmap_do_get_name(seq, payload, len, out);

        case KMAP_CMD_SET_NAME:
            return kmap_do_set_name(seq, payload, len, out);

        case KMAP_CMD_GET_ENCMAP:
            return kmap_do_get_encmap(seq, payload, len, out);

        case KMAP_CMD_SET_ENCKEY:
            return kmap_do_set_enckey(seq, payload, len, out);

        case KMAP_CMD_SET_GROUP_IDX:
            return kmap_do_set_group_idx(seq, payload, len, out);

        case KMAP_CMD_SAVE_FLASH:
            return kmap_do_save(seq, out);

        case KMAP_CMD_LOAD_FLASH:
            return kmap_do_load(seq, out);

        case KMAP_CMD_RESET_DEFAULT:
            return kmap_do_reset(seq, out);

        /* ---- 遥控模式三张表（见 遥控模式开发文档.md §4）---- */
        case KMAP_CMD_RC_GET_COUNTS:
            return kmap_do_rc_counts(seq, out);

        case KMAP_CMD_RC_GET_CATALOG:
            return kmap_do_rc_get_cat(seq, payload, len, out);

        case KMAP_CMD_RC_SET_CATALOG:
            return kmap_do_rc_set_cat(seq, payload, len, out);

        case KMAP_CMD_RC_GET_TBL:
            return kmap_do_rc_get_tbl(seq, payload, len, out);

        case KMAP_CMD_RC_SET_TBL:
            return kmap_do_rc_set_tbl(seq, payload, len, out);

        case KMAP_CMD_RC_GET_BIND:
            return kmap_do_rc_get_bind(seq, payload, len, out);

        case KMAP_CMD_RC_SET_BIND:
            return kmap_do_rc_set_bind(seq, payload, len, out);

        case KMAP_CMD_MACRO_GET_BIND:
            return kmap_do_macro_get_bind(seq, payload, len, out);

        case KMAP_CMD_MACRO_SET_BIND:
            return kmap_do_macro_set_bind(seq, payload, len, out);

        case KMAP_CMD_MACRO_GET:
            return kmap_do_macro_get(seq, payload, len, out);

        case KMAP_CMD_MACRO_SET:
            return kmap_do_macro_set(seq, payload, len, out);

        case KMAP_CMD_MACRO_SAVE:
            return kmap_do_macro_save(seq, out);

        case KMAP_CMD_RC_SAVE:
            return kmap_do_rc_save(seq, out);

        default:
            return kmap_reply_err(seq, KMAP_E_CMD, out);
    }
}

uint32_t KmapServer_RxFrames(void)
{
    return s_rx_frames;
}

uint32_t KmapServer_ErrorFrames(void)
{
    return s_err_frames;
}
