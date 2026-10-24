/**************************************************************************
 * game_score.h —— CH592X 小游戏"最高分 / 设置"存储（数据 Flash，掉电保留）
 *
 * 依赖 : CH59x_common.h（里面已包含 ISP592.h，提供 EEPROM_READ / EEPROM_WRITE）
 *
 * 存储位置：数据 Flash（基址 0x00070000，最大 32KB），只用第 1 页（256 字节）
 *   * 链接脚本里代码只占前 448K，所以数据区绝对不会被程序覆盖
 *   * 记录里带 magic + 版本 + CRC16，任何一项不对就当作"没有纪录"用默认值，
 *     绝不会因为写了一半、芯片掉电而读出乱七八糟的数字
 *   * 只有"真的破了纪录"才写 Flash（不是每帧写），Flash 寿命完全够用
 *
 * 用法：
 *     GS_Init();                                  // 开机读一次（有校验）
 *     if (GS_SetBest(GAME_ID_2048, score)) {      // 破纪录会自动保存
 *         ... 显示"新纪录" ...
 *     }
 **************************************************************************/

#ifndef __GAME_SCORE_H__
#define __GAME_SCORE_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 游戏编号 / 难度
 *========================================================================*/
#define GAME_ID_NONE        0
#define GAME_ID_2048        1       /* 2048 */
#define GAME_ID_RUNNER      2       /* 跑酷 */
#define GAME_ID_THUNDER     3       /* 雷霆战机 */
#define GAME_ID_MAX         3
/*
 * 加游戏时的规矩（很重要，关系到用户存下来的最高分）：
 *   * 只是**往后新增**一个编号（老编号的含义一个都没变）时：
 *     纪录数组预留了 GS_BEST_SLOTS=8 个槽位，记录长度不变、
 *     老纪录每一个都还落在原来的下标上 —— **不用**升 GS_VERSION。
 *     例：0x0002 这一版加 GAME_ID_THUNDER=3 就属于这种情况。
 *   * 一旦**已有编号的含义变了**（删游戏、重排顺序、改含义）：
 *     必须把 GS_VERSION +1，否则会把别的游戏的纪录读成本游戏的。
 */

typedef enum
{
    GAME_DIFF_EASY   = 0,
    GAME_DIFF_NORMAL = 1,
    GAME_DIFF_HARD   = 2
} GameDiff_t;

/*==========================================================================
 * 2. 接口
 *========================================================================*/

/*********************************************************************
 * @fn      GS_Init
 * @brief   从数据 Flash 读一次纪录和设置（内部只读一小段，很快）
 * @note    magic / 版本 / CRC 任何一项不对都会自动用默认值；可以重复调用
 *********************************************************************/
void GS_Init(void);

/* 取某个游戏的最高分（没纪录返回 0） */
uint32_t GS_GetBest(uint8_t game_id);

/*********************************************************************
 * @fn      GS_SetBest
 * @brief   尝试刷新最高分
 * @param   game_id  游戏编号（GAME_ID_xxx）
 * @param   score    本次成绩
 * @return  true = 破了纪录并且已经写进 Flash；false = 没破（不写 Flash）
 *********************************************************************/
bool GS_SetBest(uint8_t game_id, uint32_t score);

/* 全部纪录清零（同时写 Flash） */
void GS_Reset(void);

/* ---- 设置项：难度 / 上次玩的游戏 / 音效开关 ---- */
uint8_t GS_GetDifficulty(void);                     /* GameDiff_t */
void    GS_SetDifficulty(uint8_t diff);             /* 超出范围会被纠正为普通 */
uint8_t GS_GetLastGame(void);                       /* GAME_ID_xxx */
void    GS_SetLastGame(uint8_t game_id);
uint8_t GS_GetSound(void);                          /* 1 = 开，0 = 关（暂时只存着） */
void    GS_SetSound(uint8_t on);

/*********************************************************************
 * @fn      GS_Save
 * @brief   把内存里的纪录/设置写进数据 Flash（写前关中断，写完恢复）
 * @note    GS_SetBest() 内部会自动调用它；改设置项后请自己调一次
 *********************************************************************/
void GS_Save(void);

/* 调试/测试用：RAM 里正在用的纪录数据（只读） */
const void *GS_GetRawRecord(void);
uint16_t    GS_GetRawSize(void);

/* 内存里有没有还没写进 Flash 的改动（改设置项后可以用它判断要不要 GS_Save） */
bool GS_IsDirty(void);

#ifdef __cplusplus
}
#endif

#endif /* __GAME_SCORE_H__ */
