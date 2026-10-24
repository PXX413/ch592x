/**************************************************************************
 * game_thunder.h —— CH592X 雷霆战机（横屏卷轴射击）
 *
 * 屏幕：横屏 240x135
 *       y=0 .. 17    顶部 HUD：生命条 / 炸弹数 / 等级 / 分数
 *       y=18 .. 134  战场（对象只在这里活动，绘图自动裁剪）
 *
 * 操作（以代码为准：game_thunder.c 的 THD_HandleKey()）：
 *       K1 .................... 按住向上飞
 *       K4 .................... 按住向下飞
 *       K6 .................... 按住连发（120ms 一发）
 *       K3 .................... 放炸弹（清掉全场敌弹 + 全场伤害）
 *       编码器按键 ............ 暂停 / 继续
 *       编码器双击 ............ 退出到菜单
 *       BOOT/MENU 页：K5 或 编码器按键 = 开始
 *       GAMEOVER 页：K5 或 编码器按键 = 重开
 *
 * 状态机：BOOT -> MENU -> PLAYING <-> PAUSED -> GAMEOVER
 *       （WIN 保留但已不再进入：打完 Boss 接着放下一架，见 THD_BOSS_RESPAWN_MS）
 *
 * 对象池（全部静态数组，不用 malloc）：
 *       玩家 x1 / 玩家子弹 x12 / 敌机 x10 / 敌弹 x16 / Boss x1 / 爆炸 x8
 *
 * 渲染：不做全屏帧缓冲。每个对象记住"上一次画在屏幕上的矩形"，
 *       每帧先用背景色擦掉旧矩形，再画到新位置（脏矩形局部重绘）。
 **************************************************************************/

#ifndef __GAME_THUNDER_H__
#define __GAME_THUNDER_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 屏幕 / 布局
 *========================================================================*/
#define THD_W               240         /* 逻辑宽（横屏） */
#define THD_H               135         /* 逻辑高 */
#define THD_HUD_H           18          /* 顶部 HUD 条高度 */
#define THD_TOP             THD_HUD_H   /* 战场上边界 */
#define THD_BOTTOM          THD_H       /* 战场下边界（不含） */

/*==========================================================================
 * 2. 对象池规模
 *========================================================================*/
#define THD_MAX_PBULLET     12          /* 玩家子弹 */
#define THD_MAX_ENEMY       10          /* 敌机 */
#define THD_MAX_EBULLET     16          /* 敌弹 */
#define THD_MAX_BOOM        8           /* 爆炸特效 */

/*==========================================================================
 * 3. 敌机类型 / 炸弹 / 生命
 *========================================================================*/
#define THD_EK_SMALL        0           /* 小飞机：直飞       100 分 */
#define THD_EK_DIAG         1           /* 斜飞：斜向移动     150 分 */
#define THD_EK_SHOOT        2           /* 射击机：直飞+发弹  300 分 */
#define THD_EK_FAST         3           /* 高速机：快速直飞   200 分 */
#define THD_EK_BOSS         4           /* Boss：多炮口+移动 5000 分 */

#define THD_LIVES_MAX       3           /* 初始血量（命） */
#define THD_BOMB_MAX        3           /* 炸弹数 */

/*==========================================================================
 * 4. 状态机
 *========================================================================*/
typedef enum
{
    THD_ST_BOOT  = 0,       /* 开机 Logo（短暂停留后自动进 MENU） */
    THD_ST_MENU,            /* 标题页："按开始" */
    THD_ST_PLAY,            /* 游戏进行中 */
    THD_ST_PAUSE,           /* 暂停 */
    THD_ST_OVER,            /* 游戏结束 */
    THD_ST_WIN              /* 击落 Boss，通关 */
} ThdState_t;

/*==========================================================================
 * 5. 对外接口（和其它游戏保持一致）
 *========================================================================*/
void THD_Enter(void);
void THD_Update(uint16_t dt_ms);
void THD_Draw(void);
bool THD_HandleKey(const GameKeyEvent_t *ev);

bool     THD_IsOver(void);              /* 结束或通关都算"这一局完了" */
bool     THD_IsPaused(void);
bool     THD_IsWin(void);
uint32_t THD_GetScore(void);
uint8_t  THD_GetLives(void);
uint8_t  THD_GetBombs(void);
uint8_t  THD_GetLevel(void);            /* 当前波次 1..THD_WAVE_MAX */
uint8_t  THD_GetState(void);            /* ThdState_t */
bool     THD_BossAlive(void);
uint16_t THD_GetBossHp(void);

#if THD_TEST_HOOKS
/* ---- 下面这些只在 PC 自测时编译进来，真机固件里没有 ---- */
void    THD_ClearEnemies(void);
void    THD_ClearEnemyBullets(void);
void    THD_ClearPlayerBullets(void);
void    THD_SpawnEnemy(uint8_t kind, int16_t x, int16_t y);
void    THD_SpawnEnemyBullet(int16_t x, int16_t y);
void    THD_SpawnBoss(void);
void    THD_SetPlayerY(int16_t y);      /* 像素坐标 */
void    THD_SetTimeMs(uint32_t ms);     /* 直接设定已玩时间（跳波次用） */
void    THD_DamageBoss(uint16_t n);
void    THD_HitPlayer(void);            /* 直接扣一条命（测无敌时间用） */
uint8_t THD_GetEnemyCount(void);
uint8_t THD_GetEnemyBulletCount(void);
uint8_t THD_GetPlayerBulletCount(void);
uint8_t THD_GetBoomCount(void);
bool    THD_GetEnemy(uint8_t i, int16_t *x, int16_t *y, uint8_t *kind, uint8_t *hp);
bool    THD_GetBossBox(int16_t *x, int16_t *y, uint8_t *w, uint8_t *h);
bool    THD_GetEnemyBullet(uint8_t i, int16_t *x, int16_t *y);
uint16_t THD_GetPoolBytes(void);        /* 对象池占用字节数（卡内存预算用） */
int16_t THD_GetPlayerY(void);
uint8_t THD_GetPlayerW(void);
uint8_t THD_GetPlayerH(void);
bool    THD_IsInvincible(void);
#endif

#ifdef __cplusplus
}
#endif

/* 实体池改到共享池后，类型必须对 game_state.h 可见 */
typedef struct
{
    int16_t  x;         /* 位置：Q16 */
    int16_t  y;
    int16_t  vx;        /* 速度：Q16 / tick */
    int16_t  vy;
    int16_t  px;        /* 上一次画到屏幕上的**像素**位置 */
    int16_t  py;
    uint8_t  w;         /* 尺寸（像素），对象一生都不变 */
    uint8_t  h;
    uint8_t  hp;
    uint8_t  kind;
    uint8_t  alive;
    uint8_t  drawn;     /* px/py 处有没有画过东西（要擦） */
    uint16_t timer;     /* 毫秒计时：敌机开火间隔 / 爆炸剩余时间 */
} ThdObj_t;             /* sizeof == 20 字节 */

#endif /* __GAME_THUNDER_H__ */
