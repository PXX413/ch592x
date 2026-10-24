/**************************************************************************
 * bt_screen.h —— CH592X "附近蓝牙设备"界面（键盘屏上的扫描/选择/连接）
 *
 * 屏幕：竖屏 135x240
 *      y=0..17    标题：蓝牙设备
 *      y=20..175  设备列表（一屏 6 行，编码器上下选，高亮当前行）
 *      y=180..215 状态：扫描中 / 已连接 xxx / 未找到设备
 *      y=222..239 按键提示：连接 / 返回
 *
 * 用法（主循环里）：
 *      BT_SCREEN_Enter();                       // 清屏 + 开始扫描 + 画一遍
 *      while (1)
 *      {
 *          Game_Task();                         // 键盘 + 编码器
 *          while (Game_PollEvent(&ev))
 *          {
 *              switch (BT_SCREEN_HandleKey(&ev))
 *              {
 *                  case BT_SCREEN_CONNECT: BT_ConnectSelected(); break;
 *                  case BT_SCREEN_RESCAN:  BT_StartScan();       break;
 *                  case BT_SCREEN_BACK:    goto back_to_menu;
 *                  default: break;
 *              }
 *          }
 *          BT_SCREEN_Update(GAME_FRAME_MS);
 *          BT_SCREEN_Draw();
 *      }
 **************************************************************************/

#ifndef __BT_SCREEN_H__
#define __BT_SCREEN_H__

#include <stdint.h>
#include <stdbool.h>
#include "game_port.h"
#include "bt_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一屏显示几行设备 */
#define BTS_ROWS        6
#define BTS_ROW_Y0      20
#define BTS_ROW_H       26
#define BTS_STATUS_Y    180
#define BTS_STATUS_H    34

typedef enum
{
    BT_SCREEN_NONE = 0,         /* 只是移动了光标，调用者不用做事 */
    BT_SCREEN_SET_HOST,         /* 用户要把选中的设备设为宿主（白名单） */
    BT_SCREEN_RESCAN,           /* 用户要重新扫描 */
    BT_SCREEN_BACK              /* 用户要返回上一层 */
} BtScreenAction_t;

void             BT_SCREEN_Enter(void);         /* 清屏 + 开始扫描 + 全画 */
void             BT_SCREEN_Update(uint16_t dt_ms);
void             BT_SCREEN_Draw(void);
BtScreenAction_t BT_SCREEN_HandleKey(const GameKeyEvent_t *ev);

/* 强制整屏重画（比如从别处切回来） */
void BT_SCREEN_Invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* __BT_SCREEN_H__ */
