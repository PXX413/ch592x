/**************************************************************************
 * usb_desc_cdc.c —— USB 复合设备描述符（HID 键盘 + CDC 虚拟串口）
 *
 * 【为什么需要这个文件】
 *   原工程 usb_desc.c 只声明了一个 HID 接口。要让电脑上的网页通过串口
 *   直接改按键映射（不用重新烧录固件），必须再挂一个 CDC（虚拟串口）接口。
 *   一个设备同时提供两种功能，叫「复合设备」。
 *
 * 【接口布局】
 *   接口 0  HID 键盘      EP1 IN(中断) + EP1 OUT(中断)     —— 保持原样
 *   接口 1  CDC 控制       EP4 IN(中断)                     —— 类请求 + 串口状态通知
 *   接口 2  CDC 数据       EP2 OUT(批量) + EP3 IN(批量)     —— 实际的数据收发
 *
 * 【端点分配依据】
 *   查 usb_kbd_port_ch592.c：
 *     s_ep0_buf[64*3]  EP0 + EP4OUT + EP4IN       ← EP4 有独立缓冲
 *     s_ep1_buf[64*2]  EP1OUT + EP1IN             ← HID 占用
 *     s_ep23_buf[64]   EP2/EP3（原来是占位，没用）
 *   所以 EP2/EP3/EP4 都是空的，正好拿来给 CDC 用，不动 HID 一个字节。
 *
 * 【CDC 用到的类请求（USB CDC 1.2）】
 *   SET_LINE_CODING        0x20  主机下发波特率/停止位等（虚拟串口可忽略内容）
 *   GET_LINE_CODING        0x21  主机读回
 *   SET_CONTROL_LINE_STATE 0x22  主机开/关 DTR、RTS —— 用来判断"网页打开了串口"
 *
 * 【描述符长度计算（务必自洽）】
 *   配置描述符            9
 *   ├ 接口0 HID           9
 *   │  ├ HID               9
 *   │  ├ EP1 IN            7
 *   │  └ EP1 OUT           7
 *   ├ 接口1 CDC 控制       9
 *   │  ├ 功能描述符(头)     5
 *   │  ├ 功能描述符(ACM)    4
 *   │  ├ 功能描述符(Union)  5
 *   │  └ EP4 IN            7
 *   └ 接口2 CDC 数据       9
 *      ├ EP2 OUT            7
 *      └ EP3 IN             7
 *   合计 = 9 + (9+9+7+7) + (9+5+4+5+7) + (9+7+7) = 94 字节
 *
 * 【注意】所有多字节字段一律小端（LE）。这个和 USB 2.0 规范一致，
 *        也是 CH59x 硬件期望的字节序。
 **************************************************************************/

#include "usb_desc_cdc.h"
#include <string.h>

/*==========================================================================
 * 1. 配置描述符集合（94 字节）
 *========================================================================*/
const uint8_t UsbDescCdc_Config[USBDESCCDC_CONFIG_LEN] =
{
    /* ---------- 配置描述符（9） ---------- */
    0x09,                       /*  0 bLength                            */
    USBDESC_T_CONFIG,           /*  1 bDescriptorType = 0x02             */
    0x66, 0x00,                 /*  2 wTotalLength = 102 (小端)          */
    0x03,                       /*  4 bNumInterfaces = 3                 */
    0x01,                       /*  5 bConfigurationValue = 1            */
    0x00,                       /*  6 iConfiguration = 0（无字符串）     */
    0x80,                       /*  7 bmAttributes = 总线供电            */
    0x32,                       /*  8 bMaxPower = 100mA（单位 2mA）      */

    /* ---------- 接口 0：HID 键盘（9） ---------- */
    0x09,                       /*  9 bLength                            */
    USBDESC_T_INTERFACE,        /* 10 bDescriptorType = 0x04             */
    0x00,                       /* 11 bInterfaceNumber = 0               */
    0x00,                       /* 12 bAlternateSetting = 0              */
    0x02,                       /* 13 bNumEndpoints = 2                  */
    0x03,                       /* 14 bInterfaceClass = 3 (HID)          */
    0x01,                       /* 15 bInterfaceSubClass = 1 (Boot)      */
    0x01,                       /* 16 bInterfaceProtocol = 1 (Keyboard)  */
    0x00,                       /* 17 iInterface = 0                     */

    /* ---------- HID 描述符（9） ---------- */
    0x09,                       /* 18 bLength                            */
    USBDESC_T_HID,              /* 19 bDescriptorType = 0x21             */
    0x11, 0x01,                 /* 20 bcdHID = 0x0111 (小端)             */
    0x00,                       /* 22 bCountryCode = 0                   */
    0x01,                       /* 23 bNumDescriptors = 1                */
    USBDESC_T_HID_REPORT,       /* 24 bDescriptorType = 0x22 (报表)      */
    (uint8_t)(USBDESC_REPORT_LEN & 0xFFu),          /* 25 wDescriptorLength 低 */
    (uint8_t)((USBDESC_REPORT_LEN >> 8) & 0xFFu),   /* 26 wDescriptorLength 高 */

    /* ---------- 端点 EP1 IN：HID 键盘输入（7） ---------- */
    0x07,                       /* 27 bLength                            */
    USBDESC_T_ENDPOINT,         /* 28 bDescriptorType = 0x05             */
    0x81,                       /* 29 bEndpointAddress = EP1 IN          */
    0x03,                       /* 30 bmAttributes = 中断                */
    0x08, 0x00,                 /* 31 wMaxPacketSize = 8 (小端)          */
    0x01,                       /* 33 bInterval = 1ms                    */

    /* ---------- 端点 EP1 OUT：HID LED 输出（7） ---------- */
    0x07,                       /* 34 bLength                            */
    USBDESC_T_ENDPOINT,         /* 35 bDescriptorType = 0x05             */
    0x01,                       /* 36 bEndpointAddress = EP1 OUT         */
    0x03,                       /* 37 bmAttributes = 中断                */
    0x08, 0x00,                 /* 38 wMaxPacketSize = 8 (小端)          */
    0x01,                       /* 40 bInterval = 1ms                    */

    /* ---------- IAD：把接口 1+2 关联成一个 CDC 功能（8） ----------
     *
     * 【为什么必须有】
     *   CDC(ACM) 占用**两个**接口：控制接口 + 数据接口。
     *   一个设备的多个接口要组成"一个功能"，USB-IF 的 IAD ECN 规定
     *   必须放一个 Interface Association Descriptor 把它们绑在一起。
     *
     *   Windows 的复合设备父驱动 usbccgp 靠 IAD 判断怎么分组：
     *     没有 IAD -> 它把接口 1、接口 2 当成两个**独立**的功能，
     *                 接口 1（CDC 控制）单独一个没有驱动能匹配
     *                 -> 整个设备启动失败 -> 设备管理器报
     *                    "无法识别的 USB 设备"(Code 43)
     *       （注意 HID 键盘在接口 0，所以**不是**排在第一个的那个接口，
     *         老式的"第一个接口是 CDC 控制"隐式分组在这里也不成立。）
     *
     *   另外：声明了 IAD 的设备，设备描述符的 bDeviceClass 必须是
     *   0xEF / 0x02 / 0x01，Windows 才会启用 IAD 解析 ——
     *   见本文件末尾的 UsbDescCdc_Device。
     *--------------------------------------------------------------- */
    0x08,                       /* 41 bLength = 8                       */
    0x0B,                       /* 42 bDescriptorType = 0x0B (IAD)      */
    0x01,                       /* 43 bFirstInterface = 1（CDC 控制）   */
    0x02,                       /* 44 bInterfaceCount = 2（控制+数据）  */
    0x02,                       /* 45 bFunctionClass = 0x02 (CDC 控制)  */
    0x02,                       /* 46 bFunctionSubClass = 0x02 (ACM)    */
    0x01,                       /* 47 bFunctionProtocol = 0x01 (AT)     */
    0x00,                       /* 48 iFunction = 0                     */

    /* ---------- 接口 1：CDC 控制接口（9） ---------- */
    0x09,                       /* 41 bLength                            */
    USBDESC_T_INTERFACE,        /* 42 bDescriptorType = 0x04             */
    0x01,                       /* 43 bInterfaceNumber = 1               */
    0x00,                       /* 44 bAlternateSetting = 0              */
    0x01,                       /* 45 bNumEndpoints = 1                  */
    0x02,                       /* 46 bInterfaceClass = 2 (CDC 通信)     */
    0x02,                       /* 47 bInterfaceSubClass = 2 (ACM 抽象猫) */
    0x01,                       /* 48 bInterfaceProtocol = 1 (AT 命令)   */
    0x00,                       /* 49 iInterface = 0                     */

    /* ---- 功能描述符：头（5） ---- */
    0x05,                       /* 50 bLength                            */
    0x24,                       /* 51 bDescriptorType = 0x24 (CS_INTERFACE) */
    0x00,                       /* 52 bDescriptorSubType = 0 (Header)     */
    0x10, 0x01,                 /* 53 bcdCDC = 0x0110 (小端)             */

    /* ---- 功能描述符：ACM（4） ---- */
    0x04,                       /* 55 bLength                            */
    0x24,                       /* 56 bDescriptorType = 0x24             */
    0x02,                       /* 57 bDescriptorSubType = 2 (ACM)        */
    0x02,                       /* 58 bmCapabilities = 支持 line coding  */

    /* ---- 功能描述符：Union（5） ---- */
    0x05,                       /* 59 bLength                            */
    0x24,                       /* 60 bDescriptorType = 0x24             */
    0x06,                       /* 61 bDescriptorSubType = 6 (Union)      */
    0x01,                       /* 62 bControlInterface = 1（控制接口）  */
    0x02,                       /* 63 bSubordinateInterface0 = 2（数据） */

    /* ---------- 端点 EP4 IN：CDC 状态通知（7） ---------- */
    0x07,                       /* 64 bLength                            */
    USBDESC_T_ENDPOINT,         /* 65 bDescriptorType = 0x05             */
    0x84,                       /* 66 bEndpointAddress = EP4 IN          */
    0x03,                       /* 67 bmAttributes = 中断                */
    0x10, 0x00,                 /* 68 wMaxPacketSize = 16 (小端)         */
    0x10,                       /* 70 bInterval = 16ms                   */

    /* ---------- 接口 2：CDC 数据接口（9） ---------- */
    0x09,                       /* 71 bLength                            */
    USBDESC_T_INTERFACE,        /* 72 bDescriptorType = 0x04             */
    0x02,                       /* 73 bInterfaceNumber = 2               */
    0x00,                       /* 74 bAlternateSetting = 0              */
    0x02,                       /* 75 bNumEndpoints = 2                  */
    0x0A,                       /* 76 bInterfaceClass = 10 (CDC 数据)    */
    0x00,                       /* 77 bInterfaceSubClass = 0             */
    0x00,                       /* 78 bInterfaceProtocol = 0             */
    0x00,                       /* 79 iInterface = 0                     */

    /* ---------- 端点 EP2 OUT：CDC 数据接收（7） ---------- */
    0x07,                       /* 80 bLength                            */
    USBDESC_T_ENDPOINT,         /* 81 bDescriptorType = 0x05             */
    0x02,                       /* 82 bEndpointAddress = EP2 OUT         */
    0x02,                       /* 83 bmAttributes = 批量                */
    0x40, 0x00,                 /* 84 wMaxPacketSize = 64 (小端)         */
    0x00,                       /* 86 bInterval = 0（批量不用）          */

    /* ---------- 端点 EP3 IN：CDC 数据发送（7） ---------- */
    0x07,                       /* 87 bLength                            */
    USBDESC_T_ENDPOINT,         /* 88 bDescriptorType = 0x05             */
    0x83,                       /* 89 bEndpointAddress = EP3 IN          */
    0x02,                       /* 90 bmAttributes = 批量                */
    0x40, 0x00,                 /* 91 wMaxPacketSize = 64 (小端)         */
    0x00                        /* 93 bInterval = 0                      */
};

/*==========================================================================
 * 2. 各段在配置集合里的偏移（设备逻辑按接口号返回对应段）
 *========================================================================*/
const uint16_t UsbDescCdc_Offsets[USBDESCCDC_SEG_COUNT] =
{
    0u,     /* 0  配置描述符            */
    9u,     /* 1  接口 0 HID            */
    18u,    /* 2  HID 描述符            */
    27u,    /* 3  EP1 IN                */
    34u,    /* 4  EP1 OUT               */
    49u,    /* 5  接口 1 CDC 控制       （41 + 8，IAD 插在它前面）*/
    58u,    /* 6  功能描述符 头         （50 + 8）*/
    63u,    /* 7  功能描述符 ACM        （55 + 8）*/
    67u,    /* 8  功能描述符 Union      （59 + 8）*/
    72u,    /* 9  EP4 IN                （64 + 8）*/
    79u,    /* 10 接口 2 CDC 数据       （71 + 8）*/
    88u,    /* 11 EP2 OUT               （80 + 8）*/
    95u,    /* 12 EP3 IN                （87 + 8）*/
    41u     /* 13 IAD（接口关联，新增） */
};

/*==========================================================================
 * 3. 自检
 *========================================================================*/
bool UsbDescCdc_IsValid(void)
{
    uint8_t  i;
    uint8_t  off;

    /* --- 配置描述符里的总长必须等于数组实际长度 --- */
    if (UsbDescCdc_u16le(&UsbDescCdc_Config[2]) != (uint16_t)USBDESCCDC_CONFIG_LEN)
    {
        return false;
    }
    if (UsbDescCdc_Config[4] != (uint8_t)USBDESCCDC_NUM_IFACES)
    {
        return false;
    }

    /* --- 逐个描述符走一遍，检查 bLength 串起来正好覆盖整个数组 --- */
    off = 0u;
    while (off < (uint8_t)USBDESCCDC_CONFIG_LEN)
    {
        uint8_t blen = UsbDescCdc_Config[off];

        if (blen == 0u)
        {
            return false;               /* 长度为 0 会死循环 */
        }
        if ((uint16_t)off + (uint16_t)blen > (uint16_t)USBDESCCDC_CONFIG_LEN)
        {
            return false;               /* 越界 */
        }
        off = (uint8_t)(off + blen);
    }

    if (off != (uint8_t)USBDESCCDC_CONFIG_LEN)
    {
        return false;                   /* 段长度加不满，说明中间有错 */
    }

    /* --- 关键字段抽查：接口号、类、端点地址 ---
     *
     * 【2026-10-19 修复】IAD 插在第 41 字节之后，后半段所有描述符整体
     * 后移 8 字节；这里的抽查下标却没跟着改，导致描述符明明能被电脑
     * 正常枚举（verify_usbdesc.py 102 字节全通过），UsbDescCdc_IsValid()
     * 却一直返回 false。改成按各段真实起始偏移检查。 */
    if (UsbDescCdc_Config[11] != 0u) { return false; }   /* 接口0 号 = 0 */
    if (UsbDescCdc_Config[14] != 0x03u) { return false; }/* 接口0 类 = HID */

    if (UsbDescCdc_Config[42] != 0x0Bu) { return false; }/* IAD 类型 */
    if (UsbDescCdc_Config[43] != 0x01u) { return false; }/* IAD 首接口 = 1 */
    if (UsbDescCdc_Config[44] != 0x02u) { return false; }/* IAD 接口数 = 2 */

    if (UsbDescCdc_Config[51] != 0x01u) { return false; }/* 接口1 号 = 1 */
    if (UsbDescCdc_Config[54] != 0x02u) { return false; }/* 接口1 类 = CDC */
    if (UsbDescCdc_Config[81] != 0x02u) { return false; }/* 接口2 号 = 2 */
    if (UsbDescCdc_Config[84] != 0x0Au) { return false; }/* 接口2 类 = CDC 数据 */

    if (UsbDescCdc_Config[29] != 0x81u) { return false; }/* EP1 IN  */
    if (UsbDescCdc_Config[36] != 0x01u) { return false; }/* EP1 OUT */
    if (UsbDescCdc_Config[74] != 0x84u) { return false; }/* EP4 IN  */
    if (UsbDescCdc_Config[90] != 0x02u) { return false; }/* EP2 OUT */
    if (UsbDescCdc_Config[97] != 0x83u) { return false; }/* EP3 IN  */

    (void)i;
    return true;
}

/*==========================================================================
 * 3. 复合设备用的**设备描述符**
 *
 * 和 usb_desc.c 的 UsbDesc_Device 只差 4~6 字节：
 *     bDeviceClass    = 0xEF  杂项设备
 *     bDeviceSubClass = 0x02  按接口关联(IAD)定义
 *     bDeviceProtocol = 0x01
 * 这是 USB-IF 对"声明了 IAD 的复合设备"的硬性要求：
 * **Windows 只有在设备描述符是 0xEF/0x02/0x01 时才会解析 IAD**，
 * 否则 IAD 会被忽略，又退回到"每个接口各自为政"的坏行为。
 *
 * 其余字段和 UsbDesc_Device 完全一致（同一套字符串、同一个 VID/PID），
 * 这样在设备管理器里还是同一个"CH592X Keyboard"。
 *========================================================================*/
const uint8_t UsbDescCdc_Device[USBDESCCDC_DEVICE_LEN] =
{
    0x12,                       /*  0 bLength           = 18                  */
    USBDESC_T_DEVICE,           /*  1 bDescriptorType   = 0x01 设备           */
    0x10, 0x01,                 /*  2 bcdUSB            = 0x0110 (LE) USB1.1  */
    0xEF,                       /*  4 bDeviceClass      = 0xEF 杂项（复合）   */
    0x02,                       /*  5 bDeviceSubClass   = 0x02 按 IAD 关联    */
    0x01,                       /*  6 bDeviceProtocol   = 0x01                */
    0x40,                       /*  7 bMaxPacketSize0   = 64                  */
    0x41, 0x3D,                 /*  8 idVendor          = 0x3D41 (LE) = WCH   */
    0x21, 0x07,                 /* 10 idProduct         = 0x0721 (LE)         */
    0x00, 0x01,                 /* 12 bcdDevice         = 0x0100 (LE)         */
    0x01,                       /* 14 iManufacturer     = 字符串 1            */
    0x02,                       /* 15 iProduct          = 字符串 2            */
    0x03,                       /* 16 iSerialNumber     = 字符串 3            */
    0x01                        /* 17 bNumConfigurations = 1                  */
};
