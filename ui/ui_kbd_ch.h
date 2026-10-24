/**************************************************************************
 * ui_kbd_ch.h —— 把键盘页的报文真正交给 USB / 2.4G / BLE 三个模块
 *
 * ui_kbd.c 只负责"该用哪条通道"和"报文长什么样"，出口是回调。
 * 本模块把那三个回调接到真实驱动上：
 *
 *   USB  -> UsbKbd_KeyEvent() 逐键喂；再 UsbKbd_SendReportNow() 发一帧
 *   2.4G -> Kb_BuildHid6() 打成协议帧，交给 hal_rf 发出去
 *   BLE  -> BT_HID_KeyEvent() 逐键喂；再 BT_HID_Flush() 发一帧
 *
 * 真机接线（Main.c）：
 *      UiKbdCh_Init();
 *          ... 主循环里 ...
 *      UiKbdCh_Task();     // 喂底层状态 + 推进仲裁 + 发键值
 *
 * PC 测试里把 uikbdch_port_t 换成一堆记录桩，就能断言"哪个通道收到了
 * 什么报文"，不需要真的插 USB 线。
 **************************************************************************/

#ifndef __UI_KBD_CH_H__
#define __UI_KBD_CH_H__

#include <stdint.h>
#include <stdbool.h>
#include "ui_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 底层端口：真机用真实驱动，PC 测试用记录桩
 *========================================================================*/
typedef struct
{
    /* ---- 查询通道状态 ---- */
    bool (*usb_plugged)(void);
    bool (*usb_ready)(void);
    bool (*rf24_alive)(void);
    bool (*ble_ready)(void);

    /* ---- 逐键喂给 HID 报表层（key: 1..9）---- */
    void (*usb_key)(uint8_t key, bool down);
    void (*ble_key)(uint8_t key, bool down);

    /* ---- 发一帧 8 字节 6KRO 报文 ---- */
    bool (*usb_send)(const uint8_t *rep8);
    /* 2.4G 不主动发：把报表存进设备，等主机 POLL 时回上去 */
    void (*rf24_set_report)(const uint8_t *rep8);
    bool (*ble_send)(const uint8_t *rep8);

    /* ---- 广播动作（开始/停止 2.4G / BLE 广播）---- */
    void (*broadcast)(ui_transport_t tr, bool starting);

    /* 切换通道时清掉另一条通道的按键状态：
     * 否则 USB 那边会一直记得上次按着的键，切回去就是幱影按键。 */
    void (*reset_keys)(void);
} uikbdch_port_t;

/* 不传端口就用内置的"真实驱动"端口（真机默认走这条） */
void UiKbdCh_Init(void);

/*********************************************************************
 * @fn      UiKbdCh_InitSoft
 * @brief   只做软件接线：注册三条通道的出口回调 + 初始化按键状态机。
 *          **完全不碰硬件**（不读 Flash 配对信息、不调 HALRF_Init）。
 *          开机专用 —— 2.4G 射频必须等真正进键盘模式时再初始化。
 * @note    不能在开机时调 UiKbdCh_Init()：它内部会调 HALRF_Init()
 *          -> RF_Config()，在 BLE 协议栈未初始化的情况下配置射频，
 *          库内部会死等状态位导致开机卡死。
 *********************************************************************/
void UiKbdCh_InitSoft(void);

/* 测试/特殊用途：换成自己的端口 */
void UiKbdCh_InitWith(const uikbdch_port_t *port);

/* 主循环里每 1ms 调一次：喂状态、推进仲裁、按需发报文 */
/* 主循环里调：等主机 POLL，收到就回 HID 并喂心跳
 * 返回 true = 这一轮收到了合法 POLL（会阻塞约 300us）*/
bool UiKbdCh_PollRf24(void);

/*********************************************************************
 * @fn      UiKbdCh_SetRadioMode
 * @brief   键盘 2.4G 与遥控 2.4G 共用一颗射频，进遥控前必须互斥
 * @param   keyboard_on  true = 键盘用；false = 遥控用（键盘让出去）
 *********************************************************************/
void UiKbdCh_SetRadioMode(bool keyboard_on);

void UiKbdCh_OnRf24Heartbeat(void);  /* 2.4G 成功交换一次就叫一声 */
void UiKbdCh_SetRf24Alive(bool alive);   /* 2.4G 心跳由上层喂进来 */
void UiKbdCh_SetBleReady(bool ready);    /* BLE 连接状态由上层喂进来（或用内置驱动） */
void UiKbdCh_Task(void);

/* ---- 查询（给页面/调试用）---- */
ui_transport_t UiKbdCh_Channel(void);
bool           UiKbdCh_Connected(void);

uint32_t       UiKbdCh_FramesSent(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_KBD_CH_H__ */
