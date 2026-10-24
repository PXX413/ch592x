/**************************************************************************
 * game_menu.h —— CH592X 小游戏主菜单（选游戏 / 难度 / 显示最高分 / 成绩页）
 *
 * 依赖 : game_port（输入、绘图、中文）、game_score（最高分存数据 Flash）
 *
 * 主循环用法（main.c 里就是这样写的）：
 *
 *      GM_Enter();
 *      while (1)
 *      {
 *          GameKeyEvent_t ev;
 *          Game_Task();
 *
 *          while (Game_PollEvent(&ev))
 *          {
 *              if (GM_HandleKey(&ev))            // 返回 true = 用户要开始游戏
 *              {
 *                  start_game(GM_GetSelected());  // 去跑对应游戏
 *                  // 游戏结束时：
 *                  GM_ShowResult(id, score);      // 显示成绩页（内部会存最高分）
 *              }
 *          }
 *
 *          GM_Update(GAME_FRAME_MS);             // 20ms 一帧
 *          GM_Draw();                            // 菜单 + 成绩页都由它画
 *      }
 *
 * 操作：
 *      编码器旋转 / K4 K6 ....... 上下移动光标（K4=上一个、K6=下一个）
 *      K5 / 编码器按键 ......... 开始选中的游戏
 *      K1 / K3 ................. 难度 - / +
 *      K7 ...................... 音效开关（设置会存进数据 Flash）
 *      成绩页：任意键返回菜单
 **************************************************************************/

#ifndef __GAME_MENU_H__
#define __GAME_MENU_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_port.h"
#include "game_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GM_MODE_MENU   = 0,     /* 正在选游戏 */
    GM_MODE_RESULT = 1      /* 正在看成绩 */
} GmMode_t;

/* 回到菜单（清屏并画一遍）；进入程序时和每次从游戏返回时调用 */
void GM_Enter(void);

/* 处理一个按键事件；返回 true = 用户要启动 GM_GetSelected() 这个游戏 */
bool GM_HandleKey(const GameKeyEvent_t *ev);

/* 每帧调用（建议 20ms）：只做少量状态更新 */
void GM_Update(uint16_t dt_ms);

/* 画菜单或成绩页（只重画变化的部分） */
void GM_Draw(void);

/* 当前选中 / 当前难度 / 当前模式 */
uint8_t  GM_GetSelected(void);          /* GAME_ID_xxx */
uint8_t  GM_GetDifficulty(void);        /* GameDiff_t */
GmMode_t GM_GetMode(void);

/*********************************************************************
 * @fn      GM_ShowResult
 * @brief   显示一局游戏的成绩页
 * @param   game_id  哪个游戏（GAME_ID_xxx）
 * @param   score    本局得分
 * @note    内部会调 GS_SetBest()：破了纪录会写数据 Flash 并显示"新纪录"
 *********************************************************************/
void GM_ShowResult(uint8_t game_id, uint32_t score);

/* 成绩页信息（测试/演示用） */
uint32_t GM_GetResultScore(void);
uint32_t GM_GetResultBest(void);
bool     GM_GetResultNewRecord(void);

/* 取某个游戏的中文名字（菜单、成绩页都用它） */
GameTextId_t GM_GetGameNameId(uint8_t game_id);

#ifdef __cplusplus
}
#endif

#endif /* __GAME_MENU_H__ */
