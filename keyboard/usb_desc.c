/**************************************************************************
 * usb_desc.c —— USB 描述符表实现（标准 HID 键盘）
 **************************************************************************/

#include "usb_desc.h"
#include <string.h>

/* little-endian 16-bit read: every multi-byte descriptor field is LE */
static uint16_t usbdesc_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/*==========================================================================
 * 设备描述符（18 字节）
 *========================================================================*/
const uint8_t UsbDesc_Device[USBDESC_DEVICE_LEN] =
{
    USBDESC_DEVICE_LEN,         /*  0 bLength            = 18                  */
    USBDESC_T_DEVICE,           /*  1 bDescriptorType    = 0x01 设备           */
    0x10, 0x01,                 /*  2 bcdUSB             = 0x0110 (LE) USB1.1  */
    0x00,                       /*  4 bDeviceClass       = 0  类在接口里定义   */
    0x00,                       /*  5 bDeviceSubClass                          */
    0x00,                       /*  6 bDeviceProtocol                          */
    0x40,                       /*  7 bMaxPacketSize0    = 64                  */
    0x41, 0x3D,                 /*  8 idVendor           = 0x3D41 (LE) = WCH   */
    0x21, 0x07,                 /* 10 idProduct          = 0x0721 (LE)         */
    0x00, 0x01,                 /* 12 bcdDevice          = 0x0100 (LE)         */
    0x01,                       /* 14 iManufacturer      = 字符串 1            */
    0x02,                       /* 15 iProduct           = 字符串 2            */
    0x03,                       /* 16 iSerialNumber      = 字符串 3            */
    0x01                        /* 17 bNumConfigurations = 1                   */
};

/*==========================================================================
 * 配置描述符集合（41 字节）
 *
 *   偏移  0 ..  8   配置描述符
 *   偏移  9 .. 17   接口描述符
 *   偏移 18 .. 26   HID 描述符
 *   偏移 27 .. 33   端点描述符 EP1 IN
 *   偏移 34 .. 40   端点描述符 EP1 OUT
 *========================================================================*/
const uint8_t UsbDesc_Config[USBDESC_CONFIG_LEN] =
{
    /* ---- 配置描述符（9） ---- */
    0x09,                       /*  0 bLength                                  */
    USBDESC_T_CONFIG,           /*  1 bDescriptorType = 0x02                   */
    0x29, 0x00,                 /*  2 wTotalLength = 41 (LE)                   */
    0x01,                       /*  4 bNumInterfaces = 1                       */
    0x01,                       /*  5 bConfigurationValue = 1                  */
    0x00,                       /*  6 iConfiguration = 0（无字符串）           */
    0x80,                       /*  7 bmAttributes = bit7 必须为 1（总线供电） */
    0x32,                       /*  8 bMaxPower = 100 mA（单位 2mA）           */

    /* ---- 接口描述符（9） ---- */
    0x09,                       /*  9 bLength                                  */
    USBDESC_T_INTERFACE,        /* 10 bDescriptorType = 0x04                   */
    0x00,                       /* 11 bInterfaceNumber = 0                     */
    0x00,                       /* 12 bAlternateSetting = 0                    */
    0x02,                       /* 13 bNumEndpoints = 2（IN + OUT）            */
    0x03,                       /* 14 bInterfaceClass = 3      HID             */
    0x01,                       /* 15 bInterfaceSubClass = 1   Boot            */
    0x01,                       /* 16 bInterfaceProtocol = 1   Keyboard        */
    0x00,                       /* 17 iInterface = 0                           */

    /* ---- HID 描述符（9） ---- */
    0x09,                       /* 18 bLength                                  */
    USBDESC_T_HID,              /* 19 bDescriptorType = 0x21                   */
    0x11, 0x01,                 /* 20 bcdHID = 0x0111 (LE)                     */
    0x00,                       /* 22 bCountryCode = 0（不指定国家）           */
    0x01,                       /* 23 bNumDescriptors = 1                      */
    USBDESC_T_HID_REPORT,       /* 24 bDescriptorType = 0x22 报表描述符        */
    0x3F, 0x00,                 /* 25 wDescriptorLength = 63 (LE)              */

    /* ---- 端点描述符 EP1 IN（7） ---- */
    0x07,                       /* 27 bLength                                  */
    USBDESC_T_ENDPOINT,         /* 28 bDescriptorType = 0x05                   */
    0x81,                       /* 29 bEndpointAddress = 0x81 (EP1, IN)        */
    0x03,                       /* 30 bmAttributes = 0x03 中断传输             */
    0x08, 0x00,                 /* 31 wMaxPacketSize = 8 (LE)                  */
    0x01,                       /* 33 bInterval = 1 ms（1000Hz）               */

    /* ---- 端点描述符 EP1 OUT（7） ---- */
    0x07,                       /* 34 bLength                                  */
    USBDESC_T_ENDPOINT,         /* 35 bDescriptorType = 0x05                   */
    0x01,                       /* 36 bEndpointAddress = 0x01 (EP1, OUT)       */
    0x03,                       /* 37 bmAttributes = 0x03 中断传输             */
    0x08, 0x00,                 /* 38 wMaxPacketSize = 8 (LE)                  */
    0x01                        /* 40 bInterval = 1 ms                         */
};

/*==========================================================================
 * HID 报表描述符（63 字节）—— 标准 boot 键盘
 *
 *   输入报文 8 字节：1B 修饰键位域 + 1B 保留 + 6B 键码数组
 *   输出报文 1 字节：LED 位域（NumLock / CapsLock / ScrollLock）
 *========================================================================*/
const uint8_t UsbDesc_Report[USBDESC_REPORT_LEN] =
{
    0x05, 0x01,                 /* Usage Page (Generic Desktop)                */
    0x09, 0x06,                 /* Usage (Keyboard)                            */
    0xA1, 0x01,                 /* Collection (Application)                    */

    0x05, 0x07,                 /*   Usage Page (Keyboard/Keypad)              */
    0x19, 0xE0,                 /*   Usage Minimum (0xE0 LeftControl)          */
    0x29, 0xE7,                 /*   Usage Maximum (0xE7 Right GUI)            */
    0x15, 0x00,                 /*   Logical Minimum (0)                       */
    0x25, 0x01,                 /*   Logical Maximum (1)                       */
    0x75, 0x01,                 /*   Report Size (1)                           */
    0x95, 0x08,                 /*   Report Count (8)                          */
    0x81, 0x02,                 /*   Input (Data,Var,Abs)   ← 修饰键位图字节   */

    0x95, 0x01,                 /*   Report Count (1)                          */
    0x75, 0x08,                 /*   Report Size (8)                           */
    0x81, 0x01,                 /*   Input (Const)          ← 保留字节，固定 0 */

    0x95, 0x05,                 /*   Report Count (5)                          */
    0x75, 0x01,                 /*   Report Size (1)                           */
    0x05, 0x08,                 /*   Usage Page (LEDs)                         */
    0x19, 0x01,                 /*   Usage Minimum (NumLock, 0x01)             */
    0x29, 0x05,                 /*   Usage Maximum (Kana, 0x05)                */
    0x91, 0x02,                 /*   Output (Data,Var,Abs)  ← LED 位域         */
    0x95, 0x01,                 /*   Report Count (1)                          */
    0x75, 0x03,                 /*   Report Size (3)                           */
    0x91, 0x01,                 /*   Output (Const)         ← LED 补齐到 1 字节 */

    0x95, 0x06,                 /*   Report Count (6)                          */
    0x75, 0x08,                 /*   Report Size (8)                           */
    0x15, 0x00,                 /*   Logical Minimum (0)                       */
    0x25, 0x65,                 /*   Logical Maximum (101)                     */
    0x05, 0x07,                 /*   Usage Page (Key Codes)                    */
    0x19, 0x00,                 /*   Usage Minimum (0)                         */
    0x29, 0x65,                 /*   Usage Maximum (101)                       */
    0x81, 0x00,                 /*   Input (Data,Array)     ← 6 字节键码数组   */

    0xC0                        /* End Collection                              */
};

/*==========================================================================
 * 字符串描述符
 *
 * 格式： [bLength][0x03][UTF-16LE 字符……]
 *========================================================================*/
const char UsbDesc_VendorName[]  = "WCH";
const char UsbDesc_ProductName[] = "CH592X Keyboard";

/* 语言 ID：0x0409 = 美式英语 */
const uint8_t UsbDesc_StrLang[4] =
{
    0x04, USBDESC_T_STRING, 0x09, 0x04
};

/* "WCH" -> 3 个字符 -> 2 + 3*2 = 8 字节 */
const uint8_t UsbDesc_StrVendor[8] =
{
    0x08, USBDESC_T_STRING,
    'W', 0x00, 'C', 0x00, 'H', 0x00
};

/* "CH592X Keyboard" -> 15 个字符 -> 2 + 15*2 = 32 字节 */
const uint8_t UsbDesc_StrProduct[32] =
{
    0x20, USBDESC_T_STRING,
    'C', 0x00, 'H', 0x00, '5', 0x00, '9', 0x00, '2', 0x00, 'X', 0x00, ' ', 0x00,
    'K', 0x00, 'e', 0x00, 'y', 0x00, 'b', 0x00, 'o', 0x00, 'a', 0x00, 'r', 0x00,
    'd', 0x00
};

/* 序列号 "0001" -> 4 个字符 -> 2 + 4*2 = 10 字节
 * 真机上建议用芯片 UID 拼，这里先给个固定的占位值 */
const uint8_t UsbDesc_StrSerial[10] =
{
    0x0A, USBDESC_T_STRING,
    '0', 0x00, '0', 0x00, '0', 0x00, '1', 0x00
};

const uint8_t *const UsbDesc_String[USBDESC_STR_COUNT] =
{
    UsbDesc_StrLang,
    UsbDesc_StrVendor,
    UsbDesc_StrProduct,
    UsbDesc_StrSerial
};

/*==========================================================================
 * 查表
 *========================================================================*/
bool UsbDesc_Lookup(uint8_t desc_type, uint8_t index,
                    const uint8_t **pp, uint16_t *len)
{
    if ((pp == 0) || (len == 0))
    {
        return false;
    }

    switch (desc_type)
    {
        case USBDESC_T_DEVICE:
            *pp  = UsbDesc_Device;
            *len = (uint16_t)USBDESC_DEVICE_LEN;
            return true;

        case USBDESC_T_CONFIG:
            *pp  = UsbDesc_Config;
            *len = (uint16_t)USBDESC_CONFIG_LEN;
            return true;

        case USBDESC_T_HID_REPORT:      /* 0x22 */
        case USBDESC_T_HID:             /* 0x21：接口请求 HID 描述符时给配置里那 9 字节 */
            if (index != 0u)
            {
                return false;
            }
            if (desc_type == (uint8_t)USBDESC_T_HID_REPORT)
            {
                *pp  = UsbDesc_Report;
                *len = (uint16_t)USBDESC_REPORT_LEN;
            }
            else
            {
                *pp  = &UsbDesc_Config[USBDESC_CFG_OFF_HID];
                *len = 9u;
            }
            return true;

        case USBDESC_T_STRING:
            if (index >= (uint8_t)USBDESC_STR_COUNT)
            {
                return false;
            }
            *pp  = UsbDesc_String[index];
            *len = (uint16_t)UsbDesc_String[index][0];   /* 首字节就是长度 */
            return true;

        default:
            return false;
    }
}

/*==========================================================================
 * 自查
 *========================================================================*/
bool UsbDesc_IsValid(void)
{
    uint16_t total;
    uint8_t  i;

    /* 1) 设备描述符 */
    if (UsbDesc_Device[0] != (uint8_t)USBDESC_DEVICE_LEN)           { return false; }
    if (UsbDesc_Device[1] != (uint8_t)USBDESC_T_DEVICE)             { return false; }
    if (UsbDesc_Device[7] != (uint8_t)USBDESC_B_MAX_PACKET0)        { return false; }
    if (usbdesc_u16le(&UsbDesc_Device[8])  != (uint16_t)USBDESC_ID_VENDOR)  { return false; }
    if (usbdesc_u16le(&UsbDesc_Device[10]) != (uint16_t)USBDESC_ID_PRODUCT) { return false; }
    if (UsbDesc_Device[17] != 1u)                                   { return false; }

    /* 2) 配置描述符：wTotalLength 必须等于数组长度 */
    total = (uint16_t)((uint16_t)UsbDesc_Config[2] | ((uint16_t)UsbDesc_Config[3] << 8));
    if (total != (uint16_t)USBDESC_CONFIG_LEN)                      { return false; }
    if (UsbDesc_Config[1] != (uint8_t)USBDESC_T_CONFIG)             { return false; }
    if ((UsbDesc_Config[7] & 0x80u) == 0u)                          { return false; } /* bit7 必须 1 */

    /* 3) 接口描述符：必须是 HID / Boot / Keyboard */
    if (UsbDesc_Config[USBDESC_CFG_OFF_IFACE + 0u] != 9u)           { return false; }
    if (UsbDesc_Config[USBDESC_CFG_OFF_IFACE + 1u] != (uint8_t)USBDESC_T_INTERFACE) { return false; }
    if (UsbDesc_Config[USBDESC_CFG_OFF_IFACE + 5u] != 3u)           { return false; } /* HID     */
    if (UsbDesc_Config[USBDESC_CFG_OFF_IFACE + 6u] != 1u)           { return false; } /* Boot    */
    if (UsbDesc_Config[USBDESC_CFG_OFF_IFACE + 7u] != 1u)           { return false; } /* Keyboard*/
    if (UsbDesc_Config[USBDESC_CFG_OFF_IFACE + 4u] != 2u)           { return false; } /* 2 个端点 */

    /* 4) HID 描述符 */
    if (UsbDesc_Config[USBDESC_CFG_OFF_HID + 0u] != 9u)             { return false; }
    if (UsbDesc_Config[USBDESC_CFG_OFF_HID + 1u] != (uint8_t)USBDESC_T_HID) { return false; }
    if (usbdesc_u16le(&UsbDesc_Config[USBDESC_CFG_OFF_HID + 2u]) != (uint16_t)USBDESC_HID_BCD) { return false; }
    if (UsbDesc_Config[USBDESC_CFG_OFF_HID + 5u] != 1u)             { return false; } /* bNumDescriptors */
    if (UsbDesc_Config[USBDESC_CFG_OFF_HID + 6u] != (uint8_t)USBDESC_T_HID_REPORT) { return false; }
    if (usbdesc_u16le(&UsbDesc_Config[USBDESC_CFG_OFF_HID + 7u]) != (uint16_t)USBDESC_REPORT_LEN) { return false; }

    /* 5) 两个端点描述符 */
    for (i = 0u; i < 2u; i++)
    {
        uint16_t off = (i == 0u) ? (uint16_t)USBDESC_CFG_OFF_EP_IN
                                 : (uint16_t)USBDESC_CFG_OFF_EP_OUT;

        if (UsbDesc_Config[off + 0u] != 7u)                          { return false; }
        if (UsbDesc_Config[off + 1u] != (uint8_t)USBDESC_T_ENDPOINT) { return false; }
        if (UsbDesc_Config[off + 3u] != 0x03u)                       { return false; } /* 中断 */
        if (usbdesc_u16le(&UsbDesc_Config[off + 4u]) != (uint16_t)USBDESC_EP_MP_SIZE) { return false; }
        if (UsbDesc_Config[off + 6u] != (uint8_t)USBDESC_EP_INTERVAL_MS) { return false; }
    }
    if (UsbDesc_Config[USBDESC_CFG_OFF_EP_IN + 2u] != (uint8_t)USBDESC_EP_IN_ADDR)   { return false; }
    if (UsbDesc_Config[USBDESC_CFG_OFF_EP_OUT + 2u] != (uint8_t)USBDESC_EP_OUT_ADDR) { return false; }

    /* 6) 报表描述符：开头/结尾必须是键盘集合，长度要和 HID 描述符里声明的一致 */
    if (UsbDesc_Report[0] != 0x05u || UsbDesc_Report[1] != 0x01u)   { return false; }
    if (UsbDesc_Report[2] != 0x09u || UsbDesc_Report[3] != 0x06u)   { return false; }
    if (UsbDesc_Report[USBDESC_REPORT_LEN - 1u] != 0xC0u)           { return false; }

    /* 7) 字符串描述符：长度字段 = 2 + 2*字符数，且都是偶数 */
    for (i = 0u; i < (uint8_t)USBDESC_STR_COUNT; i++)
    {
        const uint8_t *s = UsbDesc_String[i];

        if (s == 0)                                                 { return false; }
        if (s[1] != (uint8_t)USBDESC_T_STRING)                      { return false; }
        if ((s[0] < 2u) || ((s[0] & 1u) != 0u))                     { return false; }
    }

    return true;
}

/******************* (C) CH592X USB 描述符 *****************/
