/**************************************************************************
 * game_state.h —— 三个游戏共用一块状态池（省 RAM）
 *
 * 为什么要有这个：
 *   三个游戏各自静态开了一份实体池，而**同一时刻只会跑一个游戏**：
 *         雷霆战机  ThdObj_t[12+10+16+8] = 20*46 = 920 B
 *         跑酷      RunObs_t[6]                   ~ 90 B
 *         2048      uint16_t[4][4] x3              128 B
 *   加起来 1108 B，其实只需要最大的那份。
 *
 * 做法：一块 union，按需要现取现用（必须 return 回 0，别只 switch）。
 *      真机上没有半途切换游戏，这个约束很容易守住。
 *
 * 用法（在游戏 .c 里）：
 *      #include "game_state.h"
 *      static ThdObj_t *s_pbullet;            // 原来是 s_pbullet[THD_MAX_PBULLET]
 *      ...
 *      进入游戏时：
 *      Game_PoolYield();                       // 先把池让出来
 *      s_pbullet = Game_PoolPb();              // 再按需要取
 *      退出游戏时：Game_PoolYield();
 *
 * 注意：上面这几行要写在**看到 ThdObj_t / RunObs_t 定义之后**，
 * 所以三个游戏各自把 GameState_Views() 放在自己文件里调用。
 **************************************************************************/

#ifndef __GAME_STATE_H__
#define __GAME_STATE_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_2048.h"
#include "game_runner.h"
#include "game_thunder.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 容量（改了这里要跟着改各游戏的 MAX）
 *========================================================================*/
#define GSTATE_PBULLET_N    12
#define GSTATE_ENEMY_N      10
#define GSTATE_EBULLET_N    16
#define GSTATE_BOOM_N        8
#define GSTATE_OBS_N         6
#define GSTATE_BOARD_N      16      /* 2048 的 4x4，一份 16 个 uint16 */

/*==========================================================================
 * 2. 取用 / 归还
 *==========================================================================*/

/*********************************************************************
 * @fn      Game_PoolYield
 * @brief   把整块池让出来（所有指针失效）。进/出游戏时各调一次。
 * @note    这是唯一安全的"复用"动作：只清指针，不保证数据还在。
 *********************************************************************/
/*********************************************************************
 * @fn      Game_PoolAcquire
 * @brief   真正进游戏时调：向堆申请状态池
 * @return  true = 拿到了；false = 堆不够（应当退出游戏）
 *********************************************************************/
bool Game_PoolAcquire(void);

/* 现在池在不在（没进游戏就不在） */
bool Game_PoolReady(void);

void Game_PoolYield(void);

/* 按需要取用；同一个池只能给一个游戏用，取错顺序会拿到重叠的块 */
ThdObj_t *Game_PoolPbullet(void);
ThdObj_t *Game_PoolEnemy(void);
ThdObj_t *Game_PoolEbullet(void);
ThdObj_t *Game_PoolBoom(void);
RunObs_t *Game_PoolObs(void);
uint16_t *Game_PoolBoard2048(void);     /* 16 个格子，按 [y*4+x] 访问 */
uint16_t *Game_PoolDrawn2048(void);
uint16_t *Game_PoolUndo2048(void);

/* 池的总字节数（测试与文档用） */
uint16_t Game_PoolSize(void);

#ifdef GAME_STATE_TEST
/*********************************************************************
 * @fn      Game_PoolCheck
 * @brief   PC 自测用：检查各指针都在池内、且相邻块不重叠
 * @return  true = 所有块都在池内且互不重叠
 *********************************************************************/
bool Game_PoolCheck(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __GAME_STATE_H__ */
