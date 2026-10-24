/**************************************************************************
 * usb_kbd.h —— CH592X USB HID 键盘（设备侧主逻辑）
 *
 * 分层：
 *      usb_kbd_report.*   按键 -> 8 字节报文（纯逻辑，可 PC 单测）
 *      usb_desc.*         描述符表（纯数据 + 自查）
 *      usb_kbd.*          ← 本文件：控制传输状态机 + HID 类请求 + 上报节流
 *      usb_kbd_port.h     USB 外设抽象（真机寄存器 / PC 虚拟端点）
 *
 * 产品里 USB 是**最高优先级的传输通道**（见键盘功能实现.txt）：
 * 插上线就用 USB，没插线才去走 2.4G / BLE。上层只要看
 * UsbKbd_IsConnected() 与 UsbKbd_IsPlugged() 两个开关即可。
 **************************************************************************/

#ifndef __USB_KBD_H__
#define __USB_KBD_H__

#include <stdint.h>
#include <stdbool.h>
#include "usb_kbd_report.h"
#include "usb_kbd_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 0. 复合设备开关（HID 键盘 + CDC 虚拟串口）
 *
 *   1（默认）= USB 复合设备，网页能通过虚拟串口直连改映射，不用重烧
 *   0        = 退回原来的「纯 HID 键盘」
 *
 *   改成 0 之后，usb_desc_cdc.* / usb_cdc.* / kmap_* 都不会被真正用到，
 *   行为跟这次改动之前一模一样，随时能退。
 *========================================================================*/
#ifndef USBKBD_ENABLE_CDC
#define USBKBD_ENABLE_CDC   1
#endif

/*==========================================================================
 * 1. 设备状态
 *========================================================================*/
typedef enum
{
    USBK_ST_DISCONNECTED = 0,   /* 没插线，或者刚被总线复位            */
    USBK_ST_DEFAULT,            /* 枚举中，地址还是 0                  */
    USBK_ST_ADDRESSED,          /* SET_ADDRESS 完成，还没配好          */
    USBK_ST_CONFIGURED          /* SET_CONFIGURATION 完成，可以发报文  */
} UsbKbdState_t;

/* HID 协议模式（SET_PROTOCOL） */
#define USBK_PROTO_BOOT         0u
#define USBK_PROTO_REPORT       1u

/*==========================================================================
 * 2. 接口
 *========================================================================*/

/*********************************************************************
 * @fn      UsbKbd_DeviceInit
 * @brief   初始化 USB 外设 + 报文层，进入"等主机枚举"状态
 * @note    内部会调 UsbPort_Init()，所以真机上它必须在
 *          SetSysClock() 之后调
 *********************************************************************/
void UsbKbd_DeviceInit(void);

/*********************************************************************
 * @fn      UsbKbd_DeviceTask
 * @brief   主循环里反复调：处理 USB 事件 + 有新按键状态就上报
 * @note    非阻塞；插拔线、枚举、上报全在这一个函数里推进
 *********************************************************************/
void UsbKbd_DeviceTask(void);

/*********************************************************************
 * @fn      UsbKbd_DeviceShutdown
 * @brief   关掉 USB（例如要切到 2.4G 省电时）
 *********************************************************************/
void UsbKbd_DeviceShutdown(void);

/*==========================================================================
 * 3. 状态查询 —— 上层靠这几个开关决定走哪条通道
 *========================================================================*/

/*********************************************************************
 * @fn      UsbKbd_IsConnected
 * @brief   USB 是否已经**枚举完成**、可以发 HID 报文
 * @return  true = 插着线并且主机已经 SET_CONFIGURATION
 *********************************************************************/
bool UsbKbd_IsConnected(void);

/*********************************************************************
 * @fn      UsbKbd_IsPlugged
 * @brief   线插着没有（比 IsConnected 宽松：只要总线活过就算）
 * @note    用来做"插了线但还没枚举完"的过渡状态：这段时间不要
 *          切去 2.4G，否则枚举会被打断
 *********************************************************************/
bool UsbKbd_IsPlugged(void);

UsbKbdState_t UsbKbd_GetState(void);

/* HID 类请求设进来的参数（调试/自测用） */
uint8_t  UsbKbd_GetProtocol(void);      /* USBK_PROTO_BOOT / REPORT */
uint16_t UsbKbd_GetIdle(void);          /* SET_IDLE 的 wValue，单位 4ms */

/* 统计 */
uint32_t UsbKbd_GetReportCount(void);   /* 成功塞进 EP1 IN 的报文数    */
uint32_t UsbKbd_GetLedCount(void);      /* 收到主机 LED 报文的次数     */
uint32_t UsbKbd_GetStallCount(void);    /* 不支持请求被 STALL 的次数   */
uint32_t UsbKbd_GetSetupCount(void);    /* 处理过的 SETUP 包数         */

/*==========================================================================
 * 4. 按键入口
 *    就是报文层的 UsbKbd_KeyEvent()（usb_kbd_report.h 里声明的那个），
 *    usb_kbd.c 不再包一层，避免同一个名字出现两份实现。
 *========================================================================*/

/*********************************************************************
 * @fn      UsbKbd_SendReportNow
 * @brief   不等"脏标志"，立刻把当前状态发一次
 * @return  true = 发出去了
 * @note    插上线的第一帧建议手动发一次全 0 报文，
 *          有些主机会因为"一直没收到报文"而报设备异常
 *********************************************************************/
bool UsbKbd_SendReportNow(void);

/*==========================================================================
 * 5. 【中断侧入口】EP0 必须在 USB 中断里处理完
 *
 * 为什么（本工程 USB 枚举 Code 43 的根因）：
 *   EP0 的收发缓冲 pEP0_DataBuf 和硬件 DMA 接收 SETUP 包用的是**同一块内存**。
 *   装包如果放到主循环，主循环一被刷屏（UI_Task，15MHz SPI 全屏约 35ms）
 *   或 Game_Task() 的 DelayMs(1) 占住，EP0 就没人喂、主机一直收 NAK；
 *   主机超时重发 SETUP 时，硬件 DMA 会把正在发的描述符数据覆盖掉，
 *   发出去的就是坏描述符 —— 实测就是 Code 10 / STATUS_DEVICE_DATA_ERROR。
 *
 *   WCH 全部 6 个 USB 例程都是"EP0 在中断里一次做完、主循环只收发应用数据"
 *   （它们的 HID 例程主循环 mDelaymS(100)x4，100ms 转一圈照样枚举成功）。
 *   这里对齐官方做法。
 *
 * EP1~EP4 的事件依旧走队列、由主循环 UsbKbd_DeviceTask() 处理
 * （那些端点主机允许 NAK 重试，晚一点没收不影响枚举）。
 *========================================================================*/
void UsbKbd_OnEp0Setup(const usbk_setup_t *su);   /* SETUP 阶段 */
void UsbKbd_OnEp0In(void);                        /* EP0 IN 发完 -> 装下一包 */
void UsbKbd_OnEp0Out(void);                       /* EP0 OUT 数据到 -> 取走并收尾 */

#ifdef __cplusplus
}
#endif

#endif /* __USB_KBD_H__ */
