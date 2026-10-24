/**************************************************************************
 * ui_transport.c —— 通信方式仲裁实现
 **************************************************************************/

#include "ui_transport.h"

/*==========================================================================
 * 内部：切阶段
 *========================================================================*/
static void ui_tr_stop_broadcast(ui_tr_t *t)
{
    if (t->action == 0)
    {
        return;
    }

    switch (t->phase)
    {
        case UI_TR_SCAN_RF24:
            t->action(UI_TR_RF24, false);   /* 停 2.4G 广播 */
            break;
        /* 【新增 SCAN_USB 分支】原来 a 阶段走 default 什么都不做。可 a 阶段
         * 恰恰是最容易锁上 USB 的阶段（USB 一插上、0.5s 内就锁），
         * 于是"锁定 USB"这条路径上从来没人调过 action(UI_TR_BLE,false)。
         * 而 radio_enter_kbd() 进键盘页时是无条件开 BLE 广播的 ——
         * 结果就是【连接 USB 时蓝牙仍然开着】，且射频一直被 BLE 占着。
         *
         * 【SCAN_BLE 分支加 cur 判断】锁定到 BLE 时**不能**在这里关广播：
         * hiddev 在 GAPROLE_CONNECTED 里已经关过一次，重复调
         * GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED,...) 只会再推一次
         * GAP 状态机，是"连上又被拆"的一个嫌疑点。锁定到 USB 时才需要关。 */
        case UI_TR_SCAN_BLE:
            if (t->cur != UI_TR_BLE)
            {
                t->action(UI_TR_BLE, false);/* 要锁的不是 BLE -> 必须关 */
            }
            break;
        case UI_TR_SCAN_USB:
            t->action(UI_TR_BLE, false);    /* 离开 a 阶段一律关 BLE */
            break;
        default:
            break;
    }
}

static void ui_tr_enter(ui_tr_t *t, ui_tr_phase_t ph)
{
    if ((t->phase == ph) && (t->started != 0u))
    {
        return;                             /* 已经在里面了 */
    }

    ui_tr_stop_broadcast(t);

    t->phase    = ph;
    t->phase_ms = 0u;
    t->started  = 1u;

    if (t->action == 0)
    {
        return;
    }

    switch (ph)
    {
        case UI_TR_SCAN_RF24:
            /* 【新增】这 1.5s 射频归 2.4G，BLE 一个包也发不出去。
             * 不先把广播关掉，随后的 HALRF_BtOwnsPhy(false) -> RF_Config()
             * 会把正在发的那一帧打断在物理层 —— 手机看到的就是"设备时有时无"，
             * 而刚连上的链路会被当场拆掉。
             * 关掉之后 c 阶段会重新开（那时 PHY 已经交还 BLE）。 */
            t->action(UI_TR_BLE, false);

            t->rf24_ms = 0u;
            t->rf24_tries++;
            t->action(UI_TR_RF24, true);    /* 开始广播 2.4G */
            break;
        case UI_TR_SCAN_BLE:
            t->ble_tries++;
            t->action(UI_TR_BLE, true);     /* 开始广播 BLE */
            break;
        default:
            break;
    }
}

static bool ui_tr_channel_ok(const ui_tr_t *t)
{
    switch (t->cur)
    {
        case UI_TR_USB:  return (t->usb_plugged != 0u) && (t->usb_ready != 0u);
        case UI_TR_RF24: return (t->rf24_alive != 0u);
        case UI_TR_BLE:  return (t->ble_ready != 0u);
        default:         return false;
    }
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
void UiTr_Init(ui_tr_t *t)
{
    if (t == 0)
    {
        return;
    }

    /* 回调不动（上层注册的），其余全清 */
    {
        void (*act)(ui_transport_t, bool)   = t->action;
        bool (*snd)(ui_transport_t, const uint8_t *, uint8_t) = t->send;

        t->action = act;
        t->send   = snd;

        t->phase    = UI_TR_SCAN_USB;
        t->cur      = UI_TR_NONE;
        t->phase_ms = 0u;
        t->rf24_ms  = 0u;
        t->started  = 0u;
        t->usb_tries = 0u;
        t->rf24_tries = 0u;
        t->ble_tries  = 0u;
        t->switches   = 0u;

        t->usb_plugged = 0u;
        t->usb_ready   = 0u;
        t->rf24_alive  = 0u;
        t->ble_ready   = 0u;
    }
}

void UiTr_Tick(ui_tr_t *t, uint16_t dt_ms)
{
    if (t == 0)
    {
        return;
    }

    t->phase_ms += dt_ms;

    /* ---------------- 已经锁定：通道掉了就从头再来 ---------------- */
    if (t->phase == UI_TR_LOCKED)
    {
        if (!ui_tr_channel_ok(t))
        {
            ui_tr_phase_t back = UI_TR_SCAN_USB;

            /* 【2026-10-14 修复：蓝牙无法稳定连接】
             *
             * 原来这里无条件 ui_tr_enter(t, UI_TR_SCAN_USB)，
             * 也就是【通道掉了 -> 从头重走 a->b->c】。
             *
             * 对 USB / 2.4G 掉线这样重挑是对的；但**对 BLE 掉线是灾难**：
             * a 阶段 0.5s + b 阶段 1.5s 一共 2 秒里 want_bt_phy() 返回 false
             * （b 阶段确实要给 2.4G；a 阶段原来也一起给了，已在 Main.c 里修掉）
             * -> HALRF_BtOwnsPhy(false) -> RF_Config() 重写 PHY
             * -> **BLE 广播物理上发不出去**。
             * 而主机在断链后是立刻、反复重连的 —— 这 2 秒里它每次都失败，
             * 现场表现就是【蓝牙断了以后再连不上，过一会儿才行】。
             *
             * 现在按【刚才用的是谁】决定回哪：
             *   刚才走 BLE -> 直接回 c 阶段（SCAN_BLE），广播立刻恢复，主机马上能重连
             *   其它       -> 照旧回 a 阶段（保留【重挑通道】的原设计）
             *
             * 回 c 阶段不丢东西：SCAN_BLE 每一拍仍然会回头查 USB 热插拔。
             * 只有【2.4G 要对上码】这一条会错过，而刚掉的是 BLE 通道，
             * 用户显然在用蓝牙，这个取舍是对的。 */
            if (t->cur == UI_TR_BLE)
            {
                back = UI_TR_SCAN_BLE;
            }

            t->cur = UI_TR_NONE;
            t->switches++;

            /* 回 back 指定的阶段：给新通道一次机会 */
            ui_tr_enter(t, back);

            if (back == UI_TR_SCAN_USB)
            {
                t->usb_tries++;
            }
        }
        return;
    }

    switch (t->phase)
    {
        /* ---------- a. 判断 USB（0.5s） ---------- */
        case UI_TR_SCAN_USB:
            if ((t->usb_plugged != 0u) && (t->usb_ready != 0u))
            {
                t->cur = UI_TR_USB;
                ui_tr_enter(t, UI_TR_LOCKED);
                return;
            }
            /* 【新增】USB 没插上，但 BLE 已经连上了：直接锁定。
             * 与其再花 1.5s 去探一个用不上的 2.4G，不如把射频留给
             * 已经建好的链路。 */
            if (t->ble_ready != 0u)
            {
                t->cur = UI_TR_BLE;
                ui_tr_enter(t, UI_TR_LOCKED);
                return;
            }
            if (t->phase_ms >= (uint32_t)UI_TR_T_USB_MS)
            {
                ui_tr_enter(t, UI_TR_SCAN_RF24);
            }
            break;

        /* ---------- b. 广播 2.4G（1.5s，其中 1s 内要收到心跳） ---------- */
        case UI_TR_SCAN_RF24:
            t->rf24_ms += dt_ms;

            if ((t->rf24_alive != 0u)
                && (t->rf24_ms <= (uint32_t)UI_TR_RF24_PAIR_MS))
            {
                /* 对上了就不用再广播：ui_tr_enter() 内部会停掉本阶段广播。
                 * 原来这里先 ui_tr_stop_broadcast() 再手动改 phase，
                 * 紧接着 ui_tr_enter() 又判了一次"已经在里面"直接返回 ——
                 * 绕成两段，且 phase 被抢在 ui_tr_enter() 之前改掉，
                 * 后面所有"按 phase 判断"的分支都读到错误的值。 */
                t->cur = UI_TR_RF24;
                ui_tr_enter(t, UI_TR_LOCKED);
                return;
            }

            /* 【新增】探 2.4G 的这 1.5s 里 BLE 若连上了，立刻停探。
             * CH592 只有一套射频前端，继续探 2.4G 等于把一条刚建立的
             * BLE 链路从物理层打死 —— 现场"连上(23) -> 立刻断(6)"的
             * 主要成因就在这里。 */
            if (t->ble_ready != 0u)
            {
                t->cur = UI_TR_BLE;
                ui_tr_enter(t, UI_TR_LOCKED);
                return;
            }

            if (t->phase_ms >= (uint32_t)UI_TR_T_RF24_MS)
            {
                ui_tr_enter(t, UI_TR_SCAN_BLE);
            }
            break;

        /* ---------- c. 广播 BLE（无限等） ---------- */
        case UI_TR_SCAN_BLE:
            /* "无限等"只是说不设超时，但**不能真的干等**：这块板子常常是
             * "先上电、再插线"，而 USB 是最高优先级。所以这里每一拍回头看一眼 USB，
             * 否则一旦走到 BLE 阶段就再也回不去了，USB 就算枚举成功也用不上。
             * 注意：2.4G 不在这里补看 —— 规格要求"前 1s 要收到心跳"，迟到的
             * 2.4G 不算配对成功；已经 LOCKED 的通道也不会被抢走，那是有意设计的。 */
            if ((t->usb_plugged != 0u) && (t->usb_ready != 0u))
            {
                t->cur = UI_TR_USB;
                ui_tr_enter(t, UI_TR_LOCKED);
                return;
            }
            if (t->ble_ready != 0u)
            {
                t->cur = UI_TR_BLE;
                ui_tr_enter(t, UI_TR_LOCKED);
            }
            break;

        default:
            break;
    }
}

ui_transport_t UiTr_Current(const ui_tr_t *t)
{
    return (t != 0) ? t->cur : UI_TR_NONE;
}

ui_tr_phase_t UiTr_Phase(const ui_tr_t *t)
{
    return (t != 0) ? t->phase : UI_TR_SCAN_USB;
}

bool UiTr_Connected(const ui_tr_t *t)
{
    if (t == 0)
    {
        return false;
    }
    if (t->phase != UI_TR_LOCKED)
    {
        return false;
    }
    return ui_tr_channel_ok(t);
}

const char *UiTr_Name(ui_transport_t tr)
{
    switch (tr)
    {
        case UI_TR_USB:  return "USB";
        case UI_TR_RF24: return "2.4G";
        case UI_TR_BLE:  return "BLE";
        default:         return "--";
    }
}

const char *UiTr_StateText(const ui_tr_t *t)
{
    if ((t == 0) || (t->phase != UI_TR_LOCKED))
    {
        return "--";
    }
    return UiTr_Name(t->cur);
}

/*==========================================================================
 * 底层喂状态
 *========================================================================*/
void UiTr_SetUsb(ui_tr_t *t, bool plugged, bool ready)
{
    if (t != 0)
    {
        t->usb_plugged = (plugged != 0) ? 1u : 0u;
        t->usb_ready   = (ready != 0) ? 1u : 0u;
    }
}

void UiTr_SetRf24(ui_tr_t *t, bool alive)
{
    if (t != 0)
    {
        t->rf24_alive = (alive != 0) ? 1u : 0u;
    }
}

void UiTr_SetBle(ui_tr_t *t, bool ready)
{
    if (t != 0)
    {
        t->ble_ready = (ready != 0) ? 1u : 0u;
    }
}

void UiTr_SetAction(ui_tr_t *t, void (*fn)(ui_transport_t tr, bool starting))
{
    if (t != 0)
    {
        t->action = fn;
    }
}

void UiTr_SetSend(ui_tr_t *t, bool (*fn)(ui_transport_t tr, const uint8_t *rep, uint8_t len))
{
    if (t != 0)
    {
        t->send = fn;
    }
}

bool UiTr_SendHid(ui_tr_t *t, const uint8_t *rep, uint8_t len)
{
    if ((t == 0) || (rep == 0) || (len == 0u))
    {
        return false;
    }
    if (!UiTr_Connected(t))
    {
        return false;
    }
    if (t->send == 0)
    {
        return false;
    }

    return t->send(t->cur, rep, len);
}
