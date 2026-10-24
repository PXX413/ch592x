/**************************************************************************
 * usb_kbd.c —— CH592X USB HID 键盘设备侧主逻辑
 *
 * 这一个文件负责 USB 控制传输的全部"应用层"动作：
 *
 *   1) 标准请求：GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION /
 *      GET_CONFIGURATION / GET_STATUS / GET_INTERFACE / SET_INTERFACE /
 *      CLEAR_FEATURE / SET_FEATURE
 *   2) HID 类请求：GET_REPORT / SET_REPORT / GET_IDLE / SET_IDLE /
 *      GET_PROTOCOL / SET_PROTOCOL
 *   3) 按键状态 -> EP1 IN 中断上报（带"上一包被取走了才发下一包"的节流）
 *   4) 主机下发的 LED 报文（SET_REPORT 走控制端点，或者 EP1 OUT 走中断端点）
 *   5) 带CDC 虚拟串口的数据收发 + CDC 线编码 + EP2/EP3/EP4 事件处理
 *
 * 不碰任何寄存器 —— 硬件动作全部经 usb_kbd_port.h。
 * ------------------------------------------------------------------
 * 宏USBKBD_ENABLE_CDC 开关：
 *   = 1（默认）：USB 复合设备 = HID 键盘 + CDC 虚拟串口
 *        屏幕和按键映射页直接走设备窗口，不用再绕任何中间层
 *   = 0：关闭，回到原本"仅 HID 键盘"的老路
 *        描述符取 usb_desc.h/.c，剥离掉 CDC 相关代码
 *
 * 这两个版本是互相隔离的：把宏改成 0 编译，整个数据通道回到老版本。
 **************************************************************************/

#include "usb_kbd.h"
#include "usb_desc.h"

#if (USBKBD_ENABLE_CDC != 0)
#include "usb_desc_cdc.h"
#include "usb_cdc.h"
#endif

#include <string.h>

/*==========================================================================
 * CDC 开关的默认值
 *
 * 这里几个宏的默认值都在这一处，关掉时就不参与编译。
 *========================================================================*/
#if (USBKBD_ENABLE_CDC != 0)
#define USBKBD_CDC_ON       1
#else
#define USBKBD_CDC_ON       0
#endif

/*==========================================================================
 * 状态
 *========================================================================*/
static UsbKbdState_t s_state;
static uint8_t       s_protocol;        /* USBK_PROTO_* */
static uint16_t      s_idle;            /* SET_IDLE 的 wValue（单位 4ms） */

/* EP0 数据阶段（发描述符）用的游标 */
static const uint8_t *s_pend_ptr;
static uint16_t       s_pend_len;
static bool           s_pend_active;

/* SET_REPORT 的控制端点 OUT 数据在等 */
static bool           s_wait_led_out;

static uint8_t  s_ep0_handled;   /* 这次 SETUP 已经被答复过了 */

static uint32_t s_setup_cnt;
static uint32_t s_report_cnt;
static uint32_t s_led_cnt;
static uint32_t s_stall_cnt;

#if (USBKBD_CDC_ON != 0)
/* CDC 数据收发计数，循环里累加，给 IDE 的观察窗口显示。 */
static uint32_t s_cdc_tx_cnt;
static uint32_t s_cdc_rx_cnt;

/* CDC SET_LINE_CODING 的 OUT 数据阶段在等 */
static bool     s_wait_linecoding;
#endif

/*==========================================================================
 * 小工具
 *========================================================================*/
static uint16_t usbk_min_u16(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

static void usbk_ep0_ack(void)
{
    UsbPort_Ep0Ack();
    s_ep0_handled = 1u;
}

static void usbk_ep0_stall(void)
{
    UsbPort_Ep0Stall();
    s_ep0_handled = 1u;
}

static void usbk_ep0_send_desc(const uint8_t *p, uint16_t len)
{
    s_pend_ptr    = p;
    s_pend_len    = len;
    s_pend_active = (len != 0u);
    s_ep0_handled = 0u;
}

/* 把游标处的下一包装进 EP0；没有数据阶段就发 0 长度收尾 */
static void usbk_kick_ep0(void)
{
    if (s_pend_active)
    {
        uint16_t n = (s_pend_len > (uint16_t)USBPORT_EP0_MP)
                     ? (uint16_t)USBPORT_EP0_MP : s_pend_len;

        UsbPort_Ep0Send(s_pend_ptr, n);

        s_pend_ptr += n;
        s_pend_len = (uint16_t)(s_pend_len - n);

        if (s_pend_len == 0u)
        {
            s_pend_active = false;
        }
    }
    else
    {
        UsbPort_Ep0Ack();
    }
}

/*==========================================================================
 * 标准请求处理
 *========================================================================*/

/*********************************************************************
 * @fn      usbk_desc_lookup
 * @brief   按 (描述类型, 索引) 找出发给主机的描述符
 * @note    USBKBD_CDC_ON = 1 时按复合设备枚举：
 *          复合设备的描述符由 usb_desc_cdc.c 提供，
 *          但 HID 报表描述符仍要取 usb_desc.c 那个 63 字节的段，
 *          这里只是把本复合设备的接口号和偏移对上，别的都不管。
 * @return  true = 找到了
 *********************************************************************/
static bool usbk_desc_lookup(uint8_t type, uint8_t index,
                             const uint8_t **pp, uint16_t *len)
{
#if (USBKBD_CDC_ON != 0)
    /* ---- 复合设备路径 ---- */
    if (type == (uint8_t)USBDESC_T_DEVICE)
    {
        /* ★ 复合设备必须用 bDeviceClass = 0xEF/0x02/0x01 的那一份，
         *   否则 Windows 不解析 IAD，CDC 的两个接口关联不起来。 */
        *pp  = UsbDescCdc_Device;
        *len = (uint16_t)USBDESCCDC_DEVICE_LEN;
        return true;
    }

    if (type == (uint8_t)USBDESC_T_CONFIG)
    {
        *pp  = UsbDescCdc_Config;
        *len = (uint16_t)USBDESCCDC_CONFIG_LEN;
        return true;
    }

    /* HID 描述符和报表描述符是「接口请求」，要指定是哪个接口的 HID，
     * index == USBDESCCDC_IF_HID(0) 就是键盘接口。
     * 这里把键盘接口放在 0 号，所以 index 是别的接口的 HID
     * 请求就直接 STALL。 */
    if (type == (uint8_t)USBDESC_T_HID_REPORT)
    {
        if (index != (uint8_t)USBDESCCDC_IF_HID)
        {
            return false;
        }
        *pp  = UsbDesc_Report;
        *len = (uint16_t)USBDESC_REPORT_LEN;
        return true;
    }

    if (type == (uint8_t)USBDESC_T_HID)
    {
        if (index != (uint8_t)USBDESCCDC_IF_HID)
        {
            return false;
        }
        /* 复合设备里的 HID 描述符是配置头里的子段，靠 UsbDescCdc_Offsets
         * 表里给出的偏移 找出这一小段，长度是固定的。 */
        *pp  = &UsbDescCdc_Config[UsbDescCdc_Offsets[USBDESCCDC_SEG_HID]];
        *len = 9u;
        return true;
    }

    if (type == (uint8_t)USBDESC_T_STRING)
    {
        if (index >= (uint8_t)USBDESC_STR_COUNT)
        {
            return false;
        }
        *pp  = UsbDesc_String[index];
        *len = (uint16_t)UsbDesc_String[index][0];
        return true;
    }

    return false;

#else   /* USBKBD_CDC_ON == 0，老路径 */

    return UsbDesc_Lookup(type, index, pp, len);

#endif
}

static void usbk_handle_get_descriptor(const usbk_setup_t *su)
{
    uint8_t        type  = (uint8_t)((su->wValue >> 8) & 0xFFu);
    uint8_t        index = (uint8_t)(su->wValue & 0xFFu);
    const uint8_t *p     = 0;
    uint16_t       len   = 0u;

    /* HID 描述符和报表描述符是"接口请求"：索引取 wIndex 低字节（接口号），
     * 不是 wValue 低字节。这是 USB 规范里一个很容易踩的坑。 */
    if ((type == (uint8_t)USBDESC_T_HID) || (type == (uint8_t)USBDESC_T_HID_REPORT))
    {
        index = (uint8_t)(su->wIndex & 0xFFu);
    }

    if (!usbk_desc_lookup(type, index, &p, &len))
    {
        usbk_ep0_stall();
        s_stall_cnt++;
        return;
    }

    /* 主机说只要前 wLength 个字节（枚举时经常先读 8 个字节探长度） */
    len = usbk_min_u16(len, su->wLength);

    usbk_ep0_send_desc(p, len);
}

static void usbk_handle_get_status(const usbk_setup_t *su)
{
    /* 设备/接口状态：2 字节，这里回"总线供电、不支持远程唤醒"*/
    static uint8_t st[2] = { 0x00u, 0x00u };

    if ((su->bmRequestType & (uint8_t)USBK_REQ_RECIP_MASK) == (uint8_t)USBK_REQ_RECIP_ENDP)
    {
        st[0] = 0x00u;                  /* 端点不处于 HALT */
        st[1] = 0x00u;
    }

    usbk_ep0_send_desc(st, usbk_min_u16(2u, su->wLength));
}

static void usbk_handle_get_config(const usbk_setup_t *su)
{
    static uint8_t cfg = 1u;

    cfg = (s_state == USBK_ST_CONFIGURED) ? 1u : 0u;
    usbk_ep0_send_desc(&cfg, usbk_min_u16(1u, su->wLength));
}

static void usbk_handle_get_interface(const usbk_setup_t *su)
{
    static uint8_t alt = 0u;

    usbk_ep0_send_desc(&alt, usbk_min_u16(1u, su->wLength));
}

static void usbk_handle_set_config(const usbk_setup_t *su)
{
    if ((su->wValue & 0xFFu) != 0u)
    {
        s_state = USBK_ST_CONFIGURED;
        UsbPort_SetConfigured(true);

        /* 【顺序】先把本次控制传输的状态阶段装好（EP0），再去武装 EP1。
         * USB 2.0 §9.4.7：状态阶段完成之前，设备不得使用接口/端点。
         * 原来先 UsbKbd_SendReportNow() 把 EP1 IN 武装起来、再 usbk_ep0_ack()，
         * 等于"端点已经能应答了，而这次 SET_CONFIGURATION 还没结束"。
         * 复合设备（usbccgp 要在 start 阶段拉起子设备）对时序更敏感，
         * 顺手把这一段原来的乱码注释也重写掉。 */
        usbk_ep0_ack();
        (void)UsbKbd_SendReportNow();
        return;
    }

    s_state = USBK_ST_ADDRESSED;        /* 配置 0 = 取消配置 */
    UsbPort_SetConfigured(false);
    usbk_ep0_ack();
}

static void usbk_handle_set_address(const usbk_setup_t *su)
{
    /* 地址要等这次控制传输的状态阶段之后才真正生效 —— 时序细节藏在
     * UsbPort_SetPendingAddress() 里。这里只登记。 */
    UsbPort_SetPendingAddress((uint8_t)(su->wValue & 0x7Fu));
    usbk_ep0_ack();

    if (s_state == USBK_ST_DEFAULT)
    {
        s_state = USBK_ST_ADDRESSED;
    }
}

static void usbk_handle_standard(const usbk_setup_t *su)
{
    switch (su->bRequest)
    {
        case USBK_REQ_GET_DESCRIPTOR:
            usbk_handle_get_descriptor(su);
            break;

        case USBK_REQ_SET_ADDRESS:
            usbk_handle_set_address(su);
            break;

        case USBK_REQ_SET_CONFIG:
            usbk_handle_set_config(su);
            break;

        case USBK_REQ_GET_CONFIG:
            usbk_handle_get_config(su);
            break;

        case USBK_REQ_GET_STATUS:
            usbk_handle_get_status(su);
            break;

        case USBK_REQ_GET_INTERFACE:
            usbk_handle_get_interface(su);
            break;

        case USBK_REQ_SET_INTERFACE:
            usbk_ep0_ack();               /* 只有 alt=0 一个设置，收到就算 OK */
            break;

        case USBK_REQ_CLEAR_FEATURE:
        case USBK_REQ_SET_FEATURE:
            /* 端点 HALT / 远程唤醒：一律 ACK 但什么都不做（键鼠不需要） */
            usbk_ep0_ack();
            break;

        case USBK_REQ_SET_DESCRIPTOR:
        default:
            usbk_ep0_stall();
            s_stall_cnt++;
            break;
    }
}

/*==========================================================================
 * HID 类请求
 *
 * 复合设备中，HID 接口只有"接口 0"。
 * 收到的 bmRequestType 和 wIndex 不是本接口 0，一律 STALL。
 *========================================================================*/
static void usbk_handle_class(const usbk_setup_t *su)
{
    /* 只处理发给接口 0 的请求；发给别的接口一律 STALL */
    if (((su->bmRequestType & (uint8_t)USBK_REQ_RECIP_MASK) == (uint8_t)USBK_REQ_RECIP_IFACE)
        && ((su->wIndex & 0xFFu) != 0u))
    {
        usbk_ep0_stall();
        s_stall_cnt++;
        return;
    }

    switch (su->bRequest)
    {
        case USBK_HID_SET_IDLE:
            /* wValue 高字节 = report id（boot 键盘没有 report id，忽略），
             * 低字节 = 空闲速率，单位 4ms，0 表示"只在变化时上报" */
            s_idle = (uint16_t)(su->wValue & 0xFFu);
            usbk_ep0_ack();
            break;

        case USBK_HID_GET_IDLE:
        {
            static uint8_t idle = 0u;

            idle = (uint8_t)(s_idle & 0xFFu);
            usbk_ep0_send_desc(&idle, usbk_min_u16(1u, su->wLength));
            break;
        }

        case USBK_HID_SET_PROTOCOL:
            /* wValue: 0 = Boot Protocol, 1 = Report Protocol */
            s_protocol = (uint8_t)(su->wValue & 0x01u);
            usbk_ep0_ack();
            break;

        case USBK_HID_GET_PROTOCOL:
        {
            static uint8_t proto = 0u;

            proto = s_protocol;
            usbk_ep0_send_desc(&proto, usbk_min_u16(1u, su->wLength));
            break;
        }

        case USBK_HID_GET_REPORT:
            /* 主机偶尔会主动来读当前输入报文；直接把当前状态给它 */
            usbk_ep0_send_desc(UsbKbd_BuildReport(),
                               usbk_min_u16((uint16_t)USBKBD_REPORT_LEN, su->wLength));
            break;

        case USBK_HID_SET_REPORT:
            /* LED 报文走控制端点的 OUT 数据阶段，等 EP0_OUT 事件来取 */
            s_wait_led_out = true;
            break;

        default:
            usbk_ep0_stall();
            s_stall_cnt++;
            break;
    }
}

/*==========================================================================
 * CDC 类请求
 *
 * CDC 1.2 的 6.2 规定了几种类请求的类型和参数含义：
 *
 *   SET_LINE_CODING      (0x20) 主机 -> 设备 7 字节参数
 *                               bmRequestType = 0x21
 *                               bRequest      = 0x20
 *                               wValue        = 0
 *                               wIndex        = 接口号，CDC 控制接口 1，
 *                               wLength       = 7
 *
 *   GET_LINE_CODING      (0x21) 设备 -> 主机 7 字节参数
 *                               bmRequestType = 0xA1
 *                               wLength       = 7
 *
 *   SET_CONTROL_LINE_STATE (0x22) 主机 -> 设备，wValue 是 DTR/RTS
 *                               bmRequestType = 0x21
 *                               wValue        = DTR | RTS<<1
 *                               wLength       = 0
 *
 *   这些请求都用 wIndex 指定 "CDC 控制接口"，即 USBDESCCDC_IF_CDC_CTRL(1)，
 *   它指向的是控制接口，不是键盘接口，所以不用管第 0 号；
 *   因为 usbk_on_setup() 已经按 bmRequestType 分好了 class 类型，
 *   这里只认 wIndex 就可以了。
 *========================================================================*/
#if (USBKBD_CDC_ON != 0)
static void usbk_handle_cdc(const usbk_setup_t *su)
{
    uint8_t iface = (uint8_t)(su->wIndex & 0xFFu);

    /* 两道前置检查， 任何一条不过就直接拒绝： 一是接收者 必须是接口，
     * 二是 bmRequestType 和 bRequest 都要是规定的那几个，不合规范就丢。
     * 如果 wIndex 不是本模块的 CDC 控制接口，那就不是给它，STALL。
     * 这样才能保证"请求只交给真正处理它的模块"这条原则。 */
    if ((su->bmRequestType & (uint8_t)USBK_REQ_RECIP_MASK)
        != (uint8_t)USBK_REQ_RECIP_IFACE)
    {
        usbk_ep0_stall();
        s_stall_cnt++;
        return;
    }

    if (iface != (uint8_t)USBDESCCDC_IF_CDC_CTRL)
    {
        usbk_ep0_stall();
        s_stall_cnt++;
        return;
    }

    switch (su->bRequest)
    {
        case USBDESCCDC_REQ_SET_LINE_CODING:
            /* 数据阶段是 OUT，等 EP0_OUT 事件来收，先把标志立起来 */
            /* 没数据阶段的 wLength 主机可能不按 USB 规范规定的 7 个，
             * 如果上来不是 7 个也照样能收到，那参考值也就跟着变了，
             * 这里只是记个预期长度，等 EP0_OUT 时再判断。 */
            s_pend_active = false;
            s_ep0_handled = 0u;
            s_wait_led_out = false;
            /* 这里交给 usbk_on_ep0_out() 去收数据，通过一个标志。 */
            s_wait_linecoding = true;
            break;

        case USBDESCCDC_REQ_GET_LINE_CODING:
        {
            uint8_t lc[7];

            (void)UsbCdc_GetLineCoding(lc);
            usbk_ep0_send_desc(lc, usbk_min_u16(7u, su->wLength));
            break;
        }

        case USBDESCCDC_REQ_SET_CONTROL_LINE_STATE:
            /* wValue 是 DTR/RTS，bit0 = DTR, bit1 = RTS，
             * 主机惯用的办法：DTR = 1 就表示"串口已打开" */
            UsbCdc_SetLineState((uint16_t)(su->wValue & 0x0003u));
            usbk_ep0_ack();
            break;

        default:
            usbk_ep0_stall();
            s_stall_cnt++;
            break;
    }
}
#endif  /* USBKBD_CDC_ON */

/*==========================================================================
 * LED 报文
 *========================================================================*/
static void usbk_apply_led(const uint8_t *p, uint16_t len)
{
    if ((p == 0) || (len == 0u))
    {
        return;
    }

    (void)UsbKbd_SetLedFromHost(p[0]);
    s_led_cnt++;
}

/*==========================================================================
 * EP0 事件
 *========================================================================*/
static void usbk_on_setup(const usbk_setup_t *su)
{
    uint8_t type;
    uint8_t iface;

    s_setup_cnt++;

    /* 每个 SETUP 都意味着上一笔控制传输结束了 */
    s_pend_active  = false;
    s_pend_len     = 0u;
    s_wait_led_out = false;
#if (USBKBD_CDC_ON != 0)
    s_wait_linecoding = false;
#endif

    s_ep0_handled = 0u;

    type  = (uint8_t)(su->bmRequestType & (uint8_t)USBK_REQ_TYP_MASK);
    iface = (uint8_t)(su->wIndex & 0xFFu);

    if (type == (uint8_t)USBK_REQ_TYP_STANDARD)
    {
        usbk_handle_standard(su);
    }
    else if (type == (uint8_t)USBK_REQ_TYP_CLASS)
    {
#if (USBKBD_CDC_ON != 0)
        /* 复合设备的类请求按下面分派，都是 class 类型，
         *   接口 1CDC 控制， -> 归 CDC 分支
         *    接口 0HID， -> 归 HID 分支
         *
         * 判据取 wIndex 低字节接口号，后面的分支只认 wIndex， 因为设备里还有别的
         * bInterfaceNumber 在别的设备上为 0，  HID 设备也是这样，
         * 是别的设备时其值为 0，光看接口号完全分不清，所以不靠它。
         * 所以干脆用 CDC 控制接口当 handler 的分界，"谁需要它"谁就去处理，
         * 所以干脆用 iface == USBDESCCDC_IF_CDC_CTRL 来判断就行。 */
        if (iface == (uint8_t)USBDESCCDC_IF_CDC_CTRL)
        {
            usbk_handle_cdc(su);
        }
        else
        {
            usbk_handle_class(su);
        }
#else
        usbk_handle_class(su);
#endif
    }
    else
    {
        usbk_ep0_stall();
        s_stall_cnt++;
    }

    /* 描述符挂好了就立刻发第一包；handler 自己 ACK/STALL 过就算了 */
    if (s_ep0_handled == 0u)
    {
        usbk_kick_ep0();
    }
}

static void usbk_on_ep0_in(void)
{
    usbk_kick_ep0();
}

static void usbk_on_ep0_out(void)
{
    uint8_t buf[USBPORT_EP0_MP];
    uint16_t n;

    n = UsbPort_Ep0Recv(buf, (uint16_t)sizeof(buf));

    if (s_wait_led_out)
    {
        usbk_apply_led(buf, n);
        s_wait_led_out = false;
    }

#if (USBKBD_CDC_ON != 0)
    if (s_wait_linecoding)
    {
        /* SET_LINE_CODING 的数据阶段会给 7 个字节，
         * 这里只判长度够不够，内容不校验，直接交给数据层去存。
         * 长度不够就丢掉这一包，不要在本地乱改数据。 */
        if (n >= 7u)
        {
            (void)UsbCdc_SetLineCoding(buf);
        }
        s_wait_linecoding = false;
    }
#endif

    usbk_ep0_ack();
}

/*==========================================================================
 * 【中断侧入口】见 usb_kbd.h 第 5 节的说明 —— EP0 在 USB 中断里一次做完
 *========================================================================*/
void UsbKbd_OnEp0Setup(const usbk_setup_t *su) { usbk_on_setup(su); }
void UsbKbd_OnEp0In(void)                      { usbk_on_ep0_in(); }
void UsbKbd_OnEp0Out(void)                     { usbk_on_ep0_out(); }

static void usbk_on_ep1_out(void)
{
    uint8_t buf[USBPORT_EP1_MP];
    uint16_t n;

    n = UsbPort_Ep1Recv(buf, (uint16_t)sizeof(buf));
    usbk_apply_led(buf, n);
}

#if (USBKBD_CDC_ON != 0)
/*==========================================================================
 * CDC 端点事件（EP2 OUT / EP3 IN / EP4 IN）
 *========================================================================*/

/* EP2 OUT：主机下发的数据包，收到就交给数据层 */
static void usbk_on_ep2_out(void)
{
    uint8_t  buf[USBPORT_EP2_MP];
    uint16_t n;

    n = UsbPort_Ep2Recv(buf, (uint16_t)sizeof(buf));
    if (n > 0u)
    {
        UsbCdc_OnDataOut(buf, n);
        s_cdc_rx_cnt++;
    }
}

/* EP3 IN：上一包已被取走，查询 EP3 有空就再发一包 */
static void usbk_on_ep3_in(void)
{
    if (!UsbPort_Ep3Ready())
    {
        return;
    }

    /* 向数据层要一包就发，没有数据就等下一次事件，
     * 每次只发一包，64 字节封顶，不在这里重发，等下一次事件再来，
     * 免得占着中断或者主循环太久。 */
    {
        uint8_t  out[USBPORT_EP3_MP];
        uint16_t m;

        m = UsbCdc_PollTx(out, (uint16_t)sizeof(out));
        if (m > 0u)
        {
            (void)UsbPort_Ep3Send(out, m);
            s_cdc_tx_cnt++;
        }
    }
}

/* EP4 IN：CDC 通知口，目前本文件不管它，由数据层自己去组织。 */
static void usbk_on_ep4_in(void)
{
    /* 这里空着：通知的发送交给 "要发的人" 自己调，本文件不插手 */
}

/* 查询数据层有没有待发的， EP3 又空着就取一包发出去。
 * 每次主循环都做一次，保证上行数据不会被压住。 */
static void usbk_cdc_pump(void)
{
    if (!UsbPort_Ep3Ready())
    {
        return;
    }
    if (UsbCdc_TxPending() == 0u)
    {
        return;
    }

    {
        uint8_t  out[USBPORT_EP3_MP];
        uint16_t m;

        m = UsbCdc_PollTx(out, (uint16_t)sizeof(out));
        if (m > 0u)
        {
            (void)UsbPort_Ep3Send(out, m);
            s_cdc_tx_cnt++;
        }
    }
}
#endif  /* USBKBD_CDC_ON */

/*==========================================================================
 * 初始化
 *========================================================================*/
void UsbKbd_DeviceInit(void)
{
    UsbKbd_Init();                      /* 报文层复位 */

    s_state        = USBK_ST_DISCONNECTED;
    s_protocol     = USBK_PROTO_BOOT;   /* boot 键盘默认就给 BIOS 用 */
    s_idle         = 0u;
    s_pend_ptr     = 0;
    s_pend_len     = 0u;
    s_pend_active  = false;
    s_wait_led_out = false;

    s_setup_cnt  = 0u;
    s_report_cnt = 0u;
    s_led_cnt    = 0u;
    s_stall_cnt  = 0u;

#if (USBKBD_CDC_ON != 0)
    s_wait_linecoding = false;
    s_cdc_tx_cnt = 0u;
    s_cdc_rx_cnt = 0u;

    /* CDC 数据层复位，把收发缓冲和计数清干净，
     * 要在 USB_DeviceInit() 之前调用，免得枚举时还带着上一次的残留数据。 */
    UsbCdc_Init();

    /* 复合设备描述符做一次自检：长度、接口号、端点地址都要对得上。 */
    (void)UsbDescCdc_IsValid();
#endif

    UsbPort_Init();

    s_state = USBK_ST_DEFAULT;          /* 等主机来枚举 */
}

void UsbKbd_DeviceShutdown(void)
{
    UsbKbd_ReleaseAll();                /* 别让主机以为还有键按着 */
    UsbPort_Shutdown();
    s_state = USBK_ST_DISCONNECTED;
}

/*==========================================================================
 * 上报
 *========================================================================*/
bool UsbKbd_SendReportNow(void)
{
    if (s_state != USBK_ST_CONFIGURED)
    {
        return false;
    }
    if (!UsbPort_Ep1Ready())
    {
        return false;                   /* 上一包还没被主机取走 */
    }

    if (!UsbPort_Ep1Send(UsbKbd_BuildReport(), (uint8_t)USBKBD_REPORT_LEN))
    {
        return false;
    }

    UsbKbd_ClearDirty();                /* 已经交给硬件了，不再算"待发" */
    s_report_cnt++;

    return true;
}

/*==========================================================================
 * 主循环
 *========================================================================*/
void UsbKbd_DeviceTask(void)
{
    usbk_setup_t su;
    usbk_evt_t   ev;
    uint8_t      guard = 0u;

    /* 一次最多处理 16 个事件，避免中断风暴把主循环卡死 */
    while ((guard < 16u) && ((ev = UsbPort_PollEvent(&su)) != USBK_EV_NONE))
    {
        guard++;

        switch (ev)
        {
            case USBK_EV_RESET:
                /* 总线复位：地址回 0，所有端点清零，回到 DEFAULT */
                s_state        = USBK_ST_DEFAULT;
                s_pend_active  = false;
                s_pend_len     = 0u;
                s_wait_led_out = false;
                UsbKbd_ReleaseAll();
                break;

            /* 【注意】下面 USBK_EV_SETUP / EP0_IN / EP0_OUT 三个分支已经**不会再被触发**：
             * EP0 的装包与收尾全部搬进了 USB 中断（见 usb_kbd.h 第 5 节、
             * UsbKbd_OnEp0Setup/OnEp0In/OnEp0Out），ISR 不再 push 这三个事件。
             * 留着是为了不改动 switch 的完整性，逻辑本身仍然是对的
             * （万一以后 USB_EP0_IN_ISR 设回 0，这三个分支就能重新生效）。 */
            case USBK_EV_SETUP:
                usbk_on_setup(&su);
                break;

            case USBK_EV_EP0_IN:
                usbk_on_ep0_in();
                break;

            case USBK_EV_EP0_OUT:
                usbk_on_ep0_out();
                break;

            case USBK_EV_EP0_STATUS:
                /* 纯状态阶段结束（没有 OUT 数据），什么都不用做 ——
                 * 上一笔控制传输已经由发数据/ACK 那一侧收尾了 */
                s_wait_led_out = false;
#if (USBKBD_CDC_ON != 0)
                s_wait_linecoding = false;
#endif
                break;

            case USBK_EV_EP1_OUT:
                usbk_on_ep1_out();
                break;

            case USBK_EV_EP1_IN:
                /* 上一包被取走了；下面统一决定要不要发新的一包 */
                break;

#if (USBKBD_CDC_ON != 0)
            case USBK_EV_EP2_OUT:
                usbk_on_ep2_out();
                break;

            case USBK_EV_EP3_IN:
                usbk_on_ep3_in();
                break;

            case USBK_EV_EP4_IN:
                usbk_on_ep4_in();
                break;
#endif

            case USBK_EV_SUSPEND:
            case USBK_EV_RESUME:
            default:
                break;
        }
    }

    /* 有变化就补一包 */
    if ((s_state == USBK_ST_CONFIGURED) && UsbKbd_IsDirty() && UsbPort_Ep1Ready())
    {
        if (UsbPort_Ep1Send(UsbKbd_BuildReport(), (uint8_t)USBKBD_REPORT_LEN))
        {
            UsbKbd_ClearDirty();
            s_report_cnt++;
        }
    }

#if (USBKBD_CDC_ON != 0)
    /* CDC 流水泵
     * EP3 IN 有空而且数据层还有待发的，就往里塞一包的数据，
     * 每次主循环判断一次，保证上行数据不会堵在里头。 */
    if (s_state == USBK_ST_CONFIGURED)
    {
        usbk_cdc_pump();
    }
#endif
}

/*==========================================================================
 * 状态查询
 *========================================================================*/
bool UsbKbd_IsConnected(void)
{
    return (s_state == USBK_ST_CONFIGURED);
}

bool UsbKbd_IsPlugged(void)
{
    return (s_state != USBK_ST_DISCONNECTED);
}

UsbKbdState_t UsbKbd_GetState(void)
{
    return s_state;
}

uint8_t UsbKbd_GetProtocol(void)
{
    return s_protocol;
}

uint16_t UsbKbd_GetIdle(void)
{
    return s_idle;
}

uint32_t UsbKbd_GetReportCount(void) { return s_report_cnt; }
uint32_t UsbKbd_GetLedCount(void)    { return s_led_cnt;    }
uint32_t UsbKbd_GetStallCount(void)  { return s_stall_cnt;  }
uint32_t UsbKbd_GetSetupCount(void)  { return s_setup_cnt;  }

/* 按键入口就是报文层的 UsbKbd_KeyEvent()（见 usb_kbd_report.h），
 * 这里不再包一层 —— usb_kbd.h 已经 include 了那个头，上层直接调即可。 */

/******************* (C) CH592X USB HID 键盘 *****************/
