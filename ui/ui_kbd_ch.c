/**************************************************************************
 * ui_kbd_ch.c —— 三通道接线实现
 *
 * 关键点：
 *   * 报文是 8 字节 6KRO（modifiers + 6 keys），和 USB 引导协议一致。
 *   * USB / BLE 两个模块各自维护"哪些键按下"和"每个键是什么键值"，所以切换
 *     通道时要把编辑好的按键状态**重新喂一遍**，否则新通道不知道现在按着什么。
 *   * USB 报表层（usb_kbd_report）的键值表是它自己的一份，默认 K1~K9 = 数字
 *     1~9。这里在喂按键前先用 ch_usb_sync_map() 把 3x3 当前的映射灌进去，
 *     否则网页/本机改完映射，USB 通道还是按默认值发。2.4G / BLE 用的是
 *     UiKbd_BuildHid6() 现算的报表，不受影响。
 *   * 2.4G 走自己那套协议：用 Kb_BuildHid6() 把 8 字节报表打成协议帧。
 **************************************************************************/

#include "ui_kbd_ch.h"
#include <string.h>
#include "ui_kbd.h"
#include "key_matrix.h"

#include "usb_kbd_report.h"
#include "usb_kbd.h"
#include "bt_hid.h"
#include "proto_kb.h"
#include "rf_cfg.h"
#include "bt_wch.h"
#include "hal_rf.h"

/* 2.4G 心跳：由 Main.c 从 rf24 主机轮询里喂进来 */
static uint8_t  s_rf24_alive;
static uint16_t s_rf_hb_ms;     /* 距上次 2.4G 成功交换多少毫秒 */
static uint32_t s_hb_last_ms;   /* 上次算心跳差值的真实毫秒 */

/*==========================================================================
 * 真机端口：接到三个真实模块上
 *========================================================================*/
static bool ch_usb_plugged(void) { return UsbKbd_IsPlugged(); }
static bool ch_usb_ready(void)   { return UsbKbd_IsConnected(); }
static bool ch_rf24_alive(void)  { return s_rf24_alive != 0u; }
static bool ch_ble_ready(void)   { return BT_WCH_IsConnected(); }

/*==========================================================================
 * USB 通道：把 3x3 当前生效的映射灌进 USB 报表层
 *
 * usb_kbd_report（报表层）自己维护一份"哪个键 = 哪个 HID 用途码"的表，
 * 默认是 1~9；映射改了以后必须同步过去，否则 USB 通道永远发默认值。
 *
 * 映射值拆法（和 ui_kbd.c 一致）：
 *   高字节 = 修饰键位图，低字节 = 主键 HID Usage
 *   低字节是 0xE0~0xE7（单独一个修饰键）时，报表层的约定是
 *   usage=0 + 修饰位单独给（见 usb_kbd_report.h 的 UsbKbd_SetModifierBit），
 *   这样它才不占 6 个键码槽位。
 *========================================================================*/
static uint8_t s_usb_usage[9];      /* 上次灌进去的值，用来判断要不要重灌 */
static uint8_t s_usb_mods[9];
static uint8_t s_usb_synced;        /* 0 = 开机还没灌过，第一次必须全量 */

static void ch_usb_sync_map(void)
{
    uint8_t k;
    bool    changed = false;

    for (k = 1u; k <= 9u; k++)
    {
        UsbKbdKey_t uk    = (UsbKbdKey_t)(k - 1u);
        uint8_t     usage = UiKbd_UsageOf(k);
        uint8_t     mods  = UiKbd_ModsOf(k);
        uint8_t     code  = usage;

        if (UiKbd_UsageModBit(usage) != 0u)
        {
            code = 0u;                  /* 纯修饰键：不占键码槽位 */
        }

        if ((s_usb_synced == 0u) ||
            (s_usb_usage[k - 1u] != code) ||
            (s_usb_mods[k - 1u] != mods))
        {
            s_usb_usage[k - 1u] = code;
            s_usb_mods[k - 1u]  = mods;

            UsbKbd_SetUsage(uk, code);
            UsbKbd_SetModifierBit(uk, mods);
            changed = true;
        }
    }

    if (changed)
    {
        s_usb_synced = 1u;

        /* 映射变了补一帧：正按着键的时候，主机那边也能立刻更新 */
        UsbKbd_MarkDirty();
    }
}

static void ch_usb_key(uint8_t key, bool down)
{
    /* 键编号 1..9 和 UsbKbdKey_t 的 K1..K9 一致 */
    if (key >= 1u && key <= 9u)
    {
        ch_usb_sync_map();          /* 先按当前映射更新报表层的键值表 */
        UsbKbd_KeyEvent((UsbKbdKey_t)(key - 1u), down);
    }
}

static void ch_ble_key(uint8_t key, bool down)
{
    if (key >= 1u && key <= 9u)
    {
        BT_HID_KeyEvent((BtHidKey_t)(key - 1u), down);
    }
}

static bool ch_usb_send(const uint8_t *rep8)
{
    (void)rep8;
    /* 报表内容已经通过 UsbKbd_KeyEvent() 喂进去了，这里只让它发出去。
     * 发之前再同步一次：网页在线改完映射、键还按着的时候也能立刻生效。 */
    ch_usb_sync_map();

    /* 额外键（编码器单击 = 3x3 之外的按键）：USB 报表层自己维护状态，
     * 要单独喂；BLE / 2.4G 直接看 rep8 里的内容就够了。 */
    UsbKbd_KeyEvent(USBKBD_ENC_BTN, UiKbd_ExtraIsDown());

    return UsbKbd_SendReportNow();
}

/*==========================================================================
 * 2.4G：本机是**键盘设备（从机）**，不是主机
 *
 * 协议方向（proto_common.h 的帧字典）：
 *     Host -- KB_POLL(5B) --> CH592X -- KB_HID_6KRO(8B) --> Host
 * 所以：
 *   * HID 报文**不能主动发**，只能先存进 kb_dev_t，等被 POLL 时回上去
 *   * 收到 POLL 才算连上（心跳来源）；1.5s 没 POLL 就当掉线
 *   * 更早的版本用 HALRF_Tx() 主动投帧，方向是反的，已改掉
 *========================================================================*/
static kb_dev_t s_kbdev;

static void ch_rf24_set_report(const uint8_t *rep8)
{
    hid_report_t r;
    uint8_t      i;

    if (rep8 == 0)
    {
        return;
    }

    /* 8 字节引导报文 -> hid_report_t（modifiers + 6 keys + NKRO 清零） */
    r.modifiers = rep8[0];
    for (i = 0u; i < HID_6KRO_KEYS; i++)
    {
        r.keys[i] = rep8[2u + i];
    }
    for (i = 0u; i < HID_NKRO_BYTES; i++)
    {
        r.nkro[i] = 0u;
    }

    Kb_DevSetReport(&s_kbdev, &r);      /* 存着，等 POLL 来才回上去 */
}

/*********************************************************************
 * @fn      UiKbdCh_SetRadioMode
 * @brief   键盘用的 2.4G 和遥控用的 2.4G **抢同一颗射频**，必须互斥
 *
 * 为什么必须互斥：
 *   CH592 只有一个 accessAddress。键盘模式在本机链路地址上等主机 POLL；
 *   而遥控协议的 proto_rc.c 自己会调 HALRF_SetLink()：
 *       枚举阶段 -> h->enum_addr（0xAABBCCDD01）
 *       数据阶段 -> h->rc_addr
 *   两个都用同一颗射频、同一个 HAL 状态。如果同时跑：
 *     * 键盘的 HALRF_RxWait() 会等在遥控的地址上，永远收不到 POLL
 *     * 谁也发不成，互相把对方的地址覆盖掉
 *
 * 所以进入遥控模式时调 UiKbdCh_SetRadioMode(false)：
 *   1) 把射频链路地址**还给键盘**（遥控退出后也要还，否则键盘收不到 POLL）
 *   2) 清掉 2.4G 心跳，让仲裁器重新判断，不会抱着一个假的"还连着"
 *
 * @param   keyboard_on  true = 键盘用射频；false = 遥控用（键盘让出去）
 *********************************************************************/
void UiKbdCh_SetRadioMode(bool keyboard_on)
{
    if (keyboard_on)
    {
        /* 遥控退出：把地址切回键盘本机的，并重新从配对信息取一次 */
        rf_cfg_kb_t kcfg;

        (void)RfCfg_KbLoad(&kcfg);
        HALRF_SetLink(kcfg.addr, kcfg.channel);

        s_rf24_alive = 0u;          /* 心跳清零：要等真的收到 POLL 才算连上 */
        s_rf_hb_ms   = 0u;
    }
    else
    {
        /* 进遥控：键盘撒手。地址由 proto_rc.c 自己管，这里只清心跳 */
        s_rf24_alive = 0u;
        s_rf_hb_ms   = 0u;
    }
}

/*********************************************************************
 * @fn      UiKbdCh_PollRf24
 * @brief   主循环里调：等主机 POLL，收到就回一帧 HID 并喂心跳
 * @return  true = 这一轮确实收到了合法 POLL
 *********************************************************************/
bool UiKbdCh_PollRf24(void)
{
    uint8_t in[RF_PLOAD_MAX];
    uint8_t inlen = 0u;
    uint8_t out[RF_PLOAD_MAX];
    uint8_t outlen = 0u;

    if (HALRF_RxWait(in, &inlen, RF_KB_RX_WINDOW_US) != HALRF_OK)
    {
        return false;                   /* 这一轮主机没叫 */
    }

    /* 【修复 P2-3】把射频实际收到的字节数透传给协议层：短帧在那里就被拒，
     * 不会拿着越界的 in5[1..4] 去改键状态（幽灵按键）。 */
    if (!Kb_DevOnPoll(&s_kbdev, in, inlen, out, &outlen))
    {
        return false;                   /* 不是 POLL / 帧坏了 */
    }

    (void)HALRF_Tx(out, outlen);        /* 把 HID 报文回上去 */

    s_rf24_alive = 1u;                  /* 收到 POLL = 主机还在 */
    s_rf_hb_ms   = 0u;
    return true;
}

static bool ch_ble_send(const uint8_t *rep8)
{
    /* 先让 bt_hid.c 把报文刷新一下（它记着键的状态），
     * 再把**已经算好的** rep8 交给 bt_wch 发出去。
     *
     * 注意：这里**不能**再走 bt_port.c 的 BT_SendHid()。
     * bt_port 那个后端指针要靠 BT_Init() 注册，而本工程从头到尾没调过
     * BT_Init()，所以 s_be 一直是 0，BT_SendHid() 永远返回 false ——
     * BLE 报文就是这么"算对了但发不出去"的。 */
    if (rep8 == 0)
    {
        return false;
    }

    (void)BT_HID_Flush();

    return BT_WCH_SendHid(rep8, UIK_HID_REPORT_LEN);
}

static void ch_broadcast(ui_transport_t tr, bool starting)
{
    if (tr == UI_TR_BLE)
    {
        /* doc (2).1.c: 广播 BLE 等待连接 */
        if (starting) { BT_WCH_StartAdvertising(); }
        else          { BT_WCH_StopAdvertising(); }
    }
    else if (tr == UI_TR_RF24)
    {
        /* 【绝对不要在这里动 s_rf24_alive！】
         * 2.4G 的"还活着"只能由真的收到主机 POLL 来决定（见 UiKbdCh_PollRf24()：
         * Kb_DevOnPoll() 成功才置位）。这里原来写的是
         *     s_rf24_alive |= (starting ? 1 : 0);
         * 结果仲裁器一进 SCAN_RF24 就通过本回调把自己标成"已连接"，
         * 下一拍就在 ui_transport.c 里锁定 2.4G —— 对面根本没有接收器也一样。
         * 表现：插上电脑也一直显示 "2.4G / OK"，USB 永远轮不上。 */
        (void)starting;             /* 2.4G 不需要在这里做任何事 */
    }
}

static void ch_reset_keys(void)
{
    /* 真机上没有单独的"清零挴键"接口，但能用 ReleaseAll() 把
     * 两边都放开；只要不往当前通道丢"松开"报文就不会误发。 */
    UsbKbd_ReleaseAll();
    BT_HID_ReleaseAll();
}

static const uikbdch_port_t s_real_port =
{
    ch_usb_plugged, ch_usb_ready, ch_rf24_alive, ch_ble_ready,
    ch_usb_key, ch_ble_key,
    ch_usb_send, ch_rf24_set_report, ch_ble_send,
    ch_broadcast, ch_reset_keys
};

/*==========================================================================
 * 状态
 *========================================================================*/
static const uikbdch_port_t *s_port;
static uint16_t              s_prev_map;    /* 上一次喂出去的按键位图 */
static uint32_t              s_frames;
static uint8_t               s_started;

/*==========================================================================
 * 出口：ui_kbd 把报文交到这里
 *========================================================================*/
static bool ch_send(ui_transport_t tr, const uint8_t *rep, uint8_t len)
{
    bool ok = false;

    if ((s_port == 0) || (rep == 0) || (len != (uint8_t)UIK_HID_REPORT_LEN))
    {
        return false;
    }

    switch (tr)
    {
        case UI_TR_USB:  ok = (s_port->usb_send != 0) && s_port->usb_send(rep); break;
        case UI_TR_RF24:
            if (s_port->rf24_set_report != 0) { s_port->rf24_set_report(rep); ok = true; }
            break;
        case UI_TR_BLE:  ok = (s_port->ble_send != 0) && s_port->ble_send(rep); break;
        default:         ok = false; break;
    }

    if (ok) { s_frames++; }
    return ok;
}

/* 只喂给**当前通道**的报表层：
 *   USB  -> UsbKbd_KeyEvent（它自己维护报表）
 *   BLE  -> BT_HID_KeyEvent（同上）
 *   2.4G -> 没有逐键状态，每次都是完整的 8 字节报文，不需要喂
 * 否则切到 2.4G 时会把按键喂给一条根本不用它的通道。 */
static void ch_feed_keys(ui_transport_t tr, uint16_t map, uint16_t prev)
{
    uint8_t k;
    uint16_t diff = (uint16_t)(map ^ prev);

    if (tr == UI_TR_RF24)
    {
        return;
    }

    /* 额外键（编码器单击发的那个键）不占 3x3 位图，这里顺带对齐一次，
     * 切通道时免得它在 USB 报表层里卡在"按下"。*/
    if (tr == UI_TR_USB)
    {
        UsbKbd_KeyEvent(USBKBD_ENC_BTN, UiKbd_ExtraIsDown());
    }

    for (k = 1u; k <= 9u; k++)
    {
        uint16_t bit = (uint16_t)(1u << (k - 1u));
        bool     down;

        if ((diff & bit) == 0u)
        {
            continue;
        }

        down = ((map & bit) != 0u);

        if ((tr == UI_TR_USB) && (s_port->usb_key != 0)) { s_port->usb_key(k, down); }
        if ((tr == UI_TR_BLE) && (s_port->ble_key != 0)) { s_port->ble_key(k, down); }
    }
}

/*==========================================================================
 * 对外
 *========================================================================*/
void UiKbdCh_InitWith(const uikbdch_port_t *port)
{
    s_port     = (port != 0) ? port : &s_real_port;
    s_prev_map = 0u;
    s_frames   = 0u;
    s_started  = 1u;

    UiKbd_Init();
    UiKbd_SetSender(ch_send);
    UiKbd_SetBroadcaster(s_port->broadcast);
}

/* 主机每成功交换一次就叫一声（Kb_HostPoll() 返回 HALRF_OK）。
 * 不喂就会超时，仲裁会把 2.4G 当成掉线重新走一遍。 */
void UiKbdCh_OnRf24Heartbeat(void)
{
    s_rf24_alive = 1u;
    s_rf_hb_ms   = 0u;
}

void UiKbdCh_SetRf24Alive(bool alive)
{
    s_rf24_alive = (uint8_t)(alive ? 1u : 0u);
    s_rf_hb_ms   = 0u;
}
void UiKbdCh_SetBleReady(bool ready)  { (void)ready; }

/* 软件接线版初始化：只注册回调，不碰射频。
 * 开机走这个，射频等进键盘模式时再 UiKbdCh_Init()。 */
void UiKbdCh_InitSoft(void)
{
    s_rf24_alive = 0u;
    UiKbdCh_InitWith(&s_real_port);
}

void UiKbdCh_Init(void)
{
    s_rf24_alive = 0u;
    UiKbdCh_InitWith(&s_real_port);

    /* 2.4G 设备侧：读配对信息 -> 配射频 -> 建 kb_dev_t */
    {
        rf_cfg_kb_t  kcfg;
        rf_config_t  rcfg;

        (void)RfCfg_KbLoad(&kcfg);          /* 没记录就用默认（1:1 同地址） */

        memset(&rcfg, 0, sizeof(rcfg));
        memcpy(rcfg.tx_addr, kcfg.addr, RF_ADDR_LEN);
        memcpy(rcfg.rx_addr, kcfg.addr, RF_ADDR_LEN);
        rcfg.channel     = kcfg.channel;
        rcfg.rate        = RF_RATE_1M;
        rcfg.crc16_en    = true;
        rcfg.auto_ack_en = true;

        HALRF_Init(&rcfg);

        Kb_DevInit(&s_kbdev);
        s_kbdev.pair.nkro_en = (kcfg.nkro_en != 0u) ? true : false;
    }
}

void UiKbdCh_Task(void)
{
    uint16_t map;
    ui_transport_t before;

    if (s_started == 0u)
    {
        UiKbdCh_Init();
    }
    if (s_port == 0)
    {
        return;
    }

    /* 0) 2.4G 心跳超时：超过 1.5s 没成功交换就当掉线。
     *
     * 【修复 §8-12】按真实毫秒累加。原来是 s_rf_hb_ms++，即"每圈 +1ms"，
     * 碰上刷屏或收发那一圈几十毫秒时，1.5s 会被拉长好几倍，掉线判定失灵。
     * 差值每圈都算（不管当前 alive 与否），限幅 100ms 同 ui_kbd.c。 */
    {
        uint32_t now = Game_Millis();
        uint32_t d   = now - s_hb_last_ms;

        s_hb_last_ms = now;
        if (d > 100u) { d = 100u; }

        if (s_rf24_alive != 0u)
        {
            s_rf_hb_ms = (uint16_t)(s_rf_hb_ms + (uint16_t)d);
            if (s_rf_hb_ms >= (uint16_t)UI_TR_T_RF24_MS)
            {
                s_rf24_alive = 0u;
            }
        }
    }

    /* 1) 把三条通道的当前状态喂给仲裁器 */
    UiKbd_SetUsb(s_port->usb_plugged(),
                 s_port->usb_ready());
    UiKbd_SetRf24(s_port->rf24_alive());
    UiKbd_SetBle(s_port->ble_ready());

    /* 2) 推进仲裁 + 发报文 */
    before = UiKbd_Channel();
    UiKbd_Task();

    /* 3) 换了通道：先把两边的旧状态清掉（避免幻影按键），
     *    再把当前按键状态重喂给新通道 */
    if (UiKbd_Channel() != before)
    {
        if (s_port->reset_keys != 0) { s_port->reset_keys(); }
        map = KEY_GetMap();
        ch_feed_keys(UiKbd_Channel(), map, 0u);
        s_prev_map = map;
        return;
    }

    /* 4) 正常情况：只喂给当前通道，只喂变化的键 */
    map = KEY_GetMap();
    if (map != s_prev_map)
    {
        ch_feed_keys(UiKbd_Channel(), map, s_prev_map);
        s_prev_map = map;
    }
}

ui_transport_t UiKbdCh_Channel(void)   { return UiKbd_Channel(); }

bool           UiKbdCh_Connected(void) { return UiKbd_Connected(); }
uint32_t       UiKbdCh_FramesSent(void){ return s_frames; }

/******************* (C) CH592X 键盘通道接线 *****************/
