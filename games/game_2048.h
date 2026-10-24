/**************************************************************************
 * game_2048.h —— CH592X 2048
 *
 * 屏幕是横屏 240x135。版面（2026-10-13 按需求书重排）：
 *      x=0..134     左侧 135x135：4x4 棋盘
 *                   格子 33px + 缝 1px = 4*33 + 3*1 = 135，正好铺满
 *      x=135        1px 宽、135 高的分隔线
 *      x=136..239   右侧 104x135：当前分数 / 最高分（白底亮色）
 *
 * 按键：
 *      K2 / K4 / K6 / K8 ... 上 / 左 / 右 / 下
 *      编码器单击 ......... 暂停（锁定进度、禁用操作）；再击继续
 *      游戏结束 ........... 按任意键重开
 *      （原来的"撤销"已按需求删除）
 *
 * 刷新：
 *      进游戏 / 重开 与 游戏结束 才整屏；游戏中左侧只刷变化格、
 *      右侧只刷当前分数。详见 game_2048.c 顶部说明。
 *
 * 规则是标准 2048：同值相撞合并且一次移动内每个块只合并一次；
 * 每次有效移动在空格里生成一个 2（90%）或 4（10%）。
 **************************************************************************/

#ifndef __GAME_2048_H__
#define __GAME_2048_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define G2048_SIZE      4
#define G2048_CELL      33      /* 格子边长：4*33 + 3*1 = 135，刚好铺满左侧 */
#define G2048_GAP       1       /* 格与格的缝（露出的底板色就是分隔线） */
#define G2048_BOARD_W   135     /* 左侧棋盘区边长 */
#define G2048_WIN       2048u

/* 方向 */
#define G2048_UP        0
#define G2048_RIGHT     1
#define G2048_DOWN      2
#define G2048_LEFT      3

void G2048_Enter(void);
void G2048_Update(uint16_t dt_ms);
void G2048_Draw(void);
bool G2048_HandleKey(const GameKeyEvent_t *ev);

bool     G2048_IsOver(void);
bool     G2048_IsPaused(void);
bool     G2048_IsWin(void);
uint32_t G2048_GetScore(void);
uint32_t G2048_GetBest(void);           /* 含本局追平/超过的部分 */
uint16_t G2048_GetCell(uint8_t x, uint8_t y);
uint8_t  G2048_GetTiles(void);          /* 盘面里还有几个非空格 */

/*********************************************************************
 * @fn      G2048_Move
 * @brief   向某个方向移动一次（自动生成新块、计分、判定胜/负）
 * @param   dir  G2048_UP / G2048_DOWN / G2048_LEFT / G2048_RIGHT
 * @return  true = 盘面发生了变化（这时才会生成新块）
 * @note    暂停或已结束时直接返回 false（进度锁定，不接受操作）
 *********************************************************************/
bool G2048_Move(uint8_t dir);

#if G2048_TEST_HOOKS
void G2048_SetCell(uint8_t x, uint8_t y, uint16_t v);
void G2048_ClearBoard(void);
void G2048_SetScore(uint32_t s);
void G2048_SetSpawnValue(uint16_t v);   /* 0 = 随机默认；非 0 = 下次生成固定值 */
#endif

#ifdef __cplusplus
}
#endif

#endif /* __GAME_2048_H__ */
