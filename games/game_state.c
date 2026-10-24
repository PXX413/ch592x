/**************************************************************************
 * game_state.c —— 三个游戏共用的状态池实现
 *
 * 一块 union，谁用谁取。所有指针指向同一块内存的不同偏移，
 * 所以"同时只跑一个游戏"是硬前提。
 **************************************************************************/

#include "game_state.h"
#include "game_mem.h"
#include <string.h>

/*==========================================================================
 * 池本体
 *
 * 用 union 而不是裸 uint8_t[]，是为了让编译器帮忙对齐：
 *   ThdObj_t / RunObs_t 里都是 int16_t + uint8_t，2 字节对齐够用；
 *   uint16_t 棋盘也要 2 字节对齐。union 会按最严格成员对齐。
 *========================================================================*/
typedef union
{
    ThdObj_t  thunder[GSTATE_PBULLET_N + GSTATE_ENEMY_N + GSTATE_EBULLET_N + GSTATE_BOOM_N];
    RunObs_t  runner[GSTATE_OBS_N];
    uint16_t  cells[GSTATE_BOARD_N * 4];        /* 2048 的三份棋盘叠加对齐 */
} game_pool_t;

#define GSTATE_POOL_BYTES   ((uint16_t)sizeof(game_pool_t))

/* 不再常驻：真正进游戏时才向堆申请，退出就还。
 * 在“选游戏”列表里还不需要它，所以那时候也不占。 */
static game_pool_t *s_pool;

/*--------------------------------------------------------------------------
 * 池内的偏移布局（字节数）
 *   雷霆把一整块按 4 段切开用；跑酷和 2048 从头开始用。
 *------------------------------------------------------------------------*/
#define OFF_PBULLET    0u
#define OFF_ENEMY      (OFF_PBULLET + GSTATE_PBULLET_N * (uint16_t)sizeof(ThdObj_t))
#define OFF_EBULLET    (OFF_ENEMY   + GSTATE_ENEMY_N   * (uint16_t)sizeof(ThdObj_t))
#define OFF_BOOM       (OFF_EBULLET + GSTATE_EBULLET_N * (uint16_t)sizeof(ThdObj_t))

/* 2048 三份棋盘：board / drawn / undo 各 16 个 uint16 */
#define OFF_BOARD      0u
#define OFF_DRAWN      (OFF_BOARD + 16u * (uint16_t)sizeof(uint16_t))
#define OFF_UNDO       (OFF_DRAWN + 16u * (uint16_t)sizeof(uint16_t))

/*********************************************************************
 * @fn      Game_PoolAcquire
 * @brief   进游戏时调：申请状态池
 * @return  true = 拿到了；false = 堆不够（调用方应当退出游戏而不是乱画）
 *********************************************************************/
bool Game_PoolAcquire(void)
{
    if (s_pool != 0)
    {
        return true;                    /* 已经在游戏里了 */
    }

    s_pool = (game_pool_t *)GameMem_Acquire(GSTATE_POOL_BYTES);
    return (s_pool != 0);
}

bool Game_PoolReady(void)
{
    return (s_pool != 0);
}

static uint8_t *pool_bytes(void) { return (uint8_t *)s_pool; }

/*==========================================================================
 * 取用
 *========================================================================*/
ThdObj_t *Game_PoolPbullet(void)
{
    if (s_pool == 0) { return 0; }
    return (ThdObj_t *)(void *)(pool_bytes() + OFF_PBULLET);
}
ThdObj_t *Game_PoolEnemy(void)
{
    if (s_pool == 0) { return 0; }
    return (ThdObj_t *)(void *)(pool_bytes() + OFF_ENEMY);
}
ThdObj_t *Game_PoolEbullet(void)
{
    if (s_pool == 0) { return 0; }
    return (ThdObj_t *)(void *)(pool_bytes() + OFF_EBULLET);
}
ThdObj_t *Game_PoolBoom(void)
{
    if (s_pool == 0) { return 0; }
    return (ThdObj_t *)(void *)(pool_bytes() + OFF_BOOM);
}

RunObs_t *Game_PoolObs(void)
{
    if (s_pool == 0) { return 0; }
    return &s_pool->runner[0];
}

uint16_t *Game_PoolBoard2048(void)
{
    if (s_pool == 0) { return 0; }
    return &s_pool->cells[OFF_BOARD / 2u];
}
uint16_t *Game_PoolDrawn2048(void)
{
    if (s_pool == 0) { return 0; }
    return &s_pool->cells[OFF_DRAWN / 2u];
}
uint16_t *Game_PoolUndo2048(void)
{
    if (s_pool == 0) { return 0; }
    return &s_pool->cells[OFF_UNDO / 2u];
}

/*********************************************************************
 * @fn      Game_PoolYield
 * @brief   把池让出来。
 * @note    这里**不做 memset**：每局开局那几行都会自己清一遍
 *          （下雪式全清是 920 字节 memset，开局一次可以接受，
 *            但退出时再来一次就是白费）。
 *********************************************************************/
void Game_PoolYield(void)
{
    /* 退出游戏：把整块状态池还给堆，下次进游戏再申请 */
    if (s_pool != 0)
    {
        GameMem_Yield((void *)s_pool);
        s_pool = 0;
    }
}

uint16_t Game_PoolSize(void)
{
    return GSTATE_POOL_BYTES;
}

#ifdef GAME_STATE_TEST
bool Game_PoolCheck(void)
{
    uint8_t  *base = pool_bytes();
    uint16_t  size = (uint16_t)sizeof(game_pool_t);

    ThdObj_t *blocks[4];
    uint16_t  off[4];
    uint8_t   i, j;

    blocks[0] = Game_PoolPbullet(); off[0] = OFF_PBULLET;
    blocks[1] = Game_PoolEnemy();   off[1] = OFF_ENEMY;
    blocks[2] = Game_PoolEbullet(); off[2] = OFF_EBULLET;
    blocks[3] = Game_PoolBoom();    off[3] = OFF_BOOM;

    for (i = 0u; i < 4u; i++)
    {
        uint8_t *p = (uint8_t *)(void *)blocks[i];

        /* 每个块都要落在池内 */
        if (p < base || (uint16_t)(p - base) + GSTATE_PBULLET_N * sizeof(ThdObj_t) > size)
        {
            return false;
        }
        for (j = 0u; j < i; j++)
        {
            if (off[i] == off[j]) { return false; }
            /* 雷霆的四段必须依次排开，不能重叠 */
            if ((uint16_t)sizeof(ThdObj_t) > (off[i] > off[j] ? off[i] - off[j] : off[j] - off[i]))
            {
                return false;
            }
        }
    }

    /* 2048 的三份也要在池内且互不重叠 */
    {
        uint16_t *a = Game_PoolBoard2048();
        uint16_t *b = Game_PoolDrawn2048();
        uint16_t *c = Game_PoolUndo2048();
        uint8_t  *pa = (uint8_t *)(void *)a;
        uint8_t  *pb = (uint8_t *)(void *)b;
        uint8_t  *pc = (uint8_t *)(void *)c;

        if (pa < base || pb < base || pc < base) { return false; }
        if ((uint16_t)(pa - base) + 32u > size) { return false; }
        if ((uint16_t)(pb - base) + 32u > size) { return false; }
        if ((uint16_t)(pc - base) + 32u > size) { return false; }
    }

    return true;
}
#endif
