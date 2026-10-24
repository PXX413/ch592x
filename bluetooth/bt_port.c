/**************************************************************************
 * bt_port.c —— CH592X 蓝牙公共层实现（设备表 / 扫描 / 选择 / 连接 / 事件）
 *
 * 这里不出现任何 WCH 协议栈的调用，全部通过 BtBackend_t 转发，
 * 所以它既能配真机后端（bt_wch.c），也能配内置模拟后端在 PC 上自测。
 **************************************************************************/

#include "bt_port.h"
#include <string.h>

/*==========================================================================
 * 内部状态
 *========================================================================*/
static const BtBackend_t *s_be;                 /* 当前后端 */
static BtState_t s_state = BT_STATE_OFF;

static BtDevice_t s_dev[BT_MAX_DEV];
static uint8_t    s_n;                          /* 表里几个设备 */
static uint8_t    s_sel;                        /* 光标 */
static int8_t     s_conn = -1;                  /* 已连接设备的下标 */

static BtEventType_t s_q[BT_EVENT_QUEUE];
static uint8_t       s_qh, s_qt, s_qc;

static uint16_t s_scan_left;                    /* 扫描剩余时间(ms)，0 表示靠栈自己结束 */

/*==========================================================================
 * 事件队列
 *========================================================================*/
static void bt_push(BtEventType_t ev)
{
    if (s_qc >= BT_EVENT_QUEUE)
    {
        return;                                 /* 满了就丢新事件，不阻塞 */
    }
    s_q[s_qt] = ev;
    s_qt = (uint8_t)((s_qt + 1u) % BT_EVENT_QUEUE);
    s_qc++;
}

bool BT_PollEvent(BtEventType_t *ev)
{
    if (ev == 0 || s_qc == 0u)
    {
        return false;
    }
    *ev = s_q[s_qh];
    s_qh = (uint8_t)((s_qh + 1u) % BT_EVENT_QUEUE);
    s_qc--;
    return true;
}

/*==========================================================================
 * 设备表
 *========================================================================*/
static int8_t bt_find(const uint8_t *addr)
{
    uint8_t i;

    for (i = 0; i < s_n; i++)
    {
        if (memcmp(s_dev[i].addr, addr, 6) == 0)
        {
            return (int8_t)i;
        }
    }
    return -1;
}

void BT_ClearDevices(void)
{
    uint8_t i;

    for (i = 0; i < BT_MAX_DEV; i++)
    {
        s_dev[i].used = false;
        s_dev[i].name[0] = '\0';
        s_dev[i].rssi = 127;
    }
    s_n = 0u;
    s_sel = 0u;
    s_conn = -1;
}

uint8_t BT_GetDeviceCount(void) { return s_n; }

const BtDevice_t *BT_GetDevice(uint8_t i)
{
    if (i >= s_n)
    {
        return 0;
    }
    return &s_dev[i];
}

uint8_t BT_GetSelected(void) { return s_sel; }

void BT_SetSelected(uint8_t i)
{
    if (i >= s_n)
    {
        return;
    }
    s_sel = i;
}

void BT_SelectNext(int8_t dir)
{
    if (s_n == 0u)
    {
        s_sel = 0u;
        return;
    }

    if (dir > 0)
    {
        s_sel = (uint8_t)((s_sel + 1u) % s_n);
    }
    else if (dir < 0)
    {
        s_sel = (uint8_t)((s_sel + s_n - 1u) % s_n);
    }
    else
    {
        /* dir == 0：什么也不做 */
    }
}

/*==========================================================================
 * 协议栈回调
 *========================================================================*/
void BT_OnDeviceFound(const uint8_t *addr, uint8_t addr_type, int8_t rssi, const char *name)
{
    int8_t idx;

    if (addr == 0)
    {
        return;
    }

    idx = bt_find(addr);

    if (idx < 0)
    {
        if (s_n >= BT_MAX_DEV)
        {
            return;                             /* 表满了：新设备丢弃 */
        }
        idx = (int8_t)s_n;
        s_dev[idx].used = true;
        memcpy(s_dev[idx].addr, addr, 6);
        s_dev[idx].name[0] = '\0';
        s_n++;
    }

    s_dev[idx].addr_type = addr_type;
    s_dev[idx].rssi = rssi;

    if (name != 0 && name[0] != '\0')
    {
        uint8_t k = 0u;
        while (name[k] != '\0' && k < (uint8_t)(BT_NAME_LEN - 1))
        {
            s_dev[idx].name[k] = name[k];
            k++;
        }
        s_dev[idx].name[k] = '\0';
    }

    bt_push(BT_EV_DEVICE_FOUND);
}

void BT_OnScanDone(void)
{
    if (s_state == BT_STATE_SCANNING)
    {
        s_state = (s_conn >= 0) ? BT_STATE_CONNECTED : BT_STATE_IDLE;
    }
    s_scan_left = 0u;
    bt_push(BT_EV_SCAN_DONE);
}

void BT_OnConnected(const uint8_t *addr)
{
    if (addr != 0)
    {
        int8_t idx = bt_find(addr);
        if (idx >= 0)
        {
            s_conn = idx;
            s_sel  = (uint8_t)idx;
        }
    }

    s_state = BT_STATE_CONNECTED;
    s_scan_left = 0u;
    bt_push(BT_EV_CONNECTED);
}

void BT_OnDisconnected(void)
{
    s_conn = -1;
    if (s_state == BT_STATE_CONNECTED)
    {
        s_state = BT_STATE_IDLE;
    }
    bt_push(BT_EV_DISCONNECTED);
}

void BT_OnHidReady(void)
{
    bt_push(BT_EV_HID_READY);
}

/*==========================================================================
 * 扫描 / 连接
 *========================================================================*/
bool BT_StartScan(void)
{
    if (s_be == 0 || s_state == BT_STATE_OFF)
    {
        return false;
    }
    if (s_state == BT_STATE_CONNECTED)
    {
        return false;                           /* 已连接就别扫了 */
    }

    if (s_be->scan_start != 0)
    {
        s_be->scan_start(BT_SCAN_MS);
    }
    s_state = BT_STATE_SCANNING;
    s_scan_left = BT_SCAN_MS;
    return true;
}

void BT_StopScan(void)
{
    if (s_be != 0 && s_be->scan_stop != 0)
    {
        s_be->scan_stop();
    }
    s_scan_left = 0u;
    if (s_state == BT_STATE_SCANNING)
    {
        s_state = (s_conn >= 0) ? BT_STATE_CONNECTED : BT_STATE_IDLE;
    }
}

bool BT_IsScanning(void) { return (s_state == BT_STATE_SCANNING) ? true : false; }

bool BT_ConnectSelected(void)
{
    if (s_be == 0 || s_state == BT_STATE_CONNECTED)
    {
        return false;
    }
    if (s_n == 0u || s_sel >= s_n)
    {
        return false;
    }

    BT_StopScan();

    s_state = BT_STATE_CONNECTING;
    if (s_be->connect != 0 && s_be->connect(&s_dev[s_sel]))
    {
        return true;
    }

    s_state = BT_STATE_IDLE;
    return false;
}

void BT_Disconnect(void)
{
    if (s_be != 0 && s_be->disconnect != 0)
    {
        s_be->disconnect();
    }
    s_conn = -1;
    if (s_state == BT_STATE_CONNECTED || s_state == BT_STATE_CONNECTING)
    {
        s_state = BT_STATE_IDLE;
    }
}

const BtDevice_t *BT_GetConnectedDevice(void)
{
    if (s_conn < 0)
    {
        return 0;
    }
    return &s_dev[s_conn];
}

/*==========================================================================
 * 宿主白名单（只允许选中的设备来连我们）
 *========================================================================*/
static bool     s_host_valid;
static uint8_t  s_host_addr[6];
static char     s_host_name[BT_NAME_LEN];

void BT_SetHost(const BtDevice_t *dev)
{
    uint8_t i;

    if (dev == 0)
    {
        BT_ClearHost();
        return;
    }

    for (i = 0; i < 6u; i++)
    {
        s_host_addr[i] = dev->addr[i];
    }
    for (i = 0; i < (uint8_t)BT_NAME_LEN; i++)
    {
        s_host_name[i] = dev->name[i];
        if (dev->name[i] == '\0')
        {
            break;
        }
    }
    s_host_name[BT_NAME_LEN - 1] = '\0';
    s_host_valid = true;
}

void BT_SetHostSelected(void)
{
    if (s_n != 0u && s_sel < s_n)
    {
        BT_SetHost(&s_dev[s_sel]);
    }
}

void BT_ClearHost(void)
{
    uint8_t i;

    for (i = 0; i < 6u; i++) { s_host_addr[i] = 0u; }
    s_host_name[0] = '\0';
    s_host_valid = false;
}

const BtDevice_t *BT_GetHost(void)
{
    int8_t idx;

    if (!s_host_valid)
    {
        return 0;
    }

    idx = bt_find(s_host_addr);
    if (idx >= 0)
    {
        return &s_dev[idx];
    }

    /* 设备表里已经没有了（比如重新扫描过），就用记下来的名字造一个 */
    {
        static BtDevice_t tmp;
        uint8_t i;
        for (i = 0; i < 6u; i++) { tmp.addr[i] = s_host_addr[i]; }
        for (i = 0; i < (uint8_t)BT_NAME_LEN; i++) { tmp.name[i] = s_host_name[i]; }
        tmp.rssi = 127;
        tmp.addr_type = 0u;
        tmp.used = true;
        return &tmp;
    }
}

bool BT_IsHostAllowed(const uint8_t *addr)
{
    if (!s_host_valid)
    {
        return true;                    /* 没设白名单：谁连都接受 */
    }
    if (addr == 0)
    {
        return false;
    }
    return (memcmp(s_host_addr, addr, 6) == 0) ? true : false;
}
bool BT_IsConnected(void) { return (s_state == BT_STATE_CONNECTED) ? true : false; }

BtState_t BT_GetState(void) { return s_state; }

bool BT_SendHid(const uint8_t *report, uint8_t len)
{
    if (!BT_IsConnected() || s_be == 0 || s_be->send_hid == 0)
    {
        return false;
    }
    return s_be->send_hid(report, len);
}

/*==========================================================================
 * 初始化 / 轮询
 *========================================================================*/
void BT_Init(const BtBackend_t *backend)
{
    s_be = (backend != 0) ? backend : BT_GetMockBackend();

    BT_ClearDevices();
    s_qh = 0; s_qt = 0; s_qc = 0;
    s_scan_left = 0u;

    if (s_be->init != 0)
    {
        s_be->init();
    }
    s_state = BT_STATE_IDLE;
}

void BT_Task(uint16_t dt_ms)
{
    if (s_state == BT_STATE_OFF)
    {
        return;
    }

    if (s_be != 0 && s_be->task != 0)
    {
        s_be->task();
    }

    if (s_state == BT_STATE_SCANNING && s_scan_left > 0u)
    {
        if (dt_ms >= s_scan_left)
        {
            BT_StopScan();
            bt_push(BT_EV_SCAN_DONE);
        }
        else
        {
            s_scan_left = (uint16_t)(s_scan_left - dt_ms);
        }
    }
}

/*==========================================================================
 * 显示用的小工具
 *========================================================================*/
static char bt_hex(uint8_t v)
{
    return (v < 10u) ? (char)('0' + v) : (char)('A' + (v - 10u));
}

const char *BT_AddrString(const BtDevice_t *dev, char *buf, uint8_t buf_len)
{
    uint8_t i, p = 0u;

    if (dev == 0 || buf == 0 || buf_len < 18u)
    {
        return "";
    }

    for (i = 0; i < 6u; i++)
    {
        uint8_t v = dev->addr[5u - i];          /* 从高位开始显示 */
        buf[p++] = bt_hex((uint8_t)(v >> 4));
        buf[p++] = bt_hex((uint8_t)(v & 0x0Fu));
        if (i != 5u)
        {
            buf[p++] = ':';
        }
    }
    buf[p] = '\0';
    return buf;
}

const char *BT_DevNameOrAddr(const BtDevice_t *dev, char *buf, uint8_t buf_len)
{
    if (dev == 0)
    {
        return "";
    }
    if (dev->name[0] != '\0')
    {
        return dev->name;
    }
    return BT_AddrString(dev, buf, buf_len);
}

/*==========================================================================
 * 模拟后端：让上面所有逻辑在没有真蓝牙时也能跑、能自测
 *========================================================================*/
static uint32_t s_mock_hid_n;
static uint8_t  s_mock_hid_last[8];
static bool     s_mock_hid_ready;

static void mock_init(void)
{
    s_mock_hid_n = 0u;
    s_mock_hid_ready = true;
}

static void mock_task(void) { }

static void mock_scan_start(uint16_t ms) { (void)ms; }
static void mock_scan_stop(void) { }

static bool mock_connect(const BtDevice_t *dev)
{
    (void)dev;
    return true;                                /* 模拟后端总是"连得上" */
}

static void mock_disconnect(void) { }

static bool mock_send_hid(const uint8_t *report, uint8_t len)
{
    uint8_t i;

    if (report == 0 || len != 8u || !s_mock_hid_ready)
    {
        return false;
    }
    for (i = 0; i < 8u; i++)
    {
        s_mock_hid_last[i] = report[i];
    }
    s_mock_hid_n++;
    return true;
}

static const BtBackend_t s_mock_backend =
{
    mock_init,
    mock_task,
    mock_scan_start,
    mock_scan_stop,
    mock_connect,
    mock_disconnect,
    mock_send_hid
};

const BtBackend_t *BT_GetMockBackend(void) { return &s_mock_backend; }

void BT_MockAddDevice(const uint8_t *addr, int8_t rssi, const char *name)
{
    BT_OnDeviceFound(addr, 0u, rssi, name);
}

void BT_MockSetHidReady(bool ready) { s_mock_hid_ready = ready; }
uint32_t BT_MockHidCount(void) { return s_mock_hid_n; }
const uint8_t *BT_MockLastHid(void) { return s_mock_hid_last; }

/******************* (C) CH592X 蓝牙公共层 *****************/
