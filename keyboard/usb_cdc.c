/**************************************************************************
 * usb_cdc.c —— USB CDC 虚拟串口的设备侧胶水（实现）
 *
 * 见 usb_cdc.h 的职责说明。这里只做三件事：
 *
 *   1) 收：EP2 OUT 来的字节 -> 喂协议解析器 -> 成帧后交给业务层
 *   2) 发：业务层的应答帧 -> 排队 -> 等 USB 层来取
 *   3) 记：线状态（DTR）与 line coding，供上层和主机握手用
 *
 * ------------------------------------------------------------------
 * 【为什么发送要排队，不能直接发】
 *   USB 的 IN 端点是"主机来取才轮到设备发"。我们的应答是在
 *   "收到 OUT 数据"的那一刻产生的，那时主机未必在读 IN。
 *   所以必须先把应答攒起来，等 EP3 IN 空闲了由 UsbCdc_PollTx() 推出去。
 *
 * 【队列满了怎么办】
 *   丢最旧的。配置场景下主机是"发一条等一条"，几乎不会积压；
 *   真要积压说明主机侧出问题了，丢旧的比卡死好（不会死锁）。
 *
 * 【关于粘包/半包】
 *   全部由 Kmap_ParserPush 处理（它有帧头同步 + LEN + CRC），
 *   本文件不关心一次 OUT 收到的是半帧还是三帧半。
 * ------------------------------------------------------------------
 * 【一帧大于 64 字节怎么办】
 *   KMAP_MAX_PAYLOAD = 64，所以最长应答帧 = 7 + 64 = 71 字节，
 *   确实会超过端点的 64 字节单包上限。
 *   本模块的 UsbCdc_PollTx() 一次只吐"能塞进调用者缓冲"的部分，
 *   并在内部记住"这一帧还剩多少没发"，下次接着吐。
 *   USB 层只要在 EP3 IN 空闲时反复调，直到返回 0，就能正确发完。
 **************************************************************************/

#include "usb_cdc.h"
#include "kmap_proto.h"
#include "kmap_server.h"

#include <string.h>

#if (USB_CDC_ENABLE != 0)

/*==========================================================================
 * 1. 内部状态
 *========================================================================*/

/* ---- 解析器：把 EP2 OUT 的字节流切成帧 ---- */
static kmap_parser_t s_parser;

/* ---- 发送队列：存整帧 ---- */
static uint8_t  s_txq[USBCDC_TXQ_FRAMES][KMAP_MAX_FRAME];
static uint8_t  s_txq_len[USBCDC_TXQ_FRAMES];   /* 每帧实际字节数 */
static uint8_t  s_txq_head;                     /* 取的位置 */
static uint8_t  s_txq_tail;                     /* 放的位置 */
static uint8_t  s_txq_count;                    /* 当前有几帧 */

/* ---- 正在发送中的那一帧（可能跨多次 EP3 IN 才能发完） ---- */
static uint8_t  s_tx_cur[KMAP_MAX_FRAME];
static uint16_t s_tx_cur_len;       /* 整帧长度 */
static uint16_t s_tx_cur_off;       /* 已经吐出去多少 */

/* ---- 线状态与 line coding ---- */
static volatile uint16_t s_line_state;          /* DTR / RTS 位 */
static uint8_t           s_line_coding[7];      /* 主机下发的 7 字节 */
static bool              s_coding_valid;

/* ---- 统计 ---- */
static uint32_t s_rx_bytes;
static uint32_t s_tx_frames;
static uint32_t s_tx_drops;

/*==========================================================================
 * 2. 队列操作
 *========================================================================*/

/* 往队尾放一帧；队满时丢队头（最旧的） */
static void usbcdc_q_push(const uint8_t *frame, uint8_t len)
{
    if ((frame == 0) || (len == 0u))
    {
        return;
    }

    if (s_txq_count >= (uint8_t)USBCDC_TXQ_FRAMES)
    {
        /* 队满：丢最旧的一帧，给新帧腾位置。
         * 主机侧一般是同步等应答，真到这一步说明对方已经不正常了，
         * 丢旧的至少保证最新的应答能出去（不会永久卡住）。 */
        s_txq_head = (uint8_t)((s_txq_head + 1u) % (uint8_t)USBCDC_TXQ_FRAMES);
        s_txq_count--;
        s_tx_drops++;
    }

    memcpy(s_txq[s_txq_tail], frame, len);
    s_txq_len[s_txq_tail] = len;
    s_txq_tail = (uint8_t)((s_txq_tail + 1u) % (uint8_t)USBCDC_TXQ_FRAMES);
    s_txq_count++;
}

/* 从队头取一帧出来，返回长度；0 = 队空
 * 注意：这里不直接用，是为了跟 usbcdc_q_push 成对存在，将来要用时可直接取。
 * 目前 UsbCdc_PollTx 自己有"当前帧 + 分片"的逻辑，不需要这个简单版。 */
#if 0
static uint8_t usbcdc_q_pop(uint8_t *out, uint8_t cap)
{
    uint8_t n;

    if ((s_txq_count == 0u) || (out == 0))
    {
        return 0u;
    }

    n = s_txq_len[s_txq_head];
    if (n > cap)
    {
        /* 调用者缓冲比整帧还小：只搬能装下的。
         * 本模块内部缓冲是 KMAP_MAX_FRAME，调用者也应该给够，
         * 这里做个保护，避免越界。 */
        n = cap;
    }

    memcpy(out, s_txq[s_txq_head], n);

    s_txq_head = (uint8_t)((s_txq_head + 1u) % (uint8_t)USBCDC_TXQ_FRAMES);
    s_txq_count--;

    return n;
}
#endif

/*==========================================================================
 * 3. 收：处理一批来自 EP2 OUT 的字节
 *========================================================================*/
void UsbCdc_OnDataOut(const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    kmap_frame_t fr;
    uint8_t      reply[KMAP_MAX_FRAME];
    uint16_t     rlen;

    if ((buf == 0) || (len == 0u))
    {
        return;
    }

    s_rx_bytes += len;

    for (i = 0u; i < len; i++)
    {
        /* 逐字节喂。成帧了才进业务处理。
         * 半包/脏数据/粘包都在解析器里消化掉了。 */
        if (!Kmap_ParserPush(&s_parser, buf[i], &fr))
        {
            continue;
        }

        /* 成帧 -> 交给业务层，拿回一帧应答 */
        rlen = KmapServer_HandleFrame(fr.cmd, fr.seq, fr.payload, fr.len, reply);

        if (rlen == 0u)
        {
            continue;               /* 理论上不会；防御性跳过 */
        }

        /* 应答入队，等 EP3 IN 空闲时发 */
        usbcdc_q_push(reply, (uint8_t)((rlen > 0xFFu) ? 0xFFu : rlen));
    }
}

/*==========================================================================
 * 4. 发：USB 层在 EP3 IN 空闲时反复调，直到返回 0
 *========================================================================*/
uint16_t UsbCdc_PollTx(uint8_t *buf, uint16_t cap)
{
    uint16_t n;

    if ((buf == 0) || (cap == 0u))
    {
        return 0u;
    }

    /* --- 情况 A：上一帧还没发完，接着发 --- */
    if (s_tx_cur_off < s_tx_cur_len)
    {
        n = (uint16_t)(s_tx_cur_len - s_tx_cur_off);
        if (n > cap)
        {
            n = cap;
        }

        memcpy(buf, &s_tx_cur[s_tx_cur_off], n);
        s_tx_cur_off = (uint16_t)(s_tx_cur_off + n);

        if (s_tx_cur_off >= s_tx_cur_len)
        {
            s_tx_cur_off = 0u;
            s_tx_cur_len = 0u;
            s_tx_frames++;
        }
        return n;
    }

    /* --- 情况 B：取新的一帧 --- */
    if (s_txq_count == 0u)
    {
        return 0u;                  /* 没东西可发 */
    }

    /* 从队列取整帧到 s_tx_cur（不直接给调用者，避免半帧时状态丢失） */
    {
        uint8_t qlen = s_txq_len[s_txq_head];

        memcpy(s_tx_cur, s_txq[s_txq_head], qlen);
        s_tx_cur_len = qlen;
        s_tx_cur_off = 0u;

        s_txq_head = (uint8_t)((s_txq_head + 1u) % (uint8_t)USBCDC_TXQ_FRAMES);
        s_txq_count--;
    }

    /* 立刻按调用者容量吐第一段 */
    n = (uint16_t)(s_tx_cur_len - s_tx_cur_off);
    if (n > cap)
    {
        n = cap;
    }

    memcpy(buf, &s_tx_cur[s_tx_cur_off], n);
    s_tx_cur_off = (uint16_t)(s_tx_cur_off + n);

    if (s_tx_cur_off >= s_tx_cur_len)
    {
        s_tx_cur_off = 0u;
        s_tx_cur_len = 0u;
        s_tx_frames++;
    }

    return n;
}

uint8_t UsbCdc_TxPending(void)
{
    /* 队列里有几帧 + 当前帧还没发完也算 */
    return (uint8_t)(s_txq_count + ((s_tx_cur_off < s_tx_cur_len) ? 1u : 0u));
}

/*==========================================================================
 * 5. 统计
 *========================================================================*/
uint32_t UsbCdc_RxBytes(void)   { return s_rx_bytes;  }
uint32_t UsbCdc_TxFrames(void)  { return s_tx_frames; }
uint32_t UsbCdc_TxDrops(void)   { return s_tx_drops;  }

/*==========================================================================
 * 6. 线状态
 *
 * CDC 的 SET_CONTROL_LINE_STATE 里：
 *   bit0 = DTR（Data Terminal Ready）
 *   bit1 = RTS（Request To Send）
 *
 * 网页端 navigator.serial 打开端口时，浏览器会把 DTR 拉高。
 * 所以「DTR == 1」基本等价于「网页已经把串口打开了」，
 * 可以用它来判断要不要响应命令（没开串口时不必浪费 CPU）。
 *========================================================================*/
void UsbCdc_SetLineState(uint16_t state)
{
    s_line_state = state;
}

uint16_t UsbCdc_LineState(void)
{
    return s_line_state;
}

bool UsbCdc_HostOpen(void)
{
    /* DTR 位 */
    return ((s_line_state & 0x0001u) != 0u) ? true : false;
}

/*==========================================================================
 * 7. Line Coding（波特率等）
 *
 * 虚拟串口其实不真的关心波特率（USB 批量传输是全速固定的），
 * 但主机（特别是 Windows 驱动）会发 SET_LINE_CODING，
 * 若不回一个像样的 GET_LINE_CODING，有些驱动会认为设备异常。
 * 所以原样存下来、原样回过去即可。
 *========================================================================*/
void UsbCdc_SetLineCoding(const uint8_t *coding7)
{
    if (coding7 == 0)
    {
        return;
    }
    memcpy(s_line_coding, coding7, 7u);
    s_coding_valid = true;
}

uint16_t UsbCdc_GetLineCoding(uint8_t *out7)
{
    if (out7 == 0)
    {
        return 0u;
    }

    if (!s_coding_valid)
    {
        /* 主机还没发过 SET_LINE_CODING，给一套最常见的默认值：
         *   115200 8N1
         * 免得驱动读回来全是 0 觉得奇怪。 */
        s_line_coding[0] = 0x00u;   /* dwDTERate = 115200 (0x0001C200) LE */
        s_line_coding[1] = 0xC2u;
        s_line_coding[2] = 0x01u;
        s_line_coding[3] = 0x00u;
        s_line_coding[4] = 0x00u;   /* bCharFormat = 1 stop bit  */
        s_line_coding[5] = 0x00u;   /* bParityType = none        */
        s_line_coding[6] = 0x08u;   /* bDataBits   = 8           */
        s_coding_valid = true;
    }

    memcpy(out7, s_line_coding, 7u);
    return 7u;
}

/*==========================================================================
 * 8. 初始化
 *========================================================================*/
void UsbCdc_Init(void)
{
    /* 解析器复位 */
    Kmap_ParserInit(&s_parser);

    /* 发送队列与当前帧清零 */
    memset(s_txq, 0, sizeof(s_txq));
    memset(s_txq_len, 0, sizeof(s_txq_len));
    memset(s_tx_cur, 0, sizeof(s_tx_cur));
    s_txq_head = 0u;
    s_txq_tail = 0u;
    s_txq_count = 0u;
    s_tx_cur_len = 0u;
    s_tx_cur_off = 0u;

    /* 线状态复位（DTR=0，表示网页还没开串口） */
    s_line_state = 0u;
    s_coding_valid = false;
    memset(s_line_coding, 0, sizeof(s_line_coding));

    /* 统计清零 */
    s_rx_bytes = 0u;
    s_tx_frames = 0u;
    s_tx_drops = 0u;

    /* 业务层统计也顺手清一下 */
    KmapServer_Init();
}

#else   /* USB_CDC_ENABLE == 0 */

/*--------------------------------------------------------------------------
 * 关掉 CDC 时的空实现，保证链接不会缺符号
 *------------------------------------------------------------------------*/
void UsbCdc_Init(void) { }
void UsbCdc_OnDataOut(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; }
uint16_t UsbCdc_PollTx(uint8_t *buf, uint16_t cap) { (void)buf; (void)cap; return 0u; }
uint8_t  UsbCdc_TxPending(void) { return 0u; }
uint32_t UsbCdc_RxBytes(void)  { return 0u; }
uint32_t UsbCdc_TxFrames(void) { return 0u; }
uint32_t UsbCdc_TxDrops(void)  { return 0u; }
void     UsbCdc_SetLineState(uint16_t state) { (void)state; }
uint16_t UsbCdc_LineState(void) { return 0u; }
bool     UsbCdc_HostOpen(void) { return false; }
void     UsbCdc_SetLineCoding(const uint8_t *coding7) { (void)coding7; }
uint16_t UsbCdc_GetLineCoding(uint8_t *out7) { (void)out7; return 0u; }

#endif  /* USB_CDC_ENABLE */

/******************* (C) CH592X 按键映射配置 传输层 *****************/
