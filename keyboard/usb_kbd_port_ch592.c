/**************************************************************************
 * usb_kbd_port_ch592.c —— usb_kbd_port.h 的 CH592 真机后端
 *
 * 只做两件事：
 *   1) 把 CH59x 的 USB 寄存器操作包成 usb_kbd_port.h 那几个函数
 *   2) USB 中断里把事件塞进环形队列（中断里不干活）
 *
 * 结构参照 WCH 官方例程
 *   CH592EVT\EVT\EXAM\USB\Device\HID_CompliantDev\src\Main.c
 * 的 USB_DevTransProcess()，但把"在中断里查 SetupReqCode 决定发什么"
 * 改成了"中断只报事件、主循环再决定" —— 这样上层逻辑在 PC 上能原样跑。
 *
 * ------------------------------------------------------------------
 * 【EP0 IN 的时序：本后端和例程不一样的地方，务必看懂】
 *
 * 例程是在 **IN 令牌中断里** 才 memcpy 描述符进 pEP0_DataBuf 的。
 * 那种写法要求"决定发什么"的逻辑必须待在中断里，没法做可移植分层。
 *
 * 本后端改成**预装**：
 *     SETUP 事件 → 主循环把第 1 包装进 EP0 缓冲并"解锁"(T_RES=ACK)
 *     → 主机来 IN，包发出去 → 中断报 USBK_EV_EP0_IN
 *     → 主循环装第 2 包并解锁 → ……
 * 中间主机如果抢先发了 IN，会收到 NAK（USB 允许，主机会自己重试），
 * 所以功能上完全等价，只是多几次重试。
 *
 * EP0 的 DATA0/DATA1 要自己翻（USB_DeviceInit 里没给 EP0 开 RB_UEP_AUTO_TOG），
 * 翻转点在 **IN 完成中断**（UIS_TOKEN_IN），不在解锁时：控制传输数据阶段的
 * 第一包必须是 DATA1（SETUP 分支写好的），解锁时再翻就成了 DATA0，枚举必失败。
 * ------------------------------------------------------------------
 *
 * 【编译前提】
 *   StdPeriphDriver\CH59x_usbdev.c 必须参与编译（它提供 USB_DeviceInit()、
 *   DevEP1_IN_Deal() 以及 pEP0/pEP1/pEP2/pEP3_RAM_Addr 的定义）。
 *   工程里这一条默认被 .cproject 排除了，见 README 第三章。
 **************************************************************************/

#include "usb_kbd_port.h"
#include "usb_kbd.h"       /* 中断侧 EP0 入口 */

/*==========================================================================
 * USB_EP0_IN_ISR —— EP0 在哪儿处理（排查用的 A/B 开关）
 *   1 = 【默认】EP0 的装包/收尾全在 USB 中断里做完（对齐 WCH 官方例程）。
 *       中断返回前 EP0 已经武装好，枚举不受主循环快慢影响。
 *   0 = 旧行为：中断只抛事件，主循环 UsbKbd_DeviceTask() 再装包。
 *       UI_Task() 刷屏 / Game_Task() 的 DelayMs 一占住主循环，EP0 就没人喂，
 *       主机超时重发 SETUP 时硬件 DMA 会覆盖正在发的数据 -> 枚举必挂。
 *   【1 是修好的那条路，不要改回 0】
 *========================================================================*/
#ifndef USB_EP0_IN_ISR
#define USB_EP0_IN_ISR   1
#endif
#include "CH59x_common.h"
#include "CH59x_usbdev.h"
#include <string.h>      /* memcpy */

/*==========================================================================
 * 端点 RAM
 *
 * WCH 要求这几个缓冲 __attribute__((aligned(4)))，而且 DMA 地址要从
 * pEPx_RAM_Addr 交给 R16_UEPx_DMA，所以必须是**静态**数组，不能是栈上的。
 *
 * ------------------------------------------------------------------
 * 这里要说明 的 EP2/EP3 正式端点缓冲
 *
 * 原因是：
 *   s_ep0_buf = EP0(64) + EP4OUT(64) + EP4IN(64)   = 192
 *   s_ep1_buf = EP1OUT(64) + EP1IN(64)             = 128
 *   s_ep23_buf= 64（占位，EP2/EP3 未启用）            = 64
 *
 * USB_DeviceInit() 里面配的端点缓冲是：
 *   R8_UEP2_3_MOD = RB_UEP2_RX_EN|RB_UEP2_TX_EN|RB_UEP3_RX_EN|RB_UEP3_TX_EN
 *   R16_UEP2_DMA  = pEP2_RAM_Addr
 *   R16_UEP3_DMA  = pEP3_RAM_Addr
 *
 * 当 2/3 在 "OUT+IN" 模式时，每个至少需要 128 字节，前 64 是 OUT，
 * 后 64 是 IN（见 CH59x_usbdev.h 里 pEP2_OUT_DataBuf / pEP2_IN_DataBuf 的定义即知）
 *
 * 所以这里原来 s_ep23_buf 只有 64 个字节， EP2/EP3 同时使用会出越界
 * 所以用两个独立的 s_ep2_buf / s_ep3_buf 分开成 128 字节。
 * 总共：64*3+64*2+64*2+64*2 = 192+128+128+128 = 576 字节，RAM 能承受。
 *
 * 这"EP2/EP3/EP4" 的分配依照 USB 2.2.11 里要求的：EP2 OUT 数据，
 * EP3 IN 数据，EP4 IN 通知——因为这是 Windows 识别复合设备时的依据
 *========================================================================*/
__attribute__((aligned(4))) static uint8_t s_ep0_buf[64 + 64 + 64];  /* EP0(64)+EP4OUT(64)+EP4IN(64) */
__attribute__((aligned(4))) static uint8_t s_ep1_buf[64 + 64];        /* EP1OUT(64)+EP1IN(64)         */
__attribute__((aligned(4))) static uint8_t s_ep2_buf[64 + 64];        /* EP2OUT(64)+EP2IN(64)         */
__attribute__((aligned(4))) static uint8_t s_ep3_buf[64 + 64];        /* EP3OUT(64)+EP3IN(64)         */

/*==========================================================================
 * 事件队列（中断 -> 主循环）
 *========================================================================*/
#define USBPORT_Q_LEN       16u

static volatile uint8_t s_q[USBPORT_Q_LEN];
static volatile uint8_t s_q_head;
static volatile uint8_t s_q_tail;

static usbk_setup_t        s_setup;         /* 最近一次 SETUP 包（主循环取走） */
static volatile uint8_t    s_setup_ready;

static volatile uint8_t    s_configured;
static volatile uint8_t    s_suspended;

static volatile uint8_t    s_pend_addr;
static volatile uint8_t    s_pend_addr_valid;

static volatile uint8_t    s_ep0_armed;     /* EP0 IN 已解锁（T_RES=ACK） */
static volatile uint16_t   s_ep0_rxlen;     /* EP0 OUT 收到的字节数 */
static volatile uint8_t    s_ep1_rxlen;     /* EP1 OUT 收到的字节数 */
static volatile uint8_t    s_ep1_in_flight; /* EP1 IN 里的包还没被取走 */

/* ---- CDC 端点状态（EP2 OUT / EP3 IN / EP4 IN） ---- */
static volatile uint8_t    s_ep2_rxlen;     /* EP2 OUT 收到的字节数 */
static volatile uint8_t    s_ep3_in_flight; /* EP3 IN 里的数据仍没被取走 */
static volatile uint8_t    s_ep4_in_flight; /* EP4 IN 通知里的没被取走 */

static uint8_t             s_address;

/*==========================================================================
 * 队列
 *========================================================================*/
static void usbport_q_push(uint8_t ev)
{
    uint8_t next = (uint8_t)((s_q_tail + 1u) % (uint8_t)USBPORT_Q_LEN);

    if (next == s_q_head)
    {
        return;                             /* 满了：丢新事件（事件都是可重入的信号） */
    }

    s_q[s_q_tail] = ev;
    s_q_tail = next;
}

static uint8_t usbport_q_pop(void)
{
    uint8_t ev;

    if (s_q_head == s_q_tail)
    {
        return (uint8_t)USBK_EV_NONE;
    }

    ev = s_q[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1u) % (uint8_t)USBPORT_Q_LEN);
    return ev;
}

/*==========================================================================
 * EP0 解锁一包
 *========================================================================*/
static void usbport_ep0_arm(void)
{
    /* 本函数**只动发送方向**的 T_RES，不动 T_TOG：
     *   · 数据/状态阶段第一包的 DATA1 是 SETUP 分支里一次写好的
     *     （R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | ...）；
     *   · 后面的 DATA0/DATA1 翻转在 IN 完成中断里做（UIS_TOKEN_IN）；
     * 这里再碰 T_TOG 会把第一包变成 DATA0，主机会按 PID 不匹配丢掉，
     * 然后设备就"发了包但主机不认"，枚举直接失败。 */
    R8_UEP0_CTRL = (uint8_t)((R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK);
    s_ep0_armed = 1u;
}

/* EP0 回 STALL */
static void usbport_ep0_stall_now(void)
{
    R8_UEP0_CTRL = (uint8_t)((R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_STALL);
    s_ep0_armed = 0u;
}

/*==========================================================================
 * 中断服务：只报事件
 *========================================================================*/
void USB_DevTransProcess(void)
{
    uint8_t intflag = R8_USB_INT_FG;
    uint8_t token;

    if ((intflag & RB_UIF_TRANSFER) != 0u)
    {
        /* ================================================================
         * 【修复 USB 枚举 Code 43 的根因】SETUP 只能靠 RB_UIS_SETUP_ACT 判定
         *
         * CH592 上"这一包是不是 SETUP"**只能**看 R8_USB_INT_ST 的最高位
         * RB_UIS_SETUP_ACT，**不能**看 MASK_UIS_TOKEN(bits[5:4])。
         * 数据手册（CH592/CH591 Datasheet，R8_USB_INT_ST 寄存器）原文：
         *   "SETUP token does not affect RB_UIS_TOG_OK, MASK_UIS_TOKEN,
         *    MASK_UIS_ENDP and R8_USB_RX_LEN."
         * 而 MASK_UIS_TOKEN 的复位值就是 11b —— 11b 是 **idle**，不是 SETUP。
         * 旧头文件里那个 UIS_TOKEN_SETUP = 0x30 是 CH57x/CH58x 时代的遗留宏，
         * 在 CH592 上 0x30 的含义已经变成 "idle"。
         *
         * 换句话说：SETUP 期间 token 字段**保持上一包的值不变**。
         * 所以原来的 `case UIS_TOKEN_SETUP:` 只有在上一次事务把该字段留成
         * idle 时才碰巧命中；只要上一包是 IN/OUT，这个 SETUP 就会被当成
         * "端点 0 的 IN/OUT" 处理、整包请求被丢掉 —— 枚举中途变哑巴：
         *   · 上次留 0b10(IN)  -> 走 case UIS_TOKEN_IN：s_ep0_armed==0，
         *                        静默丢弃 -> 主机报 "设备描述符请求失败"；
         *   · 上次留 0b00(OUT) -> 走 case UIS_TOKEN_OUT：读一个无关的
         *                        R8_USB_RX_LEN，还回一个空状态包 -> 请求丢弃，
         *                        常常正好卡在 SET_ADDRESS -> "设置地址失败"。
         * 现场先后报的两个不同失败点，就是同一条 bug 的两种落点。
         *
         * WCH 官方 7 个 CH592 例程（HID_CompliantDev / COM / CompoundDev /
         * VendorDefinedDev / USB_IAP / BLE_USB ...）**全部**用
         * `if (R8_USB_INT_ST & RB_UIS_SETUP_ACT)` 判定 SETUP，没有一个用
         * `case UIS_TOKEN_SETUP:` 做分发；COM 例程里那句 "从之前的代码改过
         * -- UIS_TOKEN_SETUP" 就是这个替换动作留下的注释。
         *
         * 手册另外要求：token 字段非 idle 且 SETUP_ACT=1 时，要先按
         * token/端点处理、再处理 SETUP。本工程把事件抛给主循环去处理，
         * 不能对 SETUP 再补一个假的 EP0 OUT/IN 事件，所以这里写成 if/else：
         * SETUP 优先，绝不与 token 分支混在一起。
         * ================================================================ */
        if ((R8_USB_INT_ST & RB_UIS_SETUP_ACT) != 0u)
        {
            /* SETUP 包已落在 EP0 RAM 里，直接搬出来（SETUP 一定发到端点 0） */
            s_setup.bmRequestType = pSetupReqPak->bRequestType;
            s_setup.bRequest      = pSetupReqPak->bRequest;
            s_setup.wValue        = pSetupReqPak->wValue;
            s_setup.wIndex        = pSetupReqPak->wIndex;
            s_setup.wLength       = pSetupReqPak->wLength;

            /* R_TOG/T_TOG 都置 1：数据阶段的第一包一定是 DATA1（USB 2.0 §8.6）。
             * 【修复】紧接着就在本中断里调 UsbKbd_OnEp0Setup() 把数据装好、
             * 由它把 T_RES 改成 ACK —— 也就是"中断返回前 EP0 已经武装好"，
             * 和 WCH 官方例程一致。以前是抛事件给主循环去装，主循环一忙
             * （UI_Task 刷屏 / Game_Task 的 DelayMs）主机就收 NAK，超时重发
             * SETUP 时硬件 DMA 会覆盖掉正在发的数据 —— 这就是枚举挂掉的根因。 */
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG
                         | UEP_R_RES_ACK | UEP_T_RES_NAK;
            s_ep0_armed   = 0u;
            s_setup_ready = 1u;
#if (USB_EP0_IN_ISR)
            UsbKbd_OnEp0Setup(&s_setup);   /* EP0 在中断里完成 */
#else
            usbport_q_push((uint8_t)USBK_EV_SETUP);
#endif
        }
        else
        {
            token = (uint8_t)(R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP));

            switch (token)
            {
                /* ---------------- 端点 0：IN/OUT（SETUP 已在上面分流） -------- */
                case UIS_TOKEN_IN:
                    if (s_ep0_armed != 0u)
                    {
                        /* 这一包（或者状态包）已经发出去了：翻转 DATA0/DATA1，
                         * 为下一包准备。必须在**发完之后**翻 —— 这样第一包
                         * （SETUP 分支给的 DATA1）不受影响，第二包正好是 DATA0。
                         * 和 WCH 例程的翻转点一致。 */
                        R8_UEP0_CTRL = (uint8_t)(R8_UEP0_CTRL ^ RB_UEP_T_TOG);
                        s_ep0_armed = 0u;
                        R8_UEP0_CTRL = (uint8_t)((R8_UEP0_CTRL & ~MASK_UEP_T_RES)
                                                 | UEP_T_RES_NAK);

                        /* 【修复 SET_ADDRESS】走到这里 = 刚发的那个 IN（数据或状态阶段的
                         * 0 长度包）已被主机收下，本次 SETUP 事务结束。SET_ADDRESS 没有
                         * 数据阶段，所以这就是它状态阶段完成的那一刻 —— 此时才换地址。 */
                        if (s_pend_addr_valid != 0u)
                        {
                            s_address = s_pend_addr;
                            R8_USB_DEV_AD = (uint8_t)(s_pend_addr & 0x7Fu);
                            s_pend_addr_valid = 0u;
                        }

#if (USB_EP0_IN_ISR)
                        UsbKbd_OnEp0In();
#else
                        usbport_q_push((uint8_t)USBK_EV_EP0_IN);
#endif
                    }
                    else
                    {
                        /* 还没准备好数据：硬件回 NAK，主机会自动重试。
                         * 这里不报事件，免得把队列刷爆。 */
                    }
                    break;

                case UIS_TOKEN_OUT:
                    s_ep0_rxlen = (uint16_t)R8_USB_RX_LEN;
                    R8_UEP0_CTRL = (uint8_t)((R8_UEP0_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK);
#if (USB_EP0_IN_ISR)
                    UsbKbd_OnEp0Out();
#else
                    usbport_q_push((uint8_t)USBK_EV_EP0_OUT);
#endif
                    break;

                /* ---------------- 端点 1 ---------------- */
                case (UIS_TOKEN_IN | 1):
                    if (s_ep1_in_flight != 0u)
                    {
                        /* 和 EP0 一样：EP1 的 DATA0/DATA1 也要软件翻。
                         * USB_DeviceInit() 给 EP1 开了 RB_UEP_AUTO_TOG，但每次总线复位
                         * （插上电脑就会收到）都会把它清掉，之后就完全靠软件翻 ——
                         * 官方例程的 IN 分支就是在这里 ^= RB_UEP_T_TOG 的。
                         * 不翻的话每包都是 DATA0，主机按 PID 不匹配丢掉，
                         * 表现就是：键盘认得出、但打不出字。 */
                        R8_UEP1_CTRL = (uint8_t)(R8_UEP1_CTRL ^ RB_UEP_T_TOG);
                        s_ep1_in_flight = 0u;
                        R8_UEP1_CTRL = (uint8_t)((R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                        usbport_q_push((uint8_t)USBK_EV_EP1_IN);
                    }
                    break;

                case (UIS_TOKEN_OUT | 1):
                    if ((R8_USB_INT_ST & RB_UIS_TOG_OK) != 0u)
                    {
                        R8_UEP1_CTRL ^= RB_UEP_R_TOG;
                        s_ep1_rxlen = (uint8_t)R8_USB_RX_LEN;
                        usbport_q_push((uint8_t)USBK_EV_EP1_OUT);
                    }
                    break;

                /* ---------------- 端点 2/3/4，CDC 用 ---- */
                case (UIS_TOKEN_OUT | 2):
                    /* EP2 OUT，收到数据包记下长度并抛事件。
                     *
                     * 【2026-10-19 修复】这里**不能再手动 XOR R_TOG**：
                     * 总线复位分支给 EP2 置了 RB_UEP_AUTO_TOG，硬件会在每次
                     * 成功收包后自动翻转 DATA0/DATA1。再加一次软件翻转等于
                     * 每包翻两次，下一包的主机 toggle 就对不上，设备会 ACK
                     * 但把数据丢掉。
                     * 现场表现正是：网页能连上、第一条命令有应答，之后所有
                     * 读/写命令全部超时，看起来就是"网页改不了键值"。
                     * EP1 在总线复位分支里没有开 AUTO_TOG，所以 EP1 仍由
                     * 下面的软件翻转逻辑处理；这里只改 EP2/EP3。 */
                    if ((R8_USB_INT_ST & RB_UIS_TOG_OK) != 0u)
                    {
                        s_ep2_rxlen = (uint8_t)R8_USB_RX_LEN;
                        usbport_q_push((uint8_t)USBK_EV_EP2_OUT);
                    }
                    break;

                case (UIS_TOKEN_IN | 3):
                    /* EP3 IN，数据包被主机取走后清标记。
                     * 【2026-10-19 修复】同理，EP3 已启用 RB_UEP_AUTO_TOG，
                     * T_TOG 由硬件自动翻；这里手动 XOR 会把硬件翻好的值又
                     * 翻回去，导致第二条 CDC 应答开始主机一直收到错误的
                     * DATA0/DATA1，网页握手后的所有命令都超时。 */
                    if (s_ep3_in_flight != 0u)
                    {
                        s_ep3_in_flight = 0u;
                        R8_UEP3_CTRL = (uint8_t)((R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                        usbport_q_push((uint8_t)USBK_EV_EP3_IN);
                    }
                    break;

                case (UIS_TOKEN_IN | 4):
                    /* EP4 IN，CDC 通知发出去了 */
                    if (s_ep4_in_flight != 0u)
                    {
                        R8_UEP4_CTRL = (uint8_t)(R8_UEP4_CTRL ^ RB_UEP_T_TOG);
                        s_ep4_in_flight = 0u;
                        R8_UEP4_CTRL = (uint8_t)((R8_UEP4_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK);
                        usbport_q_push((uint8_t)USBK_EV_EP4_IN);
                    }
                    break;

                case (UIS_TOKEN_OUT | 3):
                case (UIS_TOKEN_OUT | 4):
                    /* EP3 OUT / EP4 OUT 这两个方向不需要接收，直接
                     * 在 USB_DeviceInit 里没给它们使能 OUT ，中断不会来
                     * 这里什么都不用做。 */
                    break;

                default:
                    break;
            }
        }

        R8_USB_INT_FG = RB_UIF_TRANSFER;    /* 写 1 清标志 */
    }

    if ((intflag & RB_UIF_BUS_RST) != 0u)
    {
        /* 复位后所有端点回到初始握手状态（和 WCH 例程一致） */
        R8_USB_DEV_AD = 0x00;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_UEP4_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        s_address      = 0u;
        s_configured   = 0u;
        s_ep1_in_flight = 0u;
        s_ep2_rxlen    = 0u;
        s_ep3_in_flight = 0u;
        s_ep4_in_flight = 0u;
        s_ep0_armed    = 0u;
        usbport_q_push((uint8_t)USBK_EV_RESET);
        R8_USB_INT_FG = RB_UIF_BUS_RST;
    }

    if ((intflag & RB_UIF_SUSPEND) != 0u)
    {
        if ((R8_USB_MIS_ST & RB_UMS_SUSPEND) != 0u)
        {
            s_suspended = 1u;
            usbport_q_push((uint8_t)USBK_EV_SUSPEND);
        }
        else
        {
            s_suspended = 0u;
            usbport_q_push((uint8_t)USBK_EV_RESUME);
        }
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
}

__attribute__((interrupt("WCH-Interrupt-fast")))
__attribute__((section(".highcode")))

void USB_IRQHandler(void)
{
    USB_DevTransProcess();
}

/*==========================================================================
 * usb_kbd_port.h 的实现
 *========================================================================*/
void UsbPort_Init(void)
{
    /* 1) 把端点缓冲地址交给驱动 */
    pEP0_RAM_Addr = s_ep0_buf;
    pEP1_RAM_Addr = s_ep1_buf;
    pEP2_RAM_Addr = s_ep2_buf;              /* CDC 数据包接收缓冲 */
    pEP3_RAM_Addr = s_ep3_buf;              /* CDC 数据包发送缓冲 区 */

    /* 2) 队列和状态清零 */
    s_q_head = 0u;
    s_q_tail = 0u;
    s_setup_ready = 0u;
    s_configured  = 0u;
    s_suspended   = 0u;
    s_pend_addr_valid = 0u;
    s_ep0_armed   = 0u;
    s_ep0_rxlen   = 0u;
    s_ep1_rxlen   = 0u;
    s_ep1_in_flight = 0u;
    s_ep2_rxlen   = 0u;
    s_ep3_in_flight = 0u;
    s_ep4_in_flight = 0u;
    s_address     = 0u;

    /* 3) 初始化 USB 外设并开中断 */
    USB_DeviceInit();
    PFIC_EnableIRQ(USB_IRQn);
}

void UsbPort_Shutdown(void)
{
    PFIC_DisableIRQ(USB_IRQn);
    USB_Disable();                          /* R32_USB_CONTROL = 0 */
    USB_DisablePin();                       /* 放掉 D+ 上拉，主机看到拔出 */
    s_configured = 0u;
    s_address    = 0u;
}

usbk_evt_t UsbPort_PollEvent(usbk_setup_t *setup)
{
    uint8_t ev = usbport_q_pop();

    if (ev == (uint8_t)USBK_EV_SETUP)
    {
        if (setup != 0)
        {
            *setup = s_setup;
        }
        s_setup_ready = 0u;
    }

    return (usbk_evt_t)ev;
}

void UsbPort_Ep0Send(const uint8_t *buf, uint16_t len)
{
    if (len > (uint16_t)USBPORT_EP0_MP)
    {
        len = (uint16_t)USBPORT_EP0_MP;
    }

    if ((buf != 0) && (len > 0u))
    {
        memcpy(pEP0_DataBuf, buf, len);
    }

    R8_UEP0_T_LEN = (uint8_t)len;
    usbport_ep0_arm();
}

uint16_t UsbPort_Ep0Recv(uint8_t *buf, uint16_t cap)
{
    uint16_t len = s_ep0_rxlen;

    if (len > cap)
    {
        len = cap;
    }
    if ((buf != 0) && (len > 0u))
    {
        memcpy(buf, pEP0_DataBuf, len);
    }

    s_ep0_rxlen = 0u;
    return len;
}

void UsbPort_Ep0Ack(void)
{
    /* 状态阶段：SET_ADDRESS 的新地址正是在这一刻生效的。
     * 提前写 R8_USB_DEV_AD 会让主机还没确认就换了地址，后续令牌抓不到。 */
    /* 【修复 SET_ADDRESS】改设备地址不在这里做，挪到 EP0 IN 完成中断
     * （见 UIS_TOKEN_IN 分支）。原因：R8_USB_DEV_AD 是入包过滤器，
     * 本函数被调用时主机的状态阶段 IN 还没发下来、而且是发往旧地址的，
     * 此刻改地址会让那一次 IN 被硬件丢掉，主机报设置地址请求失败。 */

    /* 0 长度包 = 状态阶段 */
    R8_UEP0_T_LEN = 0u;
    usbport_ep0_arm();
}

void UsbPort_Ep0Stall(void)
{
    usbport_ep0_stall_now();
}

void UsbPort_SetPendingAddress(uint8_t addr)
{
    s_pend_addr       = (uint8_t)(addr & 0x7Fu);
    s_pend_addr_valid = 1u;
}

bool UsbPort_Ep1Send(const uint8_t *buf, uint8_t len)
{
    if (s_ep1_in_flight != 0u)
    {
        return false;                       /* 上一包还没取走，别覆盖 */
    }

    if (len > (uint8_t)USBPORT_EP1_MP)
    {
        len = (uint8_t)USBPORT_EP1_MP;
    }

    if ((buf != 0) && (len > 0u))
    {
        memcpy(pEP1_IN_DataBuf, buf, len);
    }

    s_ep1_in_flight = 1u;
    DevEP1_IN_Deal(len);                    /* R8_UEP1_T_LEN = len; T_RES = ACK */

    return true;
}

bool UsbPort_Ep1Ready(void)
{
    return (s_ep1_in_flight == 0u);
}

uint16_t UsbPort_Ep1Recv(uint8_t *buf, uint16_t cap)
{
    uint16_t len = s_ep1_rxlen;

    if (len > cap)
    {
        len = cap;
    }
    if ((buf != 0) && (len > 0u))
    {
        memcpy(buf, pEP1_OUT_DataBuf, len);
    }

    s_ep1_rxlen = 0u;
    return len;
}

void UsbPort_SetConfigured(bool on)
{
    s_configured = (on != false) ? 1u : 0u;
}

bool UsbPort_IsConfigured(void)
{
    return (s_configured != 0u);
}

uint8_t UsbPort_Address(void)
{
    return s_address;
}

bool UsbPort_IsSuspended(void)
{
    return (s_suspended != 0u);
}

void UsbPort_ResetState(void)
{
    s_q_head = 0u;
    s_q_tail = 0u;
    s_setup_ready = 0u;
    s_configured  = 0u;
    s_suspended   = 0u;
    s_pend_addr_valid = 0u;
    s_ep0_armed   = 0u;
    s_ep0_rxlen   = 0u;
    s_ep1_rxlen   = 0u;
    s_ep1_in_flight = 0u;
    s_ep2_rxlen   = 0u;
    s_ep3_in_flight = 0u;
    s_ep4_in_flight = 0u;
    s_address     = 0u;
}

/*==========================================================================
 * CDC 端点的定义实现（EP2 OUT / EP3 IN / EP4 IN）
 *
 * 这几个函数是 usb_kbd_port.h 里声明的，CDC 不启用的时候：
 * USBKBD_ENABLE_CDC == 0，usb_kbd.c 不会调用它们，所以这几个只是占位
 * USB_DeviceInit() 里还是有配置 4/1/2/3 这几个端点，所以要定义出 DevEPx_OUT_Deal
 * 这些函数要么没人用会被删掉，要么就得保证链接能过。
 *========================================================================*/

/* ---- EP2 OUT，数据包接收缓冲区 ---- */
uint16_t UsbPort_Ep2Recv(uint8_t *buf, uint16_t cap)
{
    uint16_t len = s_ep2_rxlen;

    if (len > cap)
    {
        len = cap;
    }
    if ((buf != 0) && (len > 0u))
    {
        memcpy(buf, pEP2_OUT_DataBuf, len);
    }

    s_ep2_rxlen = 0u;
    return len;
}

/* ---- EP3 IN，数据包发送缓冲区 ---- */
bool UsbPort_Ep3Send(const uint8_t *buf, uint16_t len)
{
    if (s_ep3_in_flight != 0u)
    {
        return false;                       /* 上一包还没取走，别覆盖 */
    }

    if (len > (uint16_t)USBPORT_EP3_MP)
    {
        len = (uint16_t)USBPORT_EP3_MP;     /* 超过端点最大包长就截断 */
    }

    if ((buf != 0) && (len > 0u))
    {
        memcpy(pEP3_IN_DataBuf, buf, len);
    }

    s_ep3_in_flight = 1u;
    DevEP3_IN_Deal((uint8_t)len);           /* R8_UEP3_T_LEN = len; T_RES = ACK */

    return true;
}

bool UsbPort_Ep3Ready(void)
{
    return (s_ep3_in_flight == 0u);
}

/* ---- EP4 IN，CDC 通知 ---- */
bool UsbPort_Ep4Send(const uint8_t *buf, uint8_t len)
{
    if (s_ep4_in_flight != 0u)
    {
        return false;
    }

    if (len > (uint8_t)USBPORT_EP4_MP)
    {
        len = (uint8_t)USBPORT_EP4_MP;
    }

    if ((buf != 0) && (len > 0u))
    {
        memcpy(pEP4_IN_DataBuf, buf, len);
    }

    s_ep4_in_flight = 1u;
    DevEP4_IN_Deal(len);

    return true;
}

bool UsbPort_Ep4Ready(void)
{
    return (s_ep4_in_flight == 0u);
}

/*==========================================================================
 * WCH USB_DeviceInit() 预定义的回调仍然在，这四个空函数的目的是
 * 给库里的回调占位，真正的处理都通过事件队列交给主循环做
 *
 * 注意：EP2 OUT 的数据不在这里回调处理，而是抛事件 USBK_EV_EP2_OUT
 *       这个事件，主循环取出来后再统一处理，整个顺序就是
 *       "从 OUT 中断里 -> 塞进队列 -> 主循环处理" 这种时序
 *========================================================================*/
void DevEP1_OUT_Deal(uint8_t l) { (void)l; }
void DevEP2_OUT_Deal(uint8_t l) { (void)l; }
void DevEP3_OUT_Deal(uint8_t l) { (void)l; }
void DevEP4_OUT_Deal(uint8_t l) { (void)l; }

/******************* (C) CH592X USB 键盘 真机后端 *****************/
