/**************************************************************************
 * rc_table.c —— 遥控模式三张配置表的实现
 *
 * 【RAM 账】本文件只占 .bss 256 字节（一块分块搬运缓冲），
 *   三张表本身**完全留在 Flash**，不在 RAM 里留副本。
 *   （全量副本要 1.8 KB，这个工程 RAM 只有 26 KB、已经用掉 89%。）
 *
 * 【掉电安全】双 bank 乒乓：
 *   每次保存都往"另一个" bank 整块重写，旧 bank 全程保持完好；
 *   写完读回校验 CRC，通过才算成功。所以"擦一半掉电"最多
 *   丢最后一次改动，绝不会读出乱码。
 *
 * 【写入代价】一次 Set 会整块重写一个 bank（约 1.8 KB，8 个 256 B 页）。
 *   配置是低频操作（网页点一下），几毫秒完全可以接受；
 *   换来的是"只用 256 B RAM" + "任何时刻掉电都不坏表"。
 **************************************************************************/

#include "rc_table.h"
#include "CH59x_common.h"
#include <string.h>

/*==========================================================================
 * 1. 数据 Flash 分区
 *   （和 ui_store 0x0300 / kmap_store 0x0400 / rf_cfg 0x0500 同一片，
 *    数据 Flash 总 32 KB，见 ISP592.h 的 EEPROM_MAX_SIZE = 0x8000）
 *========================================================================*/
#define RC_TBL_BANK_A_OFF   0x0600u
#define RC_TBL_BANK_B_OFF   0x0E00u
#define RC_TBL_BANK_SIZE    2048u
#define RC_TBL_PAGE         256u
#define RC_TBL_PAGES        (RC_TBL_BANK_SIZE / RC_TBL_PAGE)

/*==========================================================================
 * 2. bank 内的字节布局（显式偏移，不依赖结构体对齐）
 *
 *   0x000  magic 'R','C'
 *   0x002  ver       格式版本
 *   0x003  gen       代数，每次保存 +1（用来判断哪个 bank 更新）
 *   0x004  cat_count 动作目录条数 + 3 字节保留
 *   0x008  动作目录   CAT_MAX × 12 B      -> 0x308
 *   0x308  tbl_count 动作编号表张数 + 3 字节保留
 *   0x30C  动作编号表 TBL_MAX × 24 B      -> 0x48C
 *   0x48C  bind_count 设备绑定条数 + 3 字节保留
 *   0x490  设备绑定表 BIND_MAX × 32 B     -> 0x710
 *   0x710  crc16     CRC-16/CCITT-FALSE，覆盖 0x002..0x70F
 *========================================================================*/
#define RC_TBL_MAGIC0       0x52u       /* 'R' */
#define RC_TBL_MAGIC1       0x43u       /* 'C' */
#define RC_TBL_VER          1u

/* 单条记录的 Flash 字节数（4 字节对齐，方便整条搬运） */
#define RC_TBL_CAT_LEN      ((1u + (RC_ACT_NAME_MAX + 1u)))                      /* 12 */
#define RC_TBL_TBL_LEN      ((((RC_ACT_TBL_NAME_MAX + 1u) + RC_OP_COUNT) + 3u) & ~3u)   /* 24 */
#define RC_TBL_BIND_LEN     ((((RC_DEV_NAME_MAX + 1u) + (RC_ACT_TBL_NAME_MAX + 1u)) + 3u) & ~3u)

/* 注意：这些偏移宏会被下面的 #if 求值，所以**不能出现强制类型转换**
 * （(uint32_t) 这类转换在预处理表达式里非法）。全部用整数常量表达。 */
#define RC_TBL_CAT_OFF      0x008u
#define RC_TBL_TBL_CNT_OFF  (RC_TBL_CAT_OFF + (RC_ACT_CATALOG_MAX * RC_TBL_CAT_LEN))
#define RC_TBL_TBL_OFF      (RC_TBL_TBL_CNT_OFF + 4u)
#define RC_TBL_BIND_CNT_OFF (RC_TBL_TBL_OFF + (RC_ACT_TBL_MAX * RC_TBL_TBL_LEN))
#define RC_TBL_BIND_OFF     (RC_TBL_BIND_CNT_OFF + 4u)
#define RC_TBL_CRC_OFF      (RC_TBL_BIND_OFF + (RC_DEV_BIND_MAX * RC_TBL_BIND_LEN))

#define RC_TBL_CRC_FROM     0x002u      /* CRC 覆盖起点 */
#define RC_TBL_CRC_LEN      (RC_TBL_CRC_OFF - RC_TBL_CRC_FROM)

#if ((RC_TBL_CRC_OFF + 2u) > RC_TBL_BANK_SIZE)
#error "rc_table: 表数据超过一个 bank 的容量，请缩小上限或加大 RC_TBL_BANK_SIZE"
#endif

/*==========================================================================
 * 3. 模块状态（只占几十字节，不是表的副本）
 *========================================================================*/
static uint8_t s_bank;          /* 0xFF = 还没载入；0 = A；1 = B */
static uint8_t s_gen;           /* 当前 bank 的代数 */

/* 分块搬运缓冲（配置操作期间才用，平时只是占着 .bss） */
static uint8_t s_chunk[RC_TBL_PAGE];

/* 各表的条数（从当前 bank 读出来的，最多三个字节） */
static uint8_t s_cat_cnt;
static uint8_t s_tbl_cnt;
static uint8_t s_bind_cnt;

/*==========================================================================
 * 4. 底层：CRC / 读写 / 擦除
 *========================================================================*/

static uint8_t rc_rd(uint32_t off, void *buf, uint32_t len)
{
    return (EEPROM_READ(off, buf, len) == 0u) ? 0u : 1u;
}

static uint8_t rc_wr(uint32_t off, const void *buf, uint32_t len)
{
    return (EEPROM_WRITE(off, (void *)(uintptr_t)buf, len) == 0u) ? 0u : 1u;
}

static uint8_t rc_erase(uint32_t base, uint32_t len)
{
    return (FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, base, 0, len) == 0u) ? 0u : 1u;
}

static uint32_t rc_bank_base(uint8_t bank)
{
    return (bank == 0u) ? (uint32_t)RC_TBL_BANK_A_OFF : (uint32_t)RC_TBL_BANK_B_OFF;
}

/*==========================================================================
 * 5. 逐字节读写"bank 里第 off 字节"的小工具
 *    写路径走不到这里 —— 写是整块重写，见 rc_bank_flush()
 *========================================================================*/
static uint8_t rc_rd8(uint32_t off)
{
    uint8_t v = 0u;
    (void)rc_rd(off, &v, 1u);
    return v;
}

static void rc_rd_blk(uint32_t off, void *buf, uint32_t len)
{
    (void)rc_rd(off, buf, len);
}

/* 对整块 bank 的 [CRC_FROM, CRC_OFF) 做 CRC-16/CCITT-FALSE。
 * 返回 0 = 读取成功（out_crc 有效），1 = Flash 读失败。 */
static uint8_t rc_bank_crc(uint32_t base, uint16_t *out_crc)
{
    uint16_t crc = 0xFFFFu;
    uint32_t off;

    if (out_crc == 0) { return 1u; }

    for (off = RC_TBL_CRC_FROM; off < RC_TBL_CRC_OFF; off += RC_TBL_PAGE)
    {
        uint32_t n = RC_TBL_CRC_OFF - off;
        uint32_t k;

        if (n > (uint32_t)RC_TBL_PAGE) { n = (uint32_t)RC_TBL_PAGE; }
        if (rc_rd(base + off, s_chunk, n) != 0u) { return 1u; }

        for (k = 0u; k < n; k++)
        {
            uint8_t b;

            crc ^= (uint16_t)((uint16_t)s_chunk[k] << 8);
            for (b = 0u; b < 8u; b++)
            {
                if ((crc & 0x8000u) != 0u)
                {
                    crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
                }
                else
                {
                    crc = (uint16_t)(crc << 1);
                }
            }
        }
    }

    *out_crc = crc;
    return 0u;
}

/* 按 bank 当前内容重算并写入末尾 CRC16。 */
static uint8_t rc_write_bank_crc(uint32_t base)
{
    uint16_t crc;
    uint8_t  c[2];

    if (rc_bank_crc(base, &crc) != 0u) { return 1u; }

    c[0] = (uint8_t)(crc & 0xFFu);
    c[1] = (uint8_t)((crc >> 8) & 0xFFu);
    return rc_wr(base + RC_TBL_CRC_OFF, c, 2u);
}

/*==========================================================================
 * 6. bank 有效性（magic + ver + CRC）
 *========================================================================*/
static uint8_t rc_bank_valid(uint8_t bank)
{
    uint32_t base = rc_bank_base(bank);
    uint16_t crc_calc;
    uint16_t crc_rd;
    uint8_t  hdr[RC_TBL_CAT_OFF];

    if (rc_rd(base, hdr, sizeof(hdr)) != 0u) { return 0u; }
    if ((hdr[0] != RC_TBL_MAGIC0) || (hdr[1] != RC_TBL_MAGIC1) ||
        (hdr[2] != RC_TBL_VER))
    {
        return 0u;
    }

    if (rc_bank_crc(base, &crc_calc) != 0u) { return 0u; }

    crc_rd = (uint16_t)((uint16_t)rc_rd8(base + RC_TBL_CRC_OFF)
                        | ((uint16_t)rc_rd8(base + RC_TBL_CRC_OFF + 1u) << 8));
    return (crc_rd == crc_calc) ? 1u : 0u;
}

/*==========================================================================
 * 7. 把条数读进模块状态
 *========================================================================*/
static void rc_load_counts(void)
{
    uint32_t base = rc_bank_base(s_bank);

    s_cat_cnt  = rc_rd8(base + 0x004u);
    s_tbl_cnt  = rc_rd8(base + RC_TBL_TBL_CNT_OFF);
    s_bind_cnt = rc_rd8(base + RC_TBL_BIND_CNT_OFF);

    if (s_cat_cnt  > (uint8_t)RC_ACT_CATALOG_MAX) { s_cat_cnt  = (uint8_t)RC_ACT_CATALOG_MAX; }
    if (s_tbl_cnt  > (uint8_t)RC_ACT_TBL_MAX)     { s_tbl_cnt  = (uint8_t)RC_ACT_TBL_MAX; }
    if (s_bind_cnt > (uint8_t)RC_DEV_BIND_MAX)    { s_bind_cnt = (uint8_t)RC_DEV_BIND_MAX; }
}

/*==========================================================================
 * 8. 出厂默认表
 *========================================================================*/
static const struct { uint8_t id; const char *name; } k_def_cat[] =
{
    { 1u, "on"     },
    { 2u, "off"    },
    { 3u, "up"     },
    { 4u, "down"   },
    { 5u, "play"   },
    { 6u, "mute"   },
};
#define RC_DEF_CAT_N  (sizeof(k_def_cat) / sizeof(k_def_cat[0]))

/* 建一块"空的" bank 镜像并写进去（默认表） */
static void rc_write_default_bank(void)
{
    uint32_t base = rc_bank_base(s_bank);
    uint32_t i;
    uint8_t  hdr[RC_TBL_CAT_OFF];
    uint8_t  zero = 0u;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = RC_TBL_MAGIC0;
    hdr[1] = RC_TBL_MAGIC1;
    hdr[2] = RC_TBL_VER;
    hdr[3] = (uint8_t)(s_gen + 1u);         /* 代数递增 */
    hdr[4] = (uint8_t)RC_DEF_CAT_N;

    /* 先把整块擦干净，再写头部 + 默认目录。
     * 擦除后的 0xFF 不能当条数用：rc_load_counts() 会把 0xFF 收缩到
     * 容量上限，于是"默认表"会凭空出现 16 张表和 20 个绑定。
     * 所以表格条数必须显式写成 0。 */
    (void)rc_erase(base, RC_TBL_BANK_SIZE);
    (void)rc_wr(base, hdr, sizeof(hdr));

    for (i = 0u; i < RC_DEF_CAT_N; i++)
    {
        uint8_t rec[RC_TBL_CAT_LEN];

        memset(rec, 0, sizeof(rec));
        rec[0] = k_def_cat[i].id;
        strncpy((char *)&rec[1], k_def_cat[i].name, RC_ACT_NAME_MAX);

        (void)rc_wr(base + RC_TBL_CAT_OFF + (i * RC_TBL_CAT_LEN),
                    rec, RC_TBL_CAT_LEN);
    }

    (void)rc_wr(base + RC_TBL_TBL_CNT_OFF, &zero, 1u);
    (void)rc_wr(base + RC_TBL_BIND_CNT_OFF, &zero, 1u);

    /* 【2026-10-19 修复】这里原来没写 CRC！RcTable_Load() 第一次重建
     * 默认表后，两个 bank 仍然都校验失败，于是每次进遥控页都会再擦写
     * 2KB；网页对遥控表的写入也会因为 CRC 永远不对而掉电丢失。
     * 写完所有内容后必须按同样的区间算 CRC 并落盘。 */
    (void)rc_write_bank_crc(base);

    s_gen = (uint8_t)(s_gen + 1u);          /* RAM 里的代数跟头部一致 */
}

/*==========================================================================
 * 9. 载入
 *========================================================================*/
uint8_t RcTable_Load(void)
{
    uint8_t va = rc_bank_valid(0u);
    uint8_t vb = rc_bank_valid(1u);

    if ((va == 0u) && (vb == 0u))
    {
        /* 两个 bank 都坏（首次上电 / 掉电损坏）：就地重建默认表 */
        s_bank = 0u;
        s_gen  = 0u;
        rc_write_default_bank();
        rc_load_counts();
        return 1u;                          /* 告诉上层"用了默认表" */
    }

    if (va != 0u && vb != 0u)
    {
        uint8_t ga = rc_rd8(rc_bank_base(0u) + 0x003u);
        uint8_t gb = rc_rd8(rc_bank_base(1u) + 0x003u);

        /* 代数大的更新；代数相同取 A（刚写过 B 时 gen 会差 1） */
        s_bank = ((int8_t)(gb - ga) > 0) ? 1u : 0u;
    }
    else
    {
        s_bank = (va != 0u) ? 0u : 1u;
    }

    s_gen = rc_rd8(rc_bank_base(s_bank) + 0x003u);
    rc_load_counts();
    return 0u;
}

/*==========================================================================
 * 10. ★ 整块重写：当前 bank -> 另一个 bank，过程中应用一处修改
 *
 *     mod_off / mod_buf / mod_len 指定"要改哪一段"（在 bank 内的偏移），
 *     传 mod_len = 0 表示纯复制（Save 不做任何修改时用）。
 *
 *     全程只用一块 256 B 暂存，而且**旧 bank 一直不动**，
 *     所以任何时刻掉电都不会同时坏掉两个 bank。
 *========================================================================*/
typedef struct
{
    uint32_t off;           /* bank 内偏移 */
    uint32_t len;
    const uint8_t *buf;
} rc_mod_t;

static uint8_t rc_apply_mod(const rc_mod_t *m, uint32_t chunk_off,
                            uint8_t *chunk, uint32_t chunk_len)
{
    uint32_t i;

    if ((m == 0) || (m->len == 0u)) { return 0u; }

    /* 改动落在本块内的部分逐字节覆盖 */
    for (i = 0u; i < chunk_len; i++)
    {
        uint32_t abs = chunk_off + i;

        if ((abs >= m->off) && (abs < (m->off + m->len)))
        {
            chunk[i] = m->buf[abs - m->off];
        }
    }
    return 0u;
}

static uint8_t rc_bank_flush(const rc_mod_t *m)
{
    uint8_t  dst    = (uint8_t)((s_bank == 0u) ? 1u : 0u);
    uint32_t src_b  = rc_bank_base(s_bank);
    uint32_t dst_b  = rc_bank_base(dst);
    uint32_t off;

    /* 1) 擦目标 bank */
    if (rc_erase(dst_b, RC_TBL_BANK_SIZE) != 0u) { return 1u; }

    /* 2) 分块：读旧 -> 应用改动 -> 写新 */
    for (off = 0u; off < RC_TBL_BANK_SIZE; off += RC_TBL_PAGE)
    {
        if (rc_rd(src_b + off, s_chunk, RC_TBL_PAGE) != 0u) { return 2u; }

        (void)rc_apply_mod(m, off, s_chunk, RC_TBL_PAGE);

        if (off == 0u)
        {
            s_chunk[3] = (uint8_t)(s_gen + 1u);     /* 代数 +1（记在目标里） */
        }

        if (rc_wr(dst_b + off, s_chunk, RC_TBL_PAGE) != 0u) { return 3u; }
    }

    /* 3) 所有内容落盘后再按同一区间重算 CRC。
     * 原实现把每 256B 块的循环变量都从 RC_TBL_CRC_FROM 重新开始，
     * 第二块以后会额外喂进去 254 个假 0，算出的 CRC 永远不对：
     * 网页改遥控表后断电即丢，而且每次 RcTable_Load() 都会判坏并重建。
     * 【2026-10-19 修复】统一走 rc_write_bank_crc()。 */
    if (rc_write_bank_crc(dst_b) != 0u) { return 4u; }

    /* 4) 切换当前 bank + 代数 */
    s_bank = dst;
    s_gen  = (uint8_t)(s_gen + 1u);
    rc_load_counts();
    return 0u;
}

uint8_t RcTable_Save(void)
{
    if (s_bank == 0xFFu) { (void)RcTable_Load(); }
    return rc_bank_flush(0);
}

void RcTable_ResetDefault(void)
{
    if (s_bank == 0xFFu) { (void)RcTable_Load(); }

    /* 往"另一个" bank 写一份只有默认目录的空表：先擦目标，再写头 + 默认目录 */
    {
        uint8_t dst = (uint8_t)((s_bank == 0u) ? 1u : 0u);
        uint32_t db = rc_bank_base(dst);
        uint8_t  hdr[RC_TBL_CAT_OFF];
        uint32_t i;

        (void)rc_erase(db, RC_TBL_BANK_SIZE);

        memset(hdr, 0, sizeof(hdr));
        hdr[0] = RC_TBL_MAGIC0;
        hdr[1] = RC_TBL_MAGIC1;
        hdr[2] = RC_TBL_VER;
        hdr[3] = (uint8_t)(s_gen + 1u);
        hdr[4] = (uint8_t)RC_DEF_CAT_N;
        (void)rc_wr(db, hdr, sizeof(hdr));

        for (i = 0u; i < RC_DEF_CAT_N; i++)
        {
            uint8_t rec[RC_TBL_CAT_LEN];

            memset(rec, 0, sizeof(rec));
            rec[0] = k_def_cat[i].id;
            strncpy((char *)&rec[1], k_def_cat[i].name, RC_ACT_NAME_MAX);
            (void)rc_wr(db + RC_TBL_CAT_OFF + (i * RC_TBL_CAT_LEN), rec, RC_TBL_CAT_LEN);
        }

        /* 擦除后的 0xFF 条数同样会变成 16/20，必须显式写 0 */
        {
            uint8_t zero = 0u;
            (void)rc_wr(db + RC_TBL_TBL_CNT_OFF, &zero, 1u);
            (void)rc_wr(db + RC_TBL_BIND_CNT_OFF, &zero, 1u);
        }

        /* 把 CRC 也算对，否则下次上电会判成坏表 */
        (void)rc_write_bank_crc(db);

        s_bank = dst;
        s_gen  = (uint8_t)(s_gen + 1u);
        rc_load_counts();
    }
}

/*==========================================================================
 * 11. 名字校验
 *========================================================================*/
bool RcTable_NameValid(const char *name, uint8_t max_len)
{
    uint8_t n = 0u;

    if (name == 0) { return false; }

    while (name[n] != '\0')
    {
        char c = name[n];

        if (((c >= '0') && (c <= '9')) ||
            ((c >= 'A') && (c <= 'Z')) ||
            ((c >= 'a') && (c <= 'z')))
        {
            /* OK */
        }
        else
        {
            return false;
        }
        n++;
        if (n > max_len) { return false; }
    }
    return (n >= 1u);
}

uint8_t RcTable_IdxOfKeyId(uint8_t key_id)
{
    if ((key_id < 1u) || (key_id > (uint8_t)RC_OP_COUNT)) { return 0xFFu; }
    return (uint8_t)(key_id - 1u);          /* ★ 唯一的 -1 换算点 */
}

/*==========================================================================
 * 12. ① 动作目录
 *========================================================================*/
uint8_t RcTable_CatalogCount(void) { return s_cat_cnt; }

uint8_t RcTable_CatalogAt(uint8_t idx, rc_act_entry_t *out)
{
    uint8_t rec[RC_TBL_CAT_LEN];
    uint8_t n = 0u;

    if ((out == 0) || (idx >= s_cat_cnt)) { return 1u; }

    rc_rd_blk(rc_bank_base(s_bank) + RC_TBL_CAT_OFF + (idx * RC_TBL_CAT_LEN),
              rec, RC_TBL_CAT_LEN);

    memset(out, 0, sizeof(*out));
    out->id = rec[0];
    while ((n < (uint8_t)RC_ACT_NAME_MAX) && (rec[1u + n] != '\0'))
    {
        out->name[n] = (char)rec[1u + n];
        n++;
    }
    out->name[n] = '\0';
    return 0u;
}

uint8_t RcTable_NameOfId(uint8_t id, char *out, uint8_t cap)
{
    uint8_t i;

    if ((out == 0) || (cap == 0u)) { return 1u; }
    out[0] = '\0';
    if (id == RC_ACT_ID_NONE) { return 1u; }

    for (i = 0u; i < s_cat_cnt; i++)
    {
        rc_act_entry_t e;

        if (RcTable_CatalogAt(i, &e) != 0u) { continue; }
        if (e.id == id)
        {
            uint8_t k = 0u;

            while ((k + 1u < cap) && (e.name[k] != '\0')) { out[k] = e.name[k]; k++; }
            out[k] = '\0';
            return 0u;
        }
    }
    return 2u;                              /* 没这个编号 */
}

/* 写一条目录：整块重写到另一个 bank */
int8_t RcTable_SetName(uint8_t id, const char *name)
{
    uint8_t  rec[RC_TBL_CAT_LEN];
    uint8_t  i;
    uint8_t  dup = 0u;
    rc_mod_t m;

    if ((id == 0u) || (id > (uint8_t)RC_ACT_ID_MAX))       { return -1; }
    if (!RcTable_NameValid(name, (uint8_t)RC_ACT_NAME_MAX)) { return -2; }
    if (s_bank == 0xFFu) { (void)RcTable_Load(); }

    memset(rec, 0, sizeof(rec));
    rec[0] = id;
    strncpy((char *)&rec[1], name, RC_ACT_NAME_MAX);

    /* 已存在同编号就覆盖，否则追加到末尾 */
    for (i = 0u; i < s_cat_cnt; i++)
    {
        rc_act_entry_t e;

        if (RcTable_CatalogAt(i, &e) != 0u) { continue; }
        if (e.id == id) { dup = 1u; break; }
    }

    if (dup != 0u)
    {
        m.off = RC_TBL_CAT_OFF + (i * RC_TBL_CAT_LEN);
    }
    else
    {
        if (s_cat_cnt >= (uint8_t)RC_ACT_CATALOG_MAX) { return -3; }   /* 目录满了 */
        m.off = RC_TBL_CAT_OFF + (s_cat_cnt * RC_TBL_CAT_LEN);
    }
    m.len = RC_TBL_CAT_LEN;
    m.buf = rec;

    if (rc_bank_flush(&m) != 0u) { return -4; }

    /* 追加时条数还没更新，补一次头 */
    if (dup == 0u)
    {
        uint8_t one = (uint8_t)(s_cat_cnt + 1u);

        m.off = 0x004u; m.len = 1u; m.buf = &one;
        if (rc_bank_flush(&m) != 0u) { return -5; }
    }
    return 0;
}

/*==========================================================================
 * 13. ② 动作编号表
 *========================================================================*/
uint8_t RcTable_TblCount(void) { return s_tbl_cnt; }

uint8_t RcTable_TblAt(uint8_t idx, rc_act_tbl_t *out)
{
    uint8_t rec[RC_TBL_TBL_LEN];
    uint8_t n = 0u;

    if ((out == 0) || (idx >= s_tbl_cnt)) { return 1u; }

    rc_rd_blk(rc_bank_base(s_bank) + RC_TBL_TBL_OFF + (idx * RC_TBL_TBL_LEN),
              rec, RC_TBL_TBL_LEN);

    memset(out, 0, sizeof(*out));
    while ((n < (uint8_t)RC_ACT_TBL_NAME_MAX) && (rec[n] != '\0'))
    {
        out->name[n] = (char)rec[n];
        n++;
    }
    out->name[n] = '\0';
    memcpy(out->key_num, &rec[RC_ACT_TBL_NAME_MAX + 1u], RC_OP_COUNT);
    return 0u;
}

int8_t RcTable_FindTbl(const char *name)
{
    uint8_t i;

    if (name == 0) { return -1; }

    for (i = 0u; i < s_tbl_cnt; i++)
    {
        rc_act_tbl_t t;

        if (RcTable_TblAt(i, &t) != 0u) { continue; }
        if (strncmp(t.name, name, RC_ACT_TBL_NAME_MAX + 1u) == 0) { return (int8_t)i; }
    }
    return -1;
}

int8_t RcTable_SetTbl(const rc_act_tbl_t *t)
{
    uint8_t  rec[RC_TBL_TBL_LEN];
    int8_t   idx;
    rc_mod_t m;

    if (t == 0) { return -1; }
    if (!RcTable_NameValid(t->name, (uint8_t)RC_ACT_TBL_NAME_MAX)) { return -2; }
    if (s_bank == 0xFFu) { (void)RcTable_Load(); }

    memset(rec, 0, sizeof(rec));
    strncpy((char *)rec, t->name, RC_ACT_TBL_NAME_MAX);
    memcpy(&rec[RC_ACT_TBL_NAME_MAX + 1u], t->key_num, RC_OP_COUNT);

    idx = RcTable_FindTbl(t->name);
    if (idx < 0)
    {
        if (s_tbl_cnt >= (uint8_t)RC_ACT_TBL_MAX) { return -3; }
        idx = (int8_t)s_tbl_cnt;
    }

    m.off = RC_TBL_TBL_OFF + ((uint32_t)idx * RC_TBL_TBL_LEN);
    m.len = RC_TBL_TBL_LEN;
    m.buf = rec;
    if (rc_bank_flush(&m) != 0u) { return -4; }

    if (RcTable_FindTbl(t->name) < 0)       /* 原来没有 -> 补条数 */
    {
        uint8_t one = (uint8_t)(s_tbl_cnt + 1u);
        m.off = RC_TBL_TBL_CNT_OFF; m.len = 1u; m.buf = &one;
        if (rc_bank_flush(&m) != 0u) { return -5; }
    }
    return 0;
}

/*==========================================================================
 * 14. ③ 设备绑定表
 *========================================================================*/
uint8_t RcTable_BindCount(void) { return s_bind_cnt; }

uint8_t RcTable_BindAt(uint8_t idx, rc_dev_bind_t *out)
{
    uint8_t rec[RC_TBL_BIND_LEN];
    uint8_t n;

    if ((out == 0) || (idx >= s_bind_cnt)) { return 1u; }

    rc_rd_blk(rc_bank_base(s_bank) + RC_TBL_BIND_OFF + (idx * RC_TBL_BIND_LEN),
              rec, RC_TBL_BIND_LEN);

    memset(out, 0, sizeof(*out));
    n = 0u;
    while ((n < (uint8_t)RC_DEV_NAME_MAX) && (rec[n] != '\0')) { out->dev_name[n] = (char)rec[n]; n++; }
    out->dev_name[n] = '\0';

    n = 0u;
    while ((n < (uint8_t)RC_ACT_TBL_NAME_MAX) &&
           (rec[RC_DEV_NAME_MAX + 1u + n] != '\0'))
    {
        out->tbl_name[n] = (char)rec[RC_DEV_NAME_MAX + 1u + n];
        n++;
    }
    out->tbl_name[n] = '\0';
    return 0u;
}

int8_t RcTable_FindBind(const char *dev_name)
{
    uint8_t i;

    if (dev_name == 0) { return -1; }

    for (i = 0u; i < s_bind_cnt; i++)
    {
        rc_dev_bind_t b;

        if (RcTable_BindAt(i, &b) != 0u) { continue; }
        if (strncmp(b.dev_name, dev_name, RC_DEV_NAME_MAX + 1u) == 0) { return (int8_t)i; }
    }
    return -1;
}

int8_t RcTable_SetBind(const char *dev_name, const char *tbl_name)
{
    uint8_t  rec[RC_TBL_BIND_LEN];
    int8_t   idx;
    uint8_t  existed;
    rc_mod_t m;

    if ((dev_name == 0) || (tbl_name == 0)) { return -1; }
    if (!RcTable_NameValid(dev_name, (uint8_t)RC_DEV_NAME_MAX))            { return -2; }
    if (!RcTable_NameValid(tbl_name, (uint8_t)RC_ACT_TBL_NAME_MAX))        { return -3; }
    if (s_bank == 0xFFu) { (void)RcTable_Load(); }

    memset(rec, 0, sizeof(rec));
    strncpy((char *)rec, dev_name, RC_DEV_NAME_MAX);
    strncpy((char *)&rec[RC_DEV_NAME_MAX + 1u], tbl_name, RC_ACT_TBL_NAME_MAX);

    idx     = RcTable_FindBind(dev_name);
    existed = (idx >= 0) ? 1u : 0u;
    if (existed == 0u)
    {
        if (s_bind_cnt >= (uint8_t)RC_DEV_BIND_MAX) { return -4; }
        idx = (int8_t)s_bind_cnt;
    }

    m.off = RC_TBL_BIND_OFF + ((uint32_t)idx * RC_TBL_BIND_LEN);
    m.len = RC_TBL_BIND_LEN;
    m.buf = rec;
    if (rc_bank_flush(&m) != 0u) { return -5; }

    if (existed == 0u)
    {
        uint8_t one = (uint8_t)(s_bind_cnt + 1u);
        m.off = RC_TBL_BIND_CNT_OFF; m.len = 1u; m.buf = &one;
        if (rc_bank_flush(&m) != 0u) { return -6; }
    }
    return 0;
}

/*==========================================================================
 * 15. ★ 核心查表
 *========================================================================*/
uint8_t RcTable_ActionOf(const char *dev_name, uint8_t key_id)
{
    rc_dev_bind_t  b;
    rc_act_tbl_t   t;
    int8_t         idx;
    uint8_t        k;

    k = RcTable_IdxOfKeyId(key_id);
    if (k == 0xFFu) { return RC_ACT_ID_NONE; }      /* key_id 越界 */

    if (s_bank == 0xFFu) { (void)RcTable_Load(); }

    idx = RcTable_FindBind(dev_name);               /* ③ 设备名 -> 表名 */
    if (idx < 0) { return RC_ACT_ID_NONE; }
    if (RcTable_BindAt((uint8_t)idx, &b) != 0u) { return RC_ACT_ID_NONE; }

    idx = RcTable_FindTbl(b.tbl_name);              /* ② 表名 -> 12 个编号 */
    if (idx < 0) { return RC_ACT_ID_NONE; }
    if (RcTable_TblAt((uint8_t)idx, &t) != 0u) { return RC_ACT_ID_NONE; }

    return t.key_num[k];
}

uint8_t RcTable_LoadActionsFor(const char *dev_name, uint8_t out12[RC_OP_COUNT])
{
    rc_dev_bind_t b;
    rc_act_tbl_t  t;
    int8_t        idx;
    uint8_t       k;

    if (out12 == 0) { return 1u; }
    memset(out12, 0, RC_OP_COUNT);

    if (s_bank == 0xFFu) { (void)RcTable_Load(); }

    idx = RcTable_FindBind(dev_name);
    if (idx < 0) { return 2u; }
    if (RcTable_BindAt((uint8_t)idx, &b) != 0u) { return 3u; }

    idx = RcTable_FindTbl(b.tbl_name);
    if (idx < 0) { return 4u; }
    if (RcTable_TblAt((uint8_t)idx, &t) != 0u) { return 5u; }

    for (k = 0u; k < (uint8_t)RC_OP_COUNT; k++)
    {
        out12[k] = t.key_num[k];
    }
    return 0u;
}

/*==========================================================================
 * 16. 调试：把当前 bank 的摘要读出来（不占 RAM）
 *========================================================================*/
uint8_t RcTable_CurrentBank(void) { return s_bank; }
uint8_t RcTable_Gen(void)         { return s_gen;  }

/******************* (C) CH592X 遥控模式配置表 *****************/
