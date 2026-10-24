/**************************************************************************
 * game_score.c —— CH592X 小游戏"最高分 / 设置"存储实现
 *
 * 记录格式（共 32 字节）：
 *     magic 'G','S' | 版本 | 各游戏最高分（编号 1..GAME_ID_MAX） | 难度 | 上次玩的 | 音效 | 保留 | CRC16
 *
 * 版本历史（改游戏编号必须升版本，旧纪录会被自动丢弃并回到默认值）：
 *     0x0001  最早一版：1=俄罗斯方块 2=贪吃蛇 3=2048 4=跑酷
 *     0x0002  删掉贪吃蛇/俄罗斯方块，重排为 1=2048 2=跑酷
 *             （之后新增 3=雷霆战机只算"往后加一个编号"，2048/跑酷的编号没变，
 *               8 个纪录槽位也够用，所以版本**保持 0x0002 不变**，老纪录继续有效）
 **************************************************************************/

#include "game_score.h"
#include "CH59x_common.h"
#include <stddef.h>

/*==========================================================================
 * 内部定义
 *========================================================================*/
#define GS_MAGIC            0x4753u     /* 'G' | 'S'<<8 */
#define GS_VERSION          0x0002u

#define GS_FLASH_OFFSET     0x0000u     /* 数据 Flash 内的偏移（基址 0x00070000） */
#define GS_FLASH_ERASE_LEN  256u        /* 擦除最小 256 字节 */

/* 纪录槽位数（固定 8 个）：游戏增删都不会改变记录长度，
 * 以后加游戏只要 GAME_ID_MAX 往后排、并升一次 GS_VERSION 即可 */
#define GS_BEST_SLOTS       8u

/* 没有 EEPROM_ERASE 宏，直接用底层命令 */
#define GS_FLASH_ERASE(addr, len)   FLASH_EEPROM_CMD(CMD_EEPROM_ERASE, (addr), 0, (len))

typedef struct
{
    uint16_t magic;
    uint16_t version;
    uint32_t best[GS_BEST_SLOTS];       /* [0] 不用，方便用游戏编号直接索引 */
    uint8_t  difficulty;
    uint8_t  last_game;
    uint8_t  sound;
    uint8_t  reserved;
    uint16_t crc;                       /* 覆盖前面所有字节（到 crc 之前） */
} gs_record_t;

/*==========================================================================
 * 内存副本
 *========================================================================*/
static gs_record_t s_rec;
static bool        s_loaded = false;
static bool        s_dirty  = false;

/*==========================================================================
 * CRC16-CCITT（0x1021，初值 0xFFFF）
 *========================================================================*/
static uint16_t gs_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t  b;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);

        for (b = 0; b < 8u; b++)
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

static uint16_t gs_crc_len(void)
{
    return (uint16_t)offsetof(gs_record_t, crc);
}

/*==========================================================================
 * 默认值 / 合法性检查
 *========================================================================*/
static void gs_set_defaults(void)
{
    uint8_t i;

    s_rec.magic      = GS_MAGIC;
    s_rec.version    = GS_VERSION;
    s_rec.difficulty = (uint8_t)GAME_DIFF_NORMAL;
    s_rec.last_game  = (uint8_t)GAME_ID_NONE;
    s_rec.sound      = 1u;
    s_rec.reserved   = 0u;

    for (i = 0; i < (uint8_t)GS_BEST_SLOTS; i++)
    {
        s_rec.best[i] = 0u;
    }
}

static bool gs_record_valid(const gs_record_t *r)
{
    uint16_t crc;

    if (r->magic != GS_MAGIC)
    {
        return false;
    }
    if (r->version != GS_VERSION)
    {
        return false;
    }

    crc = gs_crc16((const uint8_t *)r, gs_crc_len());
    return (crc == r->crc) ? true : false;
}

static void gs_fix_settings(void)
{
    if (s_rec.difficulty > (uint8_t)GAME_DIFF_HARD)
    {
        s_rec.difficulty = (uint8_t)GAME_DIFF_NORMAL;
    }
    if (s_rec.last_game > (uint8_t)GAME_ID_MAX)
    {
        s_rec.last_game = (uint8_t)GAME_ID_NONE;
    }
    s_rec.sound = (s_rec.sound != 0u) ? 1u : 0u;
}

/*==========================================================================
 * 读 / 写
 *========================================================================*/
void GS_Init(void)
{
    gs_record_t tmp;
    uint8_t     i;

    /* 先把缓冲区清成 0，防止读失败时残留旧数据 */
    for (i = 0; i < (uint8_t)sizeof(gs_record_t); i++)
    {
        ((uint8_t *)&tmp)[i] = 0u;
    }

    EEPROM_READ(GS_FLASH_OFFSET, &tmp, (uint32_t)sizeof(gs_record_t));

    if (gs_record_valid(&tmp))
    {
        s_rec = tmp;
        gs_fix_settings();
    }
    else
    {
        gs_set_defaults();              /* 第一次上电 / 数据坏了：用默认值，不写 Flash */
    }

    s_loaded = true;
    s_dirty  = false;
}

void GS_Save(void)
{
    uint32_t irq;

    if (!s_loaded)
    {
        GS_Init();
    }

    gs_fix_settings();
    s_rec.magic   = GS_MAGIC;
    s_rec.version = GS_VERSION;
    s_rec.crc     = gs_crc16((const uint8_t *)&s_rec, gs_crc_len());

    /* 擦写数据 Flash 时关中断，写完恢复（ROM 例程执行期间 CPU 会等待） */
    irq = __risc_v_disable_irq();

    GS_FLASH_ERASE(GS_FLASH_OFFSET, GS_FLASH_ERASE_LEN);
    EEPROM_WRITE(GS_FLASH_OFFSET, &s_rec, (uint32_t)sizeof(gs_record_t));

    __risc_v_enable_irq(irq);

    s_dirty = false;
}

/*==========================================================================
 * 纪录
 *========================================================================*/
uint32_t GS_GetBest(uint8_t game_id)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    if (game_id == (uint8_t)GAME_ID_NONE || game_id > (uint8_t)GAME_ID_MAX)
    {
        return 0u;
    }
    return s_rec.best[game_id];
}

bool GS_SetBest(uint8_t game_id, uint32_t score)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    if (game_id == (uint8_t)GAME_ID_NONE || game_id > (uint8_t)GAME_ID_MAX)
    {
        return false;
    }

    if (score <= s_rec.best[game_id])
    {
        return false;                   /* 没破纪录：一个字节都不写 Flash */
    }

    s_rec.best[game_id] = score;
    s_dirty = true;
    GS_Save();
    return true;
}

void GS_Reset(void)
{
    gs_set_defaults();
    s_rec.last_game = (uint8_t)GAME_ID_NONE;
    s_loaded = true;
    GS_Save();
}

/*==========================================================================
 * 设置项
 *========================================================================*/
uint8_t GS_GetDifficulty(void)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    return s_rec.difficulty;
}

void GS_SetDifficulty(uint8_t diff)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    if (diff > (uint8_t)GAME_DIFF_HARD)
    {
        diff = (uint8_t)GAME_DIFF_NORMAL;
    }
    if (s_rec.difficulty != diff)
    {
        s_rec.difficulty = diff;
        s_dirty = true;
    }
}

uint8_t GS_GetLastGame(void)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    return s_rec.last_game;
}

void GS_SetLastGame(uint8_t game_id)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    if (game_id > (uint8_t)GAME_ID_MAX)
    {
        game_id = (uint8_t)GAME_ID_NONE;
    }
    if (s_rec.last_game != game_id)
    {
        s_rec.last_game = game_id;
        s_dirty = true;
    }
}

uint8_t GS_GetSound(void)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    return s_rec.sound;
}

void GS_SetSound(uint8_t on)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    on = (on != 0u) ? 1u : 0u;
    if (s_rec.sound != on)
    {
        s_rec.sound = on;
        s_dirty = true;
    }
}

/*==========================================================================
 * 调试/测试用
 *========================================================================*/
const void *GS_GetRawRecord(void)
{
    if (!s_loaded)
    {
        GS_Init();
    }
    return (const void *)&s_rec;
}

uint16_t GS_GetRawSize(void)
{
    return (uint16_t)sizeof(gs_record_t);
}

bool GS_IsDirty(void)
{
    return s_dirty;
}

/******************* (C) CH592X 小游戏纪录存储 *****************/
