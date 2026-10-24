/**************************************************************************
 * bt_port.h —— CH592X 蓝牙公共层（设备表 / 扫描 / 选择 / 连接 / HID 发送）
 *
 * 为什么要分两层：
 *   CH592 的蓝牙是 WCH 提供的协议栈**库**（LIBCH59xBLE.a + 一堆头文件），
 *   而"设备表怎么维护、界面怎么显示、编码器怎么选、HID 报文怎么拼"这些逻辑
 *   跟协议栈无关，放在这里；真正的协议栈调用放在一个"后端"里：
 *
 *        bt_port.c     本文件实现：设备表、扫描状态机、选择、连接、事件队列
 *        bt_hid.c      标准 BLE HID 键盘报文（8 字节 boot 报文）
 *        bt_screen.c   屏幕上"附近蓝牙设备"列表 + 编码器选择
 *        bt_wch.c      【待补】真正调用 WCH BLE 库的后端（等拿到 EVT 包）
 *
 *   没有真实协议栈时，用内置的"模拟后端"就能在 PC 上把上面三层全部自测通过。
 *
 * 典型用法（主循环）：
 *      BT_Init(&bt_wch_backend);        // 真机；PC 自测传 0 用模拟后端
 *      BT_StartScan();
 *      while (1)
 *      {
 *          Game_Task();                 // 键盘/编码器
 *          BT_Task(GAME_FRAME_MS);      // 蓝牙状态推进（含协议栈处理）
 *          ...界面...
 *      }
 **************************************************************************/

#ifndef __BT_PORT_H__
#define __BT_PORT_H__

#include <stdint.h>
#include <stdbool.h>
#include "CH59x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 配置
 *========================================================================*/
#ifndef BT_MAX_DEV
#define BT_MAX_DEV          4       /* 最多记住几个设备 */
#endif
#ifndef BT_NAME_LEN
#define BT_NAME_LEN         16      /* 设备名最多 15 个字符 + 结尾 0 */
#endif
#ifndef BT_EVENT_QUEUE
#define BT_EVENT_QUEUE      8
#endif
#ifndef BT_SCAN_MS
#define BT_SCAN_MS          5000u   /* 一次扫描持续多久 */
#endif

/*==========================================================================
 * 2. 设备 / 状态 / 事件
 *========================================================================*/
typedef struct
{
    uint8_t  addr[6];                   /* 蓝牙地址，低位在前 */
    uint8_t  addr_type;                 /* 0 = public，1 = random */
    int8_t   rssi;                      /* 信号强度 dBm（127 = 未知） */
    char     name[BT_NAME_LEN];         /* 设备名（可能为空串） */
    bool     used;
} BtDevice_t;

typedef enum
{
    BT_STATE_OFF = 0,                   /* 还没初始化 */
    BT_STATE_IDLE,                      /* 初始化好了，没扫描 */
    BT_STATE_SCANNING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED
} BtState_t;

typedef enum
{
    BT_EV_NONE = 0,
    BT_EV_DEVICE_FOUND,                 /* 发现/更新了一个设备 */
    BT_EV_SCAN_DONE,
    BT_EV_CONNECTED,
    BT_EV_DISCONNECTED,
    BT_EV_HID_READY                     /* 对端已订阅键盘报文，可以发了 */
} BtEventType_t;

/*==========================================================================
 * 3. 后端接口（真正的协议栈 / 模拟实现）
 *========================================================================*/
typedef struct
{
    void (*init)(void);
    void (*task)(void);                             /* 主循环里轮询（协议栈处理） */
    void (*scan_start)(uint16_t ms);
    void (*scan_stop)(void);
    bool (*connect)(const BtDevice_t *dev);
    void (*disconnect)(void);
    bool (*send_hid)(const uint8_t *report, uint8_t len);
} BtBackend_t;

/*==========================================================================
 * 4. 接口
 *========================================================================*/
/*********************************************************************
 * @fn      BT_Init
 * @brief   初始化蓝牙层
 * @param   backend  真正的协议栈后端；传 0 = 使用内置模拟后端（PC 自测用）
 *********************************************************************/
void BT_Init(const BtBackend_t *backend);

/* 主循环里调用：推进超时、协议栈轮询、扫描自动结束 */
void BT_Task(uint16_t dt_ms);

BtState_t BT_GetState(void);
bool      BT_IsConnected(void);

/* ---- 扫描 ---- */
bool BT_StartScan(void);
void BT_StopScan(void);
bool BT_IsScanning(void);

/* ---- 设备表 ---- */
uint8_t          BT_GetDeviceCount(void);
const BtDevice_t *BT_GetDevice(uint8_t i);
void             BT_ClearDevices(void);

/* ---- 选择（编码器上下选） ---- */
uint8_t BT_GetSelected(void);
void    BT_SetSelected(uint8_t i);
void    BT_SelectNext(int8_t dir);          /* +1 下一个 / -1 上一个，循环 */

/* ---- 连接 ---- */
bool              BT_ConnectSelected(void);
void              BT_Disconnect(void);
const BtDevice_t *BT_GetConnectedDevice(void);
/* ---- 宿主白名单：只允许这个设备来连我们（不主动连出去） ---- */
void              BT_SetHost(const BtDevice_t *dev);        /* 记下允许连接的宿主 */
void              BT_SetHostSelected(void);                 /* 把当前选中的设备设为宿主 */
void              BT_ClearHost(void);
const BtDevice_t *BT_GetHost(void);                         /* 0 = 还没设 */
bool              BT_IsHostAllowed(const uint8_t *addr);    /* 没设白名单时谁都能连 */

/* ---- 事件（给上层界面用） ---- */
bool BT_PollEvent(BtEventType_t *ev);

/* ---- 发送键盘报文（内部会调后端；没连接时返回 false） ---- */
bool BT_SendHid(const uint8_t *report, uint8_t len);

/* ---- 显示用的小工具 ---- */
const char *BT_AddrString(const BtDevice_t *dev, char *buf, uint8_t buf_len);
const char *BT_DevNameOrAddr(const BtDevice_t *dev, char *buf, uint8_t buf_len);

/*==========================================================================
 * 5. 协议栈回调（bt_wch.c 收到事件时调用这些）
 *========================================================================*/
void BT_OnDeviceFound(const uint8_t *addr, uint8_t addr_type, int8_t rssi, const char *name);
void BT_OnScanDone(void);
void BT_OnConnected(const uint8_t *addr);
void BT_OnDisconnected(void);
void BT_OnHidReady(void);

/*==========================================================================
 * 6. 模拟后端（PC 自测 / 没有真蓝牙时用）
 *========================================================================*/
const BtBackend_t *BT_GetMockBackend(void);
void BT_MockAddDevice(const uint8_t *addr, int8_t rssi, const char *name);
void BT_MockSetHidReady(bool ready);
uint32_t BT_MockHidCount(void);                 /* 后端一共发过几份报文 */
const uint8_t *BT_MockLastHid(void);            /* 最后一份报文（8 字节） */

#ifdef __cplusplus
}
#endif

#endif /* __BT_PORT_H__ */
