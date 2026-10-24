/**************************************************************************
 * kmap_store.c —— 按键映射的运行时存储与 Flash 持久化
 *
 * 见 kmap_store.h 的分区与记录格式说明。
 *
 * 设计要点：
 *   1) 映射值的「真相」仍然在 key_matrix.c 的 s_map[][] 里，
 *      本模块不另存一份，避免两处不一致。
 *      读：调 KEY_GetMapValue()
 *      写：调 KEY_SetMapValue()
 *   2) 组名 const 在 key_matrix.c 里改不了，所以这里自带一份 RAM 副本，
 *      通过 KEY_GetMapGroupName 的弱符号钩子对外暴露（见文件末尾说明）。
 *   3) Flash 读写用 WCH 的 EEPROM 宏，和 ui_store.c 一样的用法。
 **************************************************************************/

#include "kmap_store.h"
#include "key_matrix.h"

#include <string.h>

/*==========================================================================
 * EEPROM 访问宏（与 ui_store.c 保持一致的写法）
 *========================================================================*/
#define KMAP_FLASH_ERASE()  FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, KMAP_FLASH_OFF, 0, KMAP_FLASH_ERASE_LEN)

/*==========================================================================
 * 内存副本
 *========================================================================*/
static char    s_name[KEY_MAP_GROUPS][KMAP_STORE_NAME_SLOT];
static bool    s_has_record = false;
static uint8_t s_last_err   = KMAP_STORE_OK;

/*==========================================================================
 * CRC-16/CCITT-FALSE
 *   初值 0xFFFF，poly 0x1021，不反转。
 *   和 ui_store.c / rf_cfg 用的是同一套，便于统一排查。
 *========================================================================*/
static uint16_t kmap_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t  b;

    if (p == 0)
    {
        return 0u;
    }

    for (i = 0u; i < n; i++)
    {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
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

    return crc;
}

/*==========================================================================
 * 默认组名（和 key_matrix.c 的 s_map_name 一致）
 *   用作「Flash 里没有记录」时的初始值
 *========================================================================*/
static const char *const s_default_name[KEY_MAP_GROUPS] =
{
    "123", "ARROW", "F1-F9", "NUM", "ABC"
};

static void kmap_names_default(void)
{
    uint8_t g;

    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        memset(s_name[g], 0, KMAP_STORE_NAME_SLOT);
        if (s_default_name[g] != 0)
        {
            strncpy(s_name[g], s_default_name[g], (size_t)KMAP_STORE_NAME_MAX);
            s_name[g][KMAP_STORE_NAME_MAX] = '\0';
        }
    }
}

/*==========================================================================
 * 打包 / 解包
 *========================================================================*/
static void kmap_pack(uint8_t *out)
{
    uint8_t  g, k;
    uint16_t crc;
    uint16_t pos;

    memset(out, 0, KMAP_FLASH_ERASE_LEN);

    /* --- 头 --- */
    out[KMAP_OFF_MAGIC]    = (uint8_t)(KMAP_STORE_MAGIC & 0xFFu);
    out[KMAP_OFF_MAGIC+1u] = (uint8_t)((KMAP_STORE_MAGIC >> 8) & 0xFFu);
    out[KMAP_OFF_VER]      = (uint8_t)(KMAP_STORE_VER & 0xFFu);
    out[KMAP_OFF_VER+1u]   = (uint8_t)((KMAP_STORE_VER >> 8) & 0xFFu);
    out[KMAP_OFF_CURGROUP] = KEY_GetMapGroup();
    out[KMAP_OFF_GROUPS]   = (uint8_t)KEY_MAP_GROUPS;
    out[KMAP_OFF_KEYS]     = (uint8_t)KEY_MAP_KEYS;

    /* --- 映射表（小端 16 位） --- */
    pos = KMAP_OFF_MAP;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 1u; k <= (uint8_t)KEY_MAP_KEYS; k++)
        {
            uint16_t v = KEY_GetMapValue(g, k);
            out[pos]     = (uint8_t)(v & 0xFFu);
            out[pos + 1u] = (uint8_t)((v >> 8) & 0xFFu);
            pos = (uint16_t)(pos + 2u);
        }
    }

    /* --- 编码器槽（v3 新增：每组 3 个，正转/反转/单击） --- */
    pos = KMAP_OFF_ENC;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 1u; k <= (uint8_t)KEY_ENC_SLOTS; k++)
        {
            uint16_t v = KEY_GetEncMapValue(g, k);
            out[pos]      = (uint8_t)(v & 0xFFu);
            out[pos + 1u] = (uint8_t)((v >> 8) & 0xFFu);
            pos = (uint16_t)(pos + 2u);
        }
    }

    /* --- 组名 --- */
    pos = KMAP_OFF_NAME;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        memcpy(&out[pos], s_name[g], KMAP_STORE_NAME_SLOT);
        pos = (uint16_t)(pos + KMAP_STORE_NAME_SLOT);
    }

    /* --- CRC（覆盖 [0, KMAP_OFF_CRC)） --- */
    crc = kmap_crc16(out, KMAP_OFF_CRC);
    out[KMAP_OFF_CRC]     = (uint8_t)(crc & 0xFFu);
    out[KMAP_OFF_CRC+1u]  = (uint8_t)((crc >> 8) & 0xFFu);
}

/* 把记录里的数据灌进 key_matrix + 本模块的组名副本 */
static void kmap_apply(const uint8_t *in)
{
    uint8_t  g, k;
    uint16_t pos;

    /* --- 映射表 --- */
    pos = KMAP_OFF_MAP;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 1u; k <= (uint8_t)KEY_MAP_KEYS; k++)
        {
            uint16_t v = (uint16_t)((uint16_t)in[pos] | ((uint16_t)in[pos + 1u] << 8));
            KEY_SetMapValue(g, k, v);
            pos = (uint16_t)(pos + 2u);
        }
    }

    /* --- 编码器槽（v3 新增） --- */
    pos = KMAP_OFF_ENC;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        for (k = 1u; k <= (uint8_t)KEY_ENC_SLOTS; k++)
        {
            uint16_t v = (uint16_t)((uint16_t)in[pos] | ((uint16_t)in[pos + 1u] << 8));
            KEY_SetEncMapValue(g, k, v);
            pos = (uint16_t)(pos + 2u);
        }
    }

    /* --- 组名 --- */
    pos = KMAP_OFF_NAME;
    for (g = 0u; g < (uint8_t)KEY_MAP_GROUPS; g++)
    {
        memcpy(s_name[g], &in[pos], KMAP_STORE_NAME_SLOT);
        s_name[g][KMAP_STORE_NAME_MAX] = '\0';   /* 强制以 0 收尾，防脏数据 */
        pos = (uint16_t)(pos + KMAP_STORE_NAME_SLOT);
    }

    /* --- 当前组 --- */
    KEY_SetMapGroup(in[KMAP_OFF_CURGROUP]);
}

/* 校验一段记录是否合法 */
static bool kmap_verify(const uint8_t *in)
{
    uint16_t magic;
    uint16_t ver;
    uint16_t crc_calc;
    uint16_t crc_rec;

    magic = (uint16_t)((uint16_t)in[KMAP_OFF_MAGIC] | ((uint16_t)in[KMAP_OFF_MAGIC+1u] << 8));
    if (magic != KMAP_STORE_MAGIC)
    {
        return false;
    }

    ver = (uint16_t)((uint16_t)in[KMAP_OFF_VER] | ((uint16_t)in[KMAP_OFF_VER+1u] << 8));
    if (ver != KMAP_STORE_VER)
    {
        return false;
    }

    /* 组数/键数必须和编译期一致，否则记录格式对不上 */
    if (in[KMAP_OFF_GROUPS] != (uint8_t)KEY_MAP_GROUPS)
    {
        return false;
    }
    if (in[KMAP_OFF_KEYS] != (uint8_t)KEY_MAP_KEYS)
    {
        return false;
    }

    crc_calc = kmap_crc16(in, KMAP_OFF_CRC);
    crc_rec  = (uint16_t)((uint16_t)in[KMAP_OFF_CRC] | ((uint16_t)in[KMAP_OFF_CRC+1u] << 8));
    if (crc_calc != crc_rec)
    {
        return false;
    }

    return true;
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
/*==========================================================================
 * 【RAM 优化】整块 Flash 页缓冲，全模块共用一块
 *
 * 原来 Load / Save 各自在函数里开 static：
 *     KmapStore_Load()  buf[256]
 *     KmapStore_Save()  buf[256] + chk[256]
 * 三块一共 768 字节常驻 .bss。但：
 *   * 两块 buf 永远不会同时活着（Load 只在初始化跑，Save 只在用户改了键值跑）；
 *   * 回读校验根本不需要再开一整块 —— 分块回读、边读边比就行。
 * 现在合成：
 *     s_page[256]   打包 / 读页共用
 *     s_vchk[64]    回读校验的分块缓冲
 * 合计 320 字节，省下 448 字节 .bss（.bss 每省 1 字节，堆就多 1 字节）。
 *========================================================================*/
static uint8_t s_page[KMAP_FLASH_ERASE_LEN];   /* 打包/读页共用 */
static uint8_t s_vchk[64];                     /* 回读校验分块 */

bool KmapStore_Load(void)
{
    memset(s_page, 0, sizeof(s_page));
    EEPROM_READ(KMAP_FLASH_OFF, s_page, (uint32_t)KMAP_FLASH_ERASE_LEN);

    if (!kmap_verify(s_page))
    {
        s_has_record = false;
        s_last_err   = KMAP_STORE_OK;      /* 没有记录不是错误 */
        return false;
    }

    kmap_apply(s_page);
    s_has_record = true;
    s_last_err   = KMAP_STORE_OK;
    return true;
}

void KmapStore_Init(void)
{
    /* 先把组名置成默认，保证即使 Flash 空着也有名字可用 */
    kmap_names_default();
    s_has_record = false;
    s_last_err   = KMAP_STORE_OK;

    /* 映射表本身的默认值由 key_matrix.c 的 KEY_ResetMap 负责（它是惰性的，
     * 第一次调用映射相关 API 时会自动初始化）。这里只要尝试覆盖成 Flash 版。 */
    (void)KmapStore_Load();
}

uint8_t KmapStore_Save(void)
{
    /* 1) 打包当前状态 */
    kmap_pack(s_page);

    /* 2) 擦除整块，再写 */
    KMAP_FLASH_ERASE();

    if (EEPROM_WRITE(KMAP_FLASH_OFF, (void *)(uintptr_t)s_page, (uint32_t)KMAP_FLASH_ERASE_LEN) != 0u)
    {
        s_last_err = KMAP_STORE_E_FLASH;
        return KMAP_STORE_E_FLASH;
    }

    /* 3) 回读校验（Flash 写入偶尔会失败，不校验等于没存）
     *    分块回读、边读边比：不再需要一整块 256 字节的 chk。 */
    {
        uint16_t off = 0u;

        while (off < (uint16_t)KMAP_FLASH_ERASE_LEN)
        {
            uint16_t n = (uint16_t)(KMAP_FLASH_ERASE_LEN - off);

            if (n > (uint16_t)sizeof(s_vchk)) { n = (uint16_t)sizeof(s_vchk); }

            memset(s_vchk, 0, sizeof(s_vchk));
            EEPROM_READ((uint32_t)(KMAP_FLASH_OFF + off), s_vchk, (uint32_t)n);

            if (memcmp(&s_page[off], s_vchk, (size_t)n) != 0)
            {
                s_last_err = KMAP_STORE_E_VERIFY;
                return KMAP_STORE_E_VERIFY;
            }

            off = (uint16_t)(off + n);
        }
    }

    s_has_record = true;
    s_last_err   = KMAP_STORE_OK;
    return KMAP_STORE_OK;
}

uint8_t KmapStore_ResetDefault(void)
{
    uint8_t rc;

    /* 1) 映射表恢复默认（key_matrix 自己有一份 const 默认值） */
    KEY_ResetMap();

    /* 2) 组名恢复默认 */
    kmap_names_default();

    /* 3) 当前组回 0 */
    KEY_SetMapGroup(0u);

    /* 4) 落盘（这样下次开机还是默认，而不是又读回旧记录） */
    rc = KmapStore_Save();

    return rc;
}

uint16_t KmapStore_RawSize(void)
{
    return (uint16_t)KMAP_FLASH_ERASE_LEN;
}

const char *KmapStore_GroupName(uint8_t group)
{
    if (group >= (uint8_t)KEY_MAP_GROUPS)
    {
        return "";
    }
    return s_name[group];
}

uint8_t KmapStore_SetGroupName(uint8_t group, const char *name)
{
    size_t n;

    if (group >= (uint8_t)KEY_MAP_GROUPS || name == 0)
    {
        s_last_err = KMAP_STORE_E_ARG;
        return KMAP_STORE_E_ARG;
    }

    n = strlen(name);
    if (n > (size_t)KMAP_STORE_NAME_MAX)
    {
        n = (size_t)KMAP_STORE_NAME_MAX;
    }

    memset(s_name[group], 0, KMAP_STORE_NAME_SLOT);
    memcpy(s_name[group], name, n);
    s_name[group][n] = '\0';

    s_last_err = KMAP_STORE_OK;
    return KMAP_STORE_OK;
}

bool KmapStore_HasFlashRecord(void)
{
    return s_has_record;
}

uint8_t KmapStore_LastError(void)
{
    return s_last_err;
}

/**************************************************************************
 * 【组名钩子】
 *
 * key_matrix.c 里的 s_map_name[] 是 const，运行时改不了。
 * 所以在 key_matrix.c 的 KEY_GetMapGroupName() 里加了一个弱符号：
 *
 *     __attribute__((weak)) const char *KEY_MapGroupNameHook(uint8_t group)
 *     {
 *         return 0;              // 0 = 用内置的 s_map_name
 *     }
 *
 *     const char *KEY_GetMapGroupName(uint8_t group)
 *     {
 *         const char *p = KEY_MapGroupNameHook(group);
 *         ...
 *         return (p != 0) ? p : s_map_name[group];
 *     }
 *
 * 本文件实现同名强符号覆盖它，返回 RAM 里的组名副本。
 * 这样不改 key_matrix.c 的对外行为，又能让组名可改。
 * ------------------------------------------------------------------
 * 注意：这个函数必须和弱符号【同名同签名】，否则不会覆盖。
 *************************************************************************/
const char *KEY_MapGroupNameHook(uint8_t group);

const char *KEY_MapGroupNameHook(uint8_t group)
{
    if (group >= (uint8_t)KEY_MAP_GROUPS)
    {
        return 0;                   /* 让 key_matrix 自己处理越界 */
    }
    return s_name[group];
}
