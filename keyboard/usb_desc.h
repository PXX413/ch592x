/**************************************************************************
 * usb_desc.h —— USB 描述符表（标准 HID 键盘）
 *
 * 依据：USB Device Class Definition for HID v1.11 + HID Usage Tables v1.5
 *
 * 组成：
 *      设备描述符        18 字节
 *      配置描述符集合    41 字节
 *          ├ 配置描述符  9
 *          ├ 接口描述符  9    bInterfaceClass=3 / SubClass=1 / Protocol=1
 *          ├ HID 描述符  9    bcdHID=0x0111, bNumDescriptors=1, type=0x22
 *          ├ 端点描述符  7    EP1 IN  中断 8B 1ms
 *          └ 端点描述符  7    EP1 OUT 中断 8B 1ms（收 LED 报文）
 *      HID 报表描述符    63 字节
 *      字符串描述符      语言ID / 厂商 / 产品 / 序列号
 *
 * 所有多字节字段一律**小端**。
 *
 * ------------------------------------------------------------------
 * 【为什么比"最少要求"多了一个 EP1 OUT】
 * 需求写的是"EP1 OUT 收 LED 报文"，但标准 boot 键盘其实是用**控制端点**
 * 下发 LED 的（SET_REPORT，bmRequestType=0x21）。两种做法主机都有：
 * Windows / Linux 走控制端点，部分固件/工控主机走中断 OUT 端点。
 * 所以这里两个都做：描述符里声明 EP1 OUT，代码里 SET_REPORT 和 EP1 OUT
 * 两条路都收，谁先来用谁。
 **************************************************************************/

#ifndef __USB_DESC_H__
#define __USB_DESC_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 尺寸
 *========================================================================*/
#define USBDESC_DEVICE_LEN      18u
#define USBDESC_CONFIG_LEN      41u
#define USBDESC_REPORT_LEN      63u

/* 配置集合里各段的偏移（设备逻辑要按接口号返回 HID 描述符的起始地址） */
#define USBDESC_CFG_OFF_CONFIG  0u
#define USBDESC_CFG_OFF_IFACE   9u
#define USBDESC_CFG_OFF_HID     18u
#define USBDESC_CFG_OFF_EP_IN   27u
#define USBDESC_CFG_OFF_EP_OUT  34u

/* 描述符类型（USB 2.0 第 9.6 节） */
#define USBDESC_T_DEVICE        0x01u
#define USBDESC_T_CONFIG        0x02u
#define USBDESC_T_STRING        0x03u
#define USBDESC_T_INTERFACE     0x04u
#define USBDESC_T_ENDPOINT      0x05u
#define USBDESC_T_HID           0x21u
#define USBDESC_T_HID_REPORT    0x22u

/* 字符串描述符索引 */
#define USBDESC_STR_LANGID      0u
#define USBDESC_STR_VENDOR      1u
#define USBDESC_STR_PRODUCT     2u
#define USBDESC_STR_SERIAL      3u
#define USBDESC_STR_COUNT       4u

/*==========================================================================
 * 2. 描述符表
 *========================================================================*/
extern const uint8_t UsbDesc_Device[USBDESC_DEVICE_LEN];
extern const uint8_t UsbDesc_Config[USBDESC_CONFIG_LEN];
extern const uint8_t UsbDesc_Report[USBDESC_REPORT_LEN];

extern const uint8_t *const UsbDesc_String[USBDESC_STR_COUNT];

/* 各条字符串也单独导出，方便自测逐字节比对（长度见数组首字节） */
extern const uint8_t UsbDesc_StrLang[4];
extern const uint8_t UsbDesc_StrVendor[8];
extern const uint8_t UsbDesc_StrProduct[32];
extern const uint8_t UsbDesc_StrSerial[10];

/* 厂商 / 产品信息（改产品名只要改 usb_desc.c 里那两张表） */
extern const char UsbDesc_VendorName[];
extern const char UsbDesc_ProductName[];

/*********************************************************************
 * @fn      UsbDesc_TotalLen
 * @brief   配置集合的总长度（配置描述符的 wTotalLength）
 *********************************************************************/
static inline uint16_t UsbDesc_TotalLen(void)
{
    return (uint16_t)USBDESC_CONFIG_LEN;
}

/*********************************************************************
 * @fn      UsbDesc_Lookup
 * @brief   按 (描述符类型, 索引) 找一段描述符 —— 设备逻辑只调这一个
 * @param   desc_type  USB 描述符类型（0x01 设备 / 0x02 配置 / 0x22 报表 / 0x03 字符串）
 * @param   index      字符串描述符用它是语言 ID 序号；HID/报表描述符用它是接口号
 * @param   pp         输出：描述符首地址
 * @param   len        输出：描述符长度
 * @return  true = 找到了
 * @note    报表描述符（0x22）和 HID 描述符（0x21）都映射到 UsbDesc_Report /
 *          配置里的 HID 段，主机要哪份都能给。index 不是 0 一律当找不到。
 *********************************************************************/
bool UsbDesc_Lookup(uint8_t desc_type, uint8_t index,
                    const uint8_t **pp, uint16_t *len);

/*********************************************************************
 * @fn      UsbDesc_IsValid
 * @brief   自查：把描述符表过一遍，检查长度字段自洽、多字节字段是小端
 * @return  true = 全部合法
 * @note    给 PC 自测用的，也可以在真机上电时跑一次当自检
 *********************************************************************/
bool UsbDesc_IsValid(void);

/*==========================================================================
 * 3. 描述符里的关键字段（自测和上层查询用，避免到处写魔法下标）
 *========================================================================*/
#define USBDESC_ID_VENDOR       0x3D41u     /* WCH 的 VID（示例工程用的就是这个） */
#define USBDESC_ID_PRODUCT      0x0721u
#define USBDESC_BCD_USB         0x0110u
#define USBDESC_BCD_DEVICE      0x0100u
#define USBDESC_B_MAX_PACKET0   64u
#define USBDESC_EP_IN_ADDR      0x81u
#define USBDESC_EP_OUT_ADDR     0x01u
#define USBDESC_EP_MP_SIZE      8u          /* 8 字节 = boot 键盘报文 */
#define USBDESC_EP_INTERVAL_MS  1u          /* 1ms -> 1000Hz */
#define USBDESC_HID_BCD         0x0111u

#ifdef __cplusplus
}
#endif

#endif /* __USB_DESC_H__ */
