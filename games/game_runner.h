/**************************************************************************
 * game_runner.h —— CH592X 横版跑酷
 *
 * 屏幕：横屏 240x135
 *       天空 0..107，地面线 y=108，地面下面到 134
 *
 * 操作：
 *       K5 / K2 .... 跳跃：站在地上点一下（任意一个都行）起跳，
 *                    还在空中再点一下就二段跳（一次）
 *       K9 ......... 老的二段跳别名（只在空中生效）
 *       K8 ......... 按住滑铲（可以从悬空障碍下面钻过去）；
 *                    在空中按下 = 立刻砸回地面再滑
 *       K6 ......... 冲刺技能：向前突进一个障碍物宽度（12 像素），实现方式是
 *                    障碍物/地面在这 150ms 里加速左移，人物本身不做水平移动；
 *                    两层可以连按两次，按完 1 秒冷却，1 秒内没接第二下也回满
 *                    （RUN_DASH_* 见 game_runner.c）
 *       K7 ......... 按住加速（1.5 倍速）
 *       编码器按键 . 暂停 / 继续
 *       编码器双击 . 退出到菜单
 *
 * 三种障碍：
 *       矮的（跳过去） / 高的（要二段跳） / 悬空的（滑铲钻过去）
 * 速度只随时间走（80 起步、每 5 秒 +1、上限 220，见 run_calc_speed()），
 * 分数 = 跑过的距离（每 10 像素 1 分）。
 **************************************************************************/

#ifndef __GAME_RUNNER_H__
#define __GAME_RUNNER_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RUN_W           240
#define RUN_H           135
#define RUN_GROUND_Y    108         /* 地面线的 y 坐标 */
#define RUN_PLAYER_X    26
#define RUN_PLAYER_W    9
#define RUN_STAND_H     16
#define RUN_SLIDE_H     8
#define RUN_MAX_OBS     6

void RUN_Enter(void);
void RUN_Update(uint16_t dt_ms);
void RUN_Draw(void);
bool RUN_HandleKey(const GameKeyEvent_t *ev);

bool     RUN_IsOver(void);
bool     RUN_IsPaused(void);
uint32_t RUN_GetScore(void);            /* 距离 / 10 */
uint32_t RUN_GetDistance(void);         /* 跑过的像素数 */
uint16_t RUN_GetSpeed(void);            /* 像素/秒 */
uint8_t  RUN_GetPlayerH(void);          /* 现在的高度（站立 16 / 滑铲 8） */
bool     RUN_IsGrounded(void);
int16_t  RUN_GetPlayerTop(void);        /* 玩家顶部的 y */
uint8_t  RUN_GetObstacleCount(void);

/* 调试 / 测试用 */
bool     RUN_GetObstacle(uint8_t i, int16_t *x, int16_t *y, int16_t *w, int16_t *h);
void     RUN_ClearObstacles(void);
void     RUN_SpawnObstacle(int16_t x, int16_t y, int16_t w, int16_t h);

#ifdef __cplusplus
}
#endif

/* 实体池改到共享池后，类型必须对 game_state.h 可见 */
typedef struct
{
    int16_t x;          /* 左边缘（像素） */
    int16_t y;          /* 上边缘 */
    int16_t w;
    int16_t h;
    bool    active;
    bool    drawn;      /* 是否已经在屏上画过（用来擦掉旧位置） */
    int16_t px;         /* 上次画在哪 */
    int16_t py;
} RunObs_t;

#endif /* __GAME_RUNNER_H__ */
