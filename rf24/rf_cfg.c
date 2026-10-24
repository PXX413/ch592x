/**************************************************************************
 * rf_cfg.c —— 配置持久化实现
 **************************************************************************/

#include "rf_cfg.h"
#include "CH59x_common.h"
#include <string.h>

/* 数据 Flash 页擦除（没有 EEPROM_ERASE 宏，直接走底层命令） */
#define RF_CFG_PAGE_ERASE(off)  FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, (off), 0, RF_CFG_PAGE_LEN)

/*==========================================================================
 * RC 记录的字段偏移（规范 11.2）
 *========================================================================*/
#define RC_OFF_MAGIC            0u
#define RC_OFF_VER              2u
#define RC_OFF_DEVID            3u
#define RC_OFF_ADDR             4u
#define RC_OFF_SEED             9u
#define RC_OFF_HWID             11u
#define RC_OFF_HWVER            15u
#define RC_OFF_SWVER            16u
#define RC_OFF_NAMELEN          17u
#define RC_OFF_NAME             18u
#define RC_OFF_KEYMASK          0xF5u
#define RC_OFF_CRC              0xFEu
#define RC_OFF_RESERVED         0xF6u

#define RC_REC_LEN              256u

/*==========================================================================
 * KB 记录的字段偏移（规范 11.1）
 *========================================================================*/
#define KB_OFF_MAGIC            0u
#define KB_OFF_PAIRED           2u
#define KB_OFF_ADDR             3u
#define KB_OFF_CHANNEL          8u
#define KB_OFF_HOP              9u
#define KB_OFF_NKRO             12u
#define KB_OFF_LED              13u
#define KB_OFF_CRC              14u
#define KB_REC_LEN              32u

/*==========================================================================
 * 底层
 *========================================================================*/
uint16_t RfCfg_Crc(const uint8_t *p, uint16_t n)
{
    return Rf_Crc16(p, n);
}

void RfCfg_PageWrite(uint16_t off, const uint8_t *data, uint16_t len)
{
    uint32_t irq;

    if (data == 0)
    {
        return;
    }
    if (len > (uint16_t)RF_CFG_PAGE_LEN)
    {
        len = (uint16_t)RF_CFG_PAGE_LEN;
    }

    /* 擦写数据 Flash 期间关中断（ROM 例程执行时 CPU 会等） */
    irq = __risc_v_disable_irq();

    RF_CFG_PAGE_ERASE(off);
    /* SDK 的 FLASH_EEPROM_CMD 收 void*，这里显式去掉 const（它只读不写） */
    EEPROM_WRITE(off, (void *)(uintptr_t)data, (uint32_t)len);

    __risc_v_enable_irq(irq);
}

void RfCfg_PageRead(uint16_t off, uint8_t *data, uint16_t len)
{
    if (data == 0)
    {
        return;
    }
    if (len > (uint16_t)RF_CFG_PAGE_LEN)
    {
        len = (uint16_t)RF_CFG_PAGE_LEN;
    }

    EEPROM_READ(off, data, (uint32_t)len);
}

/*==========================================================================
 * 遥控配置：打包 / 解包
 *========================================================================*/
void RfCfg_RcDefault(rf_cfg_rc_t *cfg)
{
    if (cfg == 0)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->config_ver = 1u;
    cfg->dev_id     = 0u;
    cfg->pair_seed  = 0x0Bu;
    cfg->hw_ver     = 1u;
    cfg->sw_ver     = PROTO_VERSION_BYTE;

    Rf_MakeRcDevAddr(cfg->tx_addr, 0u);     /* 先听枚举地址 */

    Keymap_Default(&cfg->km, "rc_dev");
    cfg->key_mask = cfg->km.key_mask;
    cfg->valid    = 0u;
}

uint16_t RfCfg_RcPack(const rf_cfg_rc_t *cfg, uint8_t *out, uint16_t cap)
{
    if ((cfg == 0) || (out == 0) || (cap < (uint16_t)RC_REC_LEN))
    {
        return 0u;
    }

    memset(out, 0, RC_REC_LEN);

    Rf_PutU16LE(&out[RC_OFF_MAGIC], (uint16_t)RF_CFG_MAGIC_RC);
    out[RC_OFF_VER]  = cfg->config_ver;
    out[RC_OFF_DEVID]= cfg->dev_id;
    memcpy(&out[RC_OFF_ADDR], cfg->tx_addr, RF_ADDR_LEN);
    Rf_PutU16LE(&out[RC_OFF_SEED], cfg->pair_seed);
    memcpy(&out[RC_OFF_HWID], cfg->hw_id, 4u);
    out[RC_OFF_HWVER] = cfg->hw_ver;
    out[RC_OFF_SWVER] = cfg->sw_ver;

    {
        uint8_t n = cfg->km.dev_name_len;
        if (n > (uint8_t)RC_DEV_NAME_MAX)
        {
            n = (uint8_t)RC_DEV_NAME_MAX;
        }
        out[RC_OFF_NAMELEN] = n;
        memcpy(&out[RC_OFF_NAME], cfg->km.dev_name, n);
    }

    Keymap_PackNames(&cfg->km, &out[RC_FLASH_NAME_OFF]);

    out[RC_OFF_KEYMASK] = (uint8_t)(cfg->key_mask & 0xFFu);
    /* reserved 第 1 字节的 bit0 = key_mask 的第 9 位（规范里 reserved 是 0） */
    out[RC_OFF_RESERVED] = (uint8_t)((cfg->key_mask >> 8) & 0x01u);

    Rf_PutU16LE(&out[RC_OFF_CRC], Rf_Crc16(out, (uint16_t)RC_OFF_CRC));

    return (uint16_t)RC_REC_LEN;
}

bool RfCfg_RcUnpack(const uint8_t *in, uint16_t len, rf_cfg_rc_t *cfg)
{
    uint16_t crc;
    uint8_t  n;

    if ((in == 0) || (cfg == 0) || (len < (uint16_t)RC_REC_LEN))
    {
        return false;
    }

    if (Rf_GetU16LE(&in[RC_OFF_MAGIC]) != (uint16_t)RF_CFG_MAGIC_RC)
    {
        return false;
    }

    crc = Rf_Crc16(in, (uint16_t)RC_OFF_CRC);
    if (crc != Rf_GetU16LE(&in[RC_OFF_CRC]))
    {
        return false;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->config_ver = in[RC_OFF_VER];
    cfg->dev_id     = in[RC_OFF_DEVID];
    if (cfg->dev_id > (uint8_t)RF_RC_MAX_DEV)
    {
        cfg->dev_id = 0u;                   /* 越界 = 没枚举 */
    }
    memcpy(cfg->tx_addr, &in[RC_OFF_ADDR], RF_ADDR_LEN);
    cfg->pair_seed = Rf_GetU16LE(&in[RC_OFF_SEED]);
    memcpy(cfg->hw_id, &in[RC_OFF_HWID], 4u);
    cfg->hw_ver = in[RC_OFF_HWVER];
    cfg->sw_ver = in[RC_OFF_SWVER];

    n = in[RC_OFF_NAMELEN];
    if (n > (uint8_t)RC_DEV_NAME_MAX)
    {
        n = (uint8_t)RC_DEV_NAME_MAX;       /* 防越界 */
    }
    memcpy(cfg->km.dev_name, &in[RC_OFF_NAME], n);
    cfg->km.dev_name[n] = '\0';
    cfg->km.dev_name_len = n;

    Keymap_UnpackNames(&in[RC_FLASH_NAME_OFF], &cfg->km);

    cfg->km.config_ver = cfg->config_ver;
    cfg->key_mask      = (uint16_t)((uint16_t)in[RC_OFF_KEYMASK]
                                     | ((uint16_t)(in[RC_OFF_RESERVED] & 0x01u) << 8));
    cfg->valid         = 1u;

    if (cfg->km.config_ver == 0u)
    {
        cfg->km.config_ver = 1u;
    }

    return true;
}

/*==========================================================================
 * 遥控配置：双 Bank 读写
 *========================================================================*/
/*==========================================================================
 * Flash 读写用的静态缓冲区
 *
 * 为什么要放 static：栈总共只有 640 字节（Link.ld: __stack_size = 640），
 * 而下面三个函数原本在栈上开缓冲区：
 *     RfCfg_RcSave  buf[256] + chk[256] + verify -> 实测 784 字节  *** 必崩 ***
 *     RfCfg_KbSave  buf[256] + chk[256] + verify -> 实测 528 字节  *** 必崩 ***
 *     RfCfg_RcLoad  buf[256] + ...                -> 实测 528 字节  *** 必崩 ***
 * 一旦被调用就必然栈溢出 -> HardFault 死机。
 *
 * 改到 static 后这三个函数的栈占用降到几十字节，同时它们**不是可重入**的
 * （共享同一块缓冲区）。当前调用方都是单线程主循环，安全。
 *==========================================================================*/
static uint8_t s_rc_buf[RC_REC_LEN];      /* RcSave 打包 / RcLoad 读取 */
static uint8_t s_rc_chk[RC_REC_LEN];      /* RcSave 读回校验 */
static uint8_t s_kb_buf[RF_CFG_PAGE_LEN]; /* KbSave 打包 */
static uint8_t s_kb_chk[RF_CFG_PAGE_LEN]; /* KbSave 读回校验 */
static rf_cfg_rc_t s_rc_verify;           /* RcSave 读回校验用（结构体约 256B）*/
static rf_cfg_rc_t s_rc_tmp;              /* RcLoad 读取 B bank 用 */

/* 手动零填充：不用 memset。
 * memset(buf, 0, 256) 会被编译器内联成一条往栈上展开 256 字节缓冲的循环，
 * 实测让 RfCfg_RcSave 从 16 字节暴涨到 288 字节（栈总共才 640 字节）。
 * 自己写循环则完全不需要额外栈。 */
static void rf_zero(uint8_t *p, uint16_t n)
{
    while (n != 0u)
    {
        *p++ = 0u;
        n--;
    }
}

static void rf_fill_ff(uint8_t *p, uint16_t n)
{
    while (n != 0u)
    {
        *p++ = 0xFFu;
        n--;
    }
}

/* 手动比较：不用 memcmp。
 * memcmp(a, b, 256) 在 -Os 下同样被内联成往栈上展开缓冲的循环，
 * 和 memset 一样会把函数栈撑到几百字节。 */
static bool rf_equal(const uint8_t *a, const uint8_t *b, uint16_t n)
{
    while (n != 0u)
    {
        if (*a != *b)
        {
            return false;
        }
        a++;
        b++;
        n--;
    }
    return true;
}


bool RfCfg_RcLoad(rf_cfg_rc_t *cfg)
{
    uint8_t  okA;
    uint8_t  okB;

    if (cfg == 0)
    {
        return false;
    }

    rf_zero(s_rc_buf, (uint16_t)sizeof(s_rc_buf));
    RfCfg_PageRead((uint16_t)RF_CFG_PAGE_A_OFF, s_rc_buf, (uint16_t)RC_REC_LEN);
    okA = RfCfg_RcUnpack(s_rc_buf, (uint16_t)RC_REC_LEN, cfg) ? 1u : 0u;

    rf_zero(s_rc_buf, (uint16_t)sizeof(s_rc_buf));
    RfCfg_PageRead((uint16_t)RF_CFG_PAGE_B_OFF, s_rc_buf, (uint16_t)RC_REC_LEN);
    {
        /* B 更 "新"（config_ver 更大）则采纳 B */
        okB = RfCfg_RcUnpack(s_rc_buf, (uint16_t)RC_REC_LEN, &s_rc_tmp) ? 1u : 0u;
        if ((okB != 0u) && ((okA == 0u) || (s_rc_tmp.config_ver > cfg->config_ver)))
        {
            *cfg = s_rc_tmp;
            return true;
        }
    }

    if (okA != 0u)
    {
        return true;
    }

    RfCfg_RcDefault(cfg);                   /* 两个 bank 都坏：回默认值 */
    return false;
}

uint8_t RfCfg_RcSave(const rf_cfg_rc_t *cfg)
{
    if (cfg == 0)
    {
        return RC_E_PARAM;
    }

    if (RfCfg_RcPack(cfg, s_rc_buf, (uint16_t)sizeof(s_rc_buf)) == 0u)
    {
        return RC_E_PARAM;
    }

    /* 先写 A，再写 B；任一步掉电，另一个 bank 还是好的 */
    /* 双写：写 A 再写 B，双保险；掉电后任一 bank 都能读回 */
    RfCfg_PageWrite((uint16_t)RF_CFG_PAGE_A_OFF, s_rc_buf, (uint16_t)RC_REC_LEN);
    RfCfg_PageWrite((uint16_t)RF_CFG_PAGE_B_OFF, s_rc_buf, (uint16_t)RC_REC_LEN);

    /* 读回校验 */
    rf_zero(s_rc_chk, (uint16_t)sizeof(s_rc_chk));
    RfCfg_PageRead((uint16_t)RF_CFG_PAGE_A_OFF, s_rc_chk, (uint16_t)RC_REC_LEN);
    if (!rf_equal(s_rc_chk, s_rc_buf, (uint16_t)RC_REC_LEN))
    {
        return RC_E_FLASH;
    }
    if (!RfCfg_RcUnpack(s_rc_chk, (uint16_t)RC_REC_LEN, &s_rc_verify))
    {
        return RC_E_FLASH;
    }

    return RC_OK;
}

/*==========================================================================
 * 键盘配对记录
 *========================================================================*/
void RfCfg_KbDefault(rf_cfg_kb_t *cfg)
{
    if (cfg == 0)
    {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->paired    = 0u;
    cfg->addr[0]   = (uint8_t)RF_ADDR_KB_PREFIX0;
    cfg->addr[1]   = (uint8_t)RF_ADDR_KB_PREFIX1;
    cfg->addr[2]   = (uint8_t)RF_ADDR_KB_PREFIX2;
    cfg->addr[3]   = (uint8_t)RF_ADDR_KB_PREFIX3;
    cfg->addr[4]   = 0x3Fu;
    cfg->channel   = hop_kb_table[0];
    cfg->hop[0]    = hop_kb_table[0];
    cfg->hop[1]    = hop_kb_table[1];
    cfg->hop[2]    = hop_kb_table[2];
    cfg->nkro_en   = 0u;
    cfg->led_state = 0u;
    cfg->valid     = 0u;
}

static uint16_t rf_cfg_kb_pack(const rf_cfg_kb_t *cfg, uint8_t *out, uint16_t cap)
{
    if ((cfg == 0) || (out == 0) || (cap < (uint16_t)KB_REC_LEN))
    {
        return 0u;
    }

    memset(out, 0, KB_REC_LEN);

    Rf_PutU16LE(&out[KB_OFF_MAGIC], (uint16_t)RF_CFG_MAGIC_KB);
    out[KB_OFF_PAIRED]  = cfg->paired;
    memcpy(&out[KB_OFF_ADDR], cfg->addr, RF_ADDR_LEN);
    out[KB_OFF_CHANNEL] = cfg->channel;
    out[KB_OFF_HOP]     = cfg->hop[0];
    out[KB_OFF_HOP + 1u]= cfg->hop[1];
    out[KB_OFF_HOP + 2u]= cfg->hop[2];
    out[KB_OFF_NKRO]    = cfg->nkro_en;
    out[KB_OFF_LED]     = cfg->led_state;

    Rf_PutU16LE(&out[KB_OFF_CRC], Rf_Crc16(out, (uint16_t)KB_OFF_CRC));

    return (uint16_t)KB_REC_LEN;
}

static bool rf_cfg_kb_unpack(const uint8_t *in, uint16_t len, rf_cfg_kb_t *cfg)
{
    if ((in == 0) || (cfg == 0) || (len < (uint16_t)KB_REC_LEN))
    {
        return false;
    }

    if (Rf_GetU16LE(&in[KB_OFF_MAGIC]) != (uint16_t)RF_CFG_MAGIC_KB)
    {
        return false;
    }
    if (Rf_Crc16(in, (uint16_t)KB_OFF_CRC) != Rf_GetU16LE(&in[KB_OFF_CRC]))
    {
        return false;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->paired    = in[KB_OFF_PAIRED];
    memcpy(cfg->addr, &in[KB_OFF_ADDR], RF_ADDR_LEN);
    cfg->channel   = in[KB_OFF_CHANNEL];
    if (cfg->channel > (uint8_t)RF_MAX_CHANNEL)
    {
        cfg->channel = hop_kb_table[0];
    }
    /* 【修复 P0-2 余项】Flash 里的跳频值也要校验：原来原样读出，
     * >41 的非法信道会一路交给 HALRF_SetChannel() —— 要么发到 ISM 频段外，
     * 要么被静默丢弃、射频停在旧频点。越界的一律退回内置跳频表的对应值。 */
    cfg->hop[0]    = in[KB_OFF_HOP];
    cfg->hop[1]    = in[KB_OFF_HOP + 1u];
    cfg->hop[2]    = in[KB_OFF_HOP + 2u];
    {
        uint8_t k;

        for (k = 0u; k < (uint8_t)HOP_KB_COUNT; k++)
        {
            if (cfg->hop[k] > (uint8_t)RF_MAX_CHANNEL)
            {
                cfg->hop[k] = hop_kb_table[k];
            }
        }
    }
    cfg->nkro_en   = in[KB_OFF_NKRO];
    cfg->led_state = in[KB_OFF_LED];
    cfg->valid     = 1u;

    return true;
}

bool RfCfg_KbLoad(rf_cfg_kb_t *cfg)
{
    if (cfg == 0)
    {
        return false;
    }

    rf_zero(s_kb_buf, (uint16_t)KB_REC_LEN);
    /* 键盘对码记录走独立页 0x0500，不再和 RC 的 A bank（0x0100）共用：
     * 【修复 P2-4】原来 KB 记录整页写在 0x0100（256 字节擦 + 写），
     * 而 RC 的 A bank 也在 0x0100 —— 存一次键盘对码就把 RC 记录抹了。 */
    RfCfg_PageRead((uint16_t)RF_CFG_PAGE_KB_OFF, s_kb_buf, (uint16_t)KB_REC_LEN);

    if (rf_cfg_kb_unpack(s_kb_buf, (uint16_t)KB_REC_LEN, cfg))
    {
        return true;
    }

    RfCfg_KbDefault(cfg);
    return false;
}

uint8_t RfCfg_KbSave(const rf_cfg_kb_t *cfg)
{
    rf_cfg_kb_t verify;

    if (cfg == 0)
    {
        return RC_E_PARAM;
    }

    rf_fill_ff(s_kb_buf, (uint16_t)sizeof(s_kb_buf));
    if (rf_cfg_kb_pack(cfg, s_kb_buf, (uint16_t)sizeof(s_kb_buf)) == 0u)
    {
        return RC_E_PARAM;
    }

    RfCfg_PageWrite((uint16_t)RF_CFG_PAGE_KB_OFF, s_kb_buf, (uint16_t)RF_CFG_PAGE_LEN);

    rf_zero(s_kb_chk, (uint16_t)sizeof(s_kb_chk));
    RfCfg_PageRead((uint16_t)RF_CFG_PAGE_KB_OFF, s_kb_chk, (uint16_t)RF_CFG_PAGE_LEN);

    if (!rf_cfg_kb_unpack(s_kb_chk, (uint16_t)RF_CFG_PAGE_LEN, &verify))
    {
        return RC_E_FLASH;
    }

    return RC_OK;
}
