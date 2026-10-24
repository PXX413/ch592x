/**************************************************************************
 * usb_desc_cdc.h —— USB 复合设备描述符（HID 键盘 + CDC 虚拟串口）
 *
 * 和 usb_desc.h 的关系：
 *   usb_desc.h/.c      —— 原来的「纯 HID 键盘」描述符，保留不动，随时能退回
 *   usb_desc_cdc.h/.c  —— 本文件，「HID 键盘 + CDC 串口」复合设备描述符
 *
 * 用哪个由 usb_kbd.c 的编译开关决定：
 *   定义 USBKBD_ENABLE_CDC = 1  走复合设备（网页能直连改映射）
 *   定义 USBKBD_ENABLE_CDC = 0  走原来的纯键盘
 *
 * 这样改动是可逆的，不会把原功能弄坏。
 **************************************************************************/

#ifndef __USB_DESC_CDC_H__
#define __USB_DESC_CDC_H__

#include <stdint.h>
#include <stdbool.h>
#include "usb_desc.h"        /* 复用 USBDESC_T_xxx / USBDESC_REPORT_LEN 等 */

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 尺寸与接口号
 *========================================================================*/
#define USBDESCCDC_CONFIG_LEN   102u    /* HID(41) + IAD(8) + CDC(53) */     /* 配置集合总长（见 .c 里的计算） */
#define USBDESCCDC_NUM_IFACES   3u      /* HID + CDC控制 + CDC数据        */

/* 接口号（描述符里 bInterfaceNumber） */
#define USBDESCCDC_IF_HID       0u
#define USBDESCCDC_IF_CDC_CTRL  1u
#define USBDESCCDC_IF_CDC_DATA  2u

/*==========================================================================
 * 2. 配置集合里各段的偏移（按段索引查，便于设备逻辑取 HID 报表描述符）
 *========================================================================*/
#define USBDESCCDC_SEG_COUNT    14u

enum
{
    USBDESCCDC_SEG_CONFIG = 0,      /*  0  配置描述符           */
    USBDESCCDC_SEG_IF_HID,          /*  1  接口0 HID            */
    USBDESCCDC_SEG_HID,             /*  2  HID 描述符           */
    USBDESCCDC_SEG_EP1_IN,          /*  3  EP1 IN               */
    USBDESCCDC_SEG_EP1_OUT,         /*  4  EP1 OUT              */
    USBDESCCDC_SEG_IF_CDC_CTRL,     /*  5  接口1 CDC 控制       */
    USBDESCCDC_SEG_FUNC_HEADER,     /*  6  功能描述符 头        */
    USBDESCCDC_SEG_FUNC_ACM,        /*  7  功能描述符 ACM       */
    USBDESCCDC_SEG_FUNC_UNION,      /*  8  功能描述符 Union     */
    USBDESCCDC_SEG_EP4_IN,          /*  9  EP4 IN (通知)        */
    USBDESCCDC_SEG_IF_CDC_DATA,     /* 10  接口2 CDC 数据       */
    USBDESCCDC_SEG_EP2_OUT,         /* 11  EP2 OUT (数据收)     */
    USBDESCCDC_SEG_EP3_IN           /* 12  EP3 IN  (数据发)     */,
    USBDESCCDC_SEG_IAD              /* 13  IAD（接口关联）      */
};

extern const uint8_t  UsbDescCdc_Config[USBDESCCDC_CONFIG_LEN];
extern const uint16_t UsbDescCdc_Offsets[USBDESCCDC_SEG_COUNT];

/*--------------------------------------------------------------------------
 * 复合设备专用的**设备描述符**
 *
 * 和 usb_desc.c 的 UsbDesc_Device 只差 4~6 字节：
 *     bDeviceClass = 0xEF / SubClass = 0x02 / Protocol = 0x01
 * 这是 USB-IF 对"声明了 IAD 的复合设备"的硬性要求 ——
 * **Windows 只有在设备描述符是 0xEF/0x02/0x01 时才会解析 IAD**，
 * 否则 IAD 被忽略，CDC 的两个接口关联不起来，
 * 设备管理器就报"无法识别的 USB 设备"(Code 43)。
 *------------------------------------------------------------------------*/
#define USBDESCCDC_DEVICE_LEN   18u
extern const uint8_t UsbDescCdc_Device[USBDESCCDC_DEVICE_LEN];

/*==========================================================================
 * 3. 端点地址（描述符里 bEndpointAddress 的值）
 *========================================================================*/
#define USBDESCCDC_EP_HID_IN    0x81u
#define USBDESCCDC_EP_HID_OUT   0x01u
#define USBDESCCDC_EP_NOTIFY    0x84u   /* EP4 IN  中断，CDC 状态通知 */
#define USBDESCCDC_EP_DATA_OUT  0x02u   /* EP2 OUT 批量，收网页命令   */
#define USBDESCCDC_EP_DATA_IN   0x83u   /* EP3 IN  批量，回网页数据   */

/* 各端点单包最大字节数 */
#define USBDESCCDC_NOTIFY_MP    16u
#define USBDESCCDC_DATA_MP      64u

/*==========================================================================
 * 4. CDC 类请求（USB CDC 1.2 第 6.2 节）
 *========================================================================*/
#define USBDESCCDC_REQ_SET_LINE_CODING          0x20u
#define USBDESCCDC_REQ_GET_LINE_CODING          0x21u
#define USBDESCCDC_REQ_SET_CONTROL_LINE_STATE   0x22u

/* Line Coding 结构（7 字节），收到后原样存着，GET 时回给主机 */
typedef struct
{
    uint32_t dwDTERate;         /* 波特率，小端        */
    uint8_t  bCharFormat;       /* 停止位：0=1位       */
    uint8_t  bParityType;       /* 校验：0=无          */
    uint8_t  bDataBits;         /* 数据位：8           */
} usbcdc_linecoding_t;

/* CDC 状态通知的 bmRequestType（串口线状态，固定值） */
#define USBDESCCDC_NOTIFY_SERIAL_STATE  0xA1u
#define USBDESCCDC_NOTIFY_REQUEST       0x20u

/*==========================================================================
 * 5. 小工具
 *========================================================================*/
/* 小端 16 位读取 */
static inline uint16_t UsbDescCdc_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/*********************************************************************
 * @fn      UsbDescCdc_IsValid
 * @brief   自检：配置集合长度、接口号、端点地址是否都自洽
 * @return  true = 合法
 * @note    开机调一次，描述符写错了能第一时间发现（否则主机枚举
 *          失败会表现为"插上没反应"，很难查）
 *********************************************************************/
bool UsbDescCdc_IsValid(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_DESC_CDC_H__ */
