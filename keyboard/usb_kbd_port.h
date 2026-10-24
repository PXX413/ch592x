/**************************************************************************
 * usb_kbd_port.h —— USB 键盘硬件抽象层（Port）
 *
 * 把"USB 外设怎么用"关在这一层后面，让 usb_kbd.c 只处理协议逻辑：
 *      * 真机：usb_kbd_port_ch592.c —— 直接读写 CH59x 的 USB 寄存器
 *      * PC  ：tests\stub\usb_kbd_port_sim.c —— 一个虚拟 USB 主机 + 虚拟端点
 *
 * 事件模型：
 *      USB 中断里只往环形队列里塞事件（不干活），主循环 UsbPort_PollEvent()
 *      一个个取出来处理 —— 这样上层逻辑在 PC 上能完整复现。
 *
 * 关于 EP0 控制传输：
 *      USB 控制传输是"SETUP → 若干 IN/OUT 数据包 → 状态包"的多包过程。
 *      这里不做隐藏的自动分片，而是把 IN 令牌显式暴露成
 *      USBK_EV_EP0_IN 事件，由 usb_kbd.c 自己决定下一包发什么
 *      （和 WCH 例程里 SetupReqLen / pDescr 那套是等价的做法，但可测）。
 **************************************************************************/

#ifndef __USB_KBD_PORT_H__
#define __USB_KBD_PORT_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 端点号（和描述符里的 bEndpointAddress 对应）
 *========================================================================*/
#define USBPORT_EP0             0u
#define USBPORT_EP1             1u
#define USBPORT_EP2             2u      /* CDC 数据 OUT，接收缓冲区 */
#define USBPORT_EP3             3u      /* CDC 数据 IN 发送缓冲区 */
#define USBPORT_EP4             4u      /* CDC 通知 IN，串口线状态中断端点 */

/* 端点 0 单包最大字节数（设备描述符里 bMaxPacketSize0 = 64） */
#define USBPORT_EP0_MP          64u
/* 端点 1 单包最大字节数（= boot 键盘报文长度） */
#define USBPORT_EP1_MP          8u
/* 端点 2/3/4 单包最大字节数，USB 全速端点的硬上限是 64 */
#define USBPORT_EP2_MP          64u
#define USBPORT_EP3_MP          64u
#define USBPORT_EP4_MP          64u

/*==========================================================================
 * 事件
 *========================================================================*/
typedef enum
{
    USBK_EV_NONE = 0,

    USBK_EV_RESET,          /* 总线复位：所有端点清零，回到"未配置"        */
    USBK_EV_SETUP,          /* 收到 SETUP 包，setup 参数已填好             */
    USBK_EV_EP0_IN,         /* EP0 的 IN 令牌：该发下一包了                */
    USBK_EV_EP0_OUT,        /* EP0 的 OUT 数据到了（setup 阶段的数据）     */
    USBK_EV_EP0_STATUS,     /* EP0 状态阶段完成（一次控制传输结束了）      */
    USBK_EV_EP1_IN,         /* EP1 IN 的报文被主机取走了                    */
    USBK_EV_EP1_OUT,        /* EP1 OUT 收到数据（LED 报文）                */
    USBK_EV_EP2_OUT,        /* EP2 OUT 收到 CDC 数据（主机发来的）    */
    USBK_EV_EP3_IN,         /* EP3 IN 上一包被主机取走了，可以再发       */
    USBK_EV_EP4_IN,         /* EP4 IN 通知报文已发出，无需处理             */
    USBK_EV_SUSPEND,        /* 主机让我挂起                                */
    USBK_EV_RESUME          /* 主机唤醒我                                  */
} usbk_evt_t;

/*==========================================================================
 * SETUP 包（USB 2.0 第 9.3 节，8 字节，全是小端）
 *========================================================================*/
typedef struct
{
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usbk_setup_t;

/* bmRequestType 的位域 */
#define USBK_REQ_DIR_MASK       0x80u
#define USBK_REQ_DIR_IN         0x80u       /* 主机 -> 设备方向是"设备发数据" */
#define USBK_REQ_TYP_MASK       0x60u
#define USBK_REQ_TYP_STANDARD   0x00u
#define USBK_REQ_TYP_CLASS      0x20u
#define USBK_REQ_TYP_VENDOR     0x40u
#define USBK_REQ_RECIP_MASK     0x1Fu
#define USBK_REQ_RECIP_DEVICE   0x00u
#define USBK_REQ_RECIP_IFACE    0x01u
#define USBK_REQ_RECIP_ENDP     0x02u

/* 标准请求码 */
#define USBK_REQ_GET_STATUS     0x00u
#define USBK_REQ_CLEAR_FEATURE  0x01u
#define USBK_REQ_SET_FEATURE    0x03u
#define USBK_REQ_SET_ADDRESS    0x05u
#define USBK_REQ_GET_DESCRIPTOR 0x06u
#define USBK_REQ_SET_DESCRIPTOR 0x07u
#define USBK_REQ_GET_CONFIG     0x08u
#define USBK_REQ_SET_CONFIG     0x09u
#define USBK_REQ_GET_INTERFACE  0x0Au
#define USBK_REQ_SET_INTERFACE  0x0Bu

/* HID 类请求码（HID 1.11 第 7.2 节） */
#define USBK_HID_GET_REPORT     0x01u
#define USBK_HID_GET_IDLE       0x02u
#define USBK_HID_GET_PROTOCOL   0x03u
#define USBK_HID_SET_REPORT     0x09u
#define USBK_HID_SET_IDLE       0x0Au
#define USBK_HID_SET_PROTOCOL   0x0Bu

/*==========================================================================
 * 接口
 *========================================================================*/

/*********************************************************************
 * @fn      UsbPort_Init
 * @brief   配好 USB 外设（时钟、引脚、端点缓冲）并打开中断
 * @note    真机后端里 EP 缓冲是静态数组，必须 __attribute__((aligned(4)))
 *********************************************************************/
void UsbPort_Init(void);

/* 关掉 USB（拔线后想省电就调它） */
void UsbPort_Shutdown(void);

/*********************************************************************
 * @fn      UsbPort_PollEvent
 * @brief   从事件队列取一个事件（没事件返回 USBK_EV_NONE）
 * @param   setup  收到 USBK_EV_SETUP 时，SETUP 包参数写这里
 *********************************************************************/
usbk_evt_t UsbPort_PollEvent(usbk_setup_t *setup);

/*********************************************************************
 * @fn      UsbPort_Ep0Send
 * @brief   在 EP0 上发一包（≤64 字节）
 * @note    只在收到 USBK_EV_EP0_IN 之后调；多包传输由上层分次调用
 *********************************************************************/
void UsbPort_Ep0Send(const uint8_t *buf, uint16_t len);

/*********************************************************************
 * @fn      UsbPort_Ep0Recv
 * @brief   读走 EP0 OUT 阶段收到的数据
 * @return  实际读到的字节数
 *********************************************************************/
uint16_t UsbPort_Ep0Recv(uint8_t *buf, uint16_t cap);

/* EP0 状态阶段：回 ACK（正常结束） */
void UsbPort_Ep0Ack(void);

/* EP0 状态阶段：回 STALL（不支持这个请求） */
void UsbPort_Ep0Stall(void);

/*********************************************************************
 * @fn      UsbPort_SetPendingAddress
 * @brief   收到 SET_ADDRESS 后把新地址记下来
 * @note    真正写进硬件必须等到这次控制传输的**状态阶段**（IN 令牌）
 *          之后，否则主机还没确认你换地址，后续令牌就抓不到了。
 *          这个"什么时候生效"的时序藏在后端里，上层不用管。
 *********************************************************************/
void UsbPort_SetPendingAddress(uint8_t addr);

/*********************************************************************
 * @fn      UsbPort_Ep1Send
 * @brief   把 8 字节报文装进 EP1 IN，等主机来取
 * @return  true = 装进去了；false = 上一次的还没被取走（别覆盖）
 *********************************************************************/
bool UsbPort_Ep1Send(const uint8_t *buf, uint8_t len);

/* 上一次 EP1 IN 的报文被取走了吗（可以送下一份） */
bool UsbPort_Ep1Ready(void);

/*********************************************************************
 * @fn      UsbPort_Ep1Recv
 * @brief   读走 EP1 OUT 收到的 LED 报文
 * @return  实际读到的字节数（0 = 没有）
 *********************************************************************/
uint16_t UsbPort_Ep1Recv(uint8_t *buf, uint16_t cap);

/*==========================================================================
 * CDC 端点（EP2 OUT / EP3 IN / EP4 IN）
 *
 * 这几个接口只在 USBKBD_ENABLE_CDC = 1 时才被调用
 * 没有真实后端时默认弱实现一律返回 0 / true，就算在 USBCDC_ENABLE = 0 时
 * 上层也不会卡。
 *========================================================================*/

/*********************************************************************
 * @fn      UsbPort_Ep2Recv
 * @brief   读取 EP2 OUT 收到的 CDC 数据（主机发来的）
 * @return  实际读到的字节数（0 = 没有）
 * @note    每一次 OUT 数据被读走后计数清零，新包来了会覆盖旧包
 *********************************************************************/
uint16_t UsbPort_Ep2Recv(uint8_t *buf, uint16_t cap);

/*********************************************************************
 * @fn      UsbPort_Ep3Send
 * @brief   往 EP3 IN 填一包 CDC 数据，等主机取
 * @return  true = 装进去了；false = 上一包还没被取走（别覆盖）
 * @note    可以先查 UsbPort_Ep3Ready() 再发，没空就失败
 *          单包最大长度超过 USBPORT_EP3_MP 的部分会被直接截掉
 *********************************************************************/
bool UsbPort_Ep3Send(const uint8_t *buf, uint16_t len);

/* 上一次 EP3 IN 的报文被主机取走了吗（可以发下一包） */
bool UsbPort_Ep3Ready(void);

/*********************************************************************
 * @fn      UsbPort_Ep4Send
 * @brief   往 EP4 IN 发一条 CDC 通知（16 字节）。
 * @return  true = 装进去了
 * @note    CDC 的通知一般用来报告串口状态，
 *          本工程暂时没有发过它，接口留着以后用
 *          真要发时先看空闲，别覆盖上一包
 *********************************************************************/
bool UsbPort_Ep4Send(const uint8_t *buf, uint8_t len);

/* EP4 IN 上一包通知被取走了吗 */
bool UsbPort_Ep4Ready(void);

/*==========================================================================
 * 状态查询
 *========================================================================*/
bool    UsbPort_IsConfigured(void);     /* 主机已经 SET_CONFIGURATION 过了 */

/*********************************************************************
 * @fn      UsbPort_SetConfigured
 * @brief   上层处理完 SET_CONFIGURATION 后告诉端口层
 * @note    端口本身不解析 SETUP 包（那是 usb_kbd.c 的活），
 *          所以这个状态得由上层回填
 *********************************************************************/
void    UsbPort_SetConfigured(bool on);

uint8_t UsbPort_Address(void);          /* 当前设备地址 */
bool    UsbPort_IsSuspended(void);

/* 自测/调试：清掉事件队列和所有状态 */
void UsbPort_ResetState(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_KBD_PORT_H__ */
