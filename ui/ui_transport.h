/**************************************************************************
 * ui_transport.h —— 键盘通信方式仲裁（USB → 2.4G → BLE）
 *
 * 依据：文档「（二）1. 键盘自动判断通信方式」
 *   a. 判断 USB 是否连接主机，有则直接使用 USB 通信；没有就进 b（本阶段 0.5s）
 *   b. 广播 2.4G，1 秒内对码并收到心跳包就直接用 2.4G；否则进 c（本阶段 1.5s）
 *   c. 广播 BLE，等待连接（时长不限）
 *
 * 设计：
 *   * 纯状态机，不碰任何硬件 —— USB / 2.4G / BLE 三个模块把"插上了没""心跳有没有"
 *     喂进来（UiTr_SetUsb / SetRf24 / SetBle），本模块只负责"该用谁"。
 *   * 需要"开始/停止广播"的时候通过 UiTr_SetAction() 回调出去；
 *     要发 HID 报文的时候通过 UiTr_SetSend() 回调出去。
 *     这样 PC 上能完整测时序，真机上把回调接到三个通道模块即可。
 *   * 通道掉了（USB 拔了 / 2.4G 心跳没了 / BLE 断了）→ 从头重新走一遍 a→b→c。
 **************************************************************************/

#ifndef __UI_TRANSPORT_H__
#define __UI_TRANSPORT_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 通道
 *========================================================================*/
typedef enum
{
    UI_TR_NONE = 0,         /* 还没定下来（正在判断） */
    UI_TR_USB,              /* 走数据线 */
    UI_TR_RF24,             /* 走 2.4G 私有协议 */
    UI_TR_BLE               /* 走蓝牙 */
} ui_transport_t;

/*==========================================================================
 * 2. 时序（文档里的硬性数字）
 *========================================================================*/
#define UI_TR_T_USB_MS      500u    /* a 阶段：判断 USB，0.5s          */
#define UI_TR_T_RF24_MS     1500u   /* b 阶段：广播 2.4G，1.5s         */
#define UI_TR_RF24_PAIR_MS  1000u   /* b 阶段里"1 秒内对码 + 心跳"     */

/*==========================================================================
 * 3. 阶段（给页面/调试看）
 *========================================================================*/
typedef enum
{
    UI_TR_SCAN_USB = 0,
    UI_TR_SCAN_RF24,
    UI_TR_SCAN_BLE,
    UI_TR_LOCKED            /* 已经选中某个通道 */
} ui_tr_phase_t;

typedef struct
{
    ui_tr_phase_t  phase;
    ui_transport_t cur;         /* UI_TR_NONE = 还没定 */

    uint32_t phase_ms;          /* 本阶段已经过了多久 */
    uint32_t rf24_ms;           /* 进入 2.4G 阶段后过了多久（判断"1 秒内"用） */

    uint8_t  usb_plugged;       /* 底层喂进来的状态 */
    uint8_t  usb_ready;
    uint8_t  rf24_alive;
    uint8_t  ble_ready;

    uint8_t  started;           /* 第一次 Tick 时才会去起广播 */
    uint32_t usb_tries;
    uint32_t rf24_tries;
    uint32_t ble_tries;
    uint32_t switches;          /* 切换过几次通道（含掉线重来） */

    void (*action)(ui_transport_t tr, bool starting);
    bool (*send)(ui_transport_t tr, const uint8_t *rep, uint8_t len);
} ui_tr_t;

/*==========================================================================
 * 4. 接口
 *========================================================================*/

/*********************************************************************
 * @fn      UiTr_Init
 * @brief   复位状态机，从 a 阶段（判断 USB）重新开始
 *********************************************************************/
void UiTr_Init(ui_tr_t *t);

/*********************************************************************
 * @fn      UiTr_Tick
 * @brief   推进状态机
 * @param   dt_ms  距上次调用的毫秒数（建议 10ms 一次，或者 1ms 一次都行）
 *********************************************************************/
void UiTr_Tick(ui_tr_t *t, uint16_t dt_ms);

/* 当前选中的通道（UI_TR_NONE = 还在判断） */
ui_transport_t UiTr_Current(const ui_tr_t *t);

/* 当前阶段 */
ui_tr_phase_t UiTr_Phase(const ui_tr_t *t);

/* 选中的通道是不是"已连接、能发键值" */
bool UiTr_Connected(const ui_tr_t *t);

/* 通道名，给键盘页右上角那 50x50 显示用（纯 ASCII，屏上再用字库画） */
const char *UiTr_Name(ui_transport_t tr);
const char *UiTr_StateText(const ui_tr_t *t);   /* "USB" / "2.4G" / "BLE" / "--" */

/*==========================================================================
 * 5. 底层喂状态（真机里由三个通道模块调；PC 测试直接调）
 *========================================================================*/
void UiTr_SetUsb(ui_tr_t *t, bool plugged, bool ready);
void UiTr_SetRf24(ui_tr_t *t, bool alive);
void UiTr_SetBle(ui_tr_t *t, bool ready);

/*==========================================================================
 * 6. 回调（真机接到三个通道上；PC 测试拿桩件记录）
 *========================================================================*/

/* 进入/离开某个阶段的广播动作：starting=true 开始广播，false 停止 */
void UiTr_SetAction(ui_tr_t *t, void (*fn)(ui_transport_t tr, bool starting));

/* 按当前通道发一帧 HID 报文；返回 false = 没发出去 */
void UiTr_SetSend(ui_tr_t *t, bool (*fn)(ui_transport_t tr, const uint8_t *rep, uint8_t len));

/*********************************************************************
 * @fn      UiTr_SendHid
 * @brief   按当前通道发一帧 HID 报文（键盘页按键时调它）
 * @return  true = 交给通道了；false = 当前没有可用通道
 * @note    文档「（二）4」：USB/蓝牙直接发 HID；2.4G 是发给接收器，由接收器转发 HID。
 *          转发这件事在 rf24 那边做，这里只管"交给 2.4G"
 *********************************************************************/
bool UiTr_SendHid(ui_tr_t *t, const uint8_t *rep, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* __UI_TRANSPORT_H__ */
