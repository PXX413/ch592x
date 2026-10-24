/**************************************************************************
 * bt_wch.h —— CH592X BLE 从设备（外设角色，广播名 CH592X）
 *
 * 这一层是 WCH 协议栈的"接线盒"：
 *     BT_WCH_Init()    起协议栈 + 设置广播数据/设备名 + 开始广播
 *     BT_WCH_Process() 主循环里反复调用（跑 TMOS 任务调度）
 * 上层（bt_port 的设备表/白名单、bt_hid 的键盘报文）不用管协议栈细节。
 **************************************************************************/

#ifndef __BT_WCH_H__
#define __BT_WCH_H__

#include <stdint.h>
#include <stdbool.h>
#include "bt_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 广播名（要改就改这里，最长 GAP_DEVICE_NAME_LEN） */
#define BT_WCH_NAME         "CH592X"

/* 起 BLE 协议栈：初始化 + 开始广播（从设备） */
void BT_WCH_Init(void);

/* 【2026-10-14】诊断用：最近一次断开原因码 / 实际连接参数 / 参数更新应答 */
uint8_t  BT_WCH_LastDiscReason(void);
uint8_t  BT_WCH_ConnInterval(void);
uint16_t BT_WCH_ConnTimeout(void);
uint8_t  BT_WCH_ParamUpdStatus(void);

/* 主循环里反复调用，别阻塞太久（TMOS 要按时跑） */
void BT_WCH_Process(void);

/* 初始化进度：1~6 对应 BLEInit/HALInit/Task/GAP/StartDev/AdvOn */
extern volatile uint8_t g_bt_step;
extern uint8_t g_bt_addr[6];
void BT_WCH_SetStep(uint8_t s);

/* 诊断打点（只读）。20/21/22/23 = 连接建立路径的四步，6 = 收到断链，
 * 7 = 设备启动完成，1~5 = 初始化各步。 */
extern volatile uint8_t  g_bt_diag_step;
extern volatile uint32_t g_bt_diag_tick;

/*********************************************************************
 * @fn      BT_WCH_DrawDiag
 * @brief   把诊断打点画到屏幕底部（主循环里周期调用）
 * @note    **只能在主循环里调。** 它做 SPI 刷屏（约 17ms），
 *         绝不能在 GAP/TMOS 协议栈回调里调 —— 那会阻塞协议栈
 *         十几个连接事件，直接把刚建立的链路弄断。
 *
 *         编译开关 BT_DIAG_DRAW 默认 0，本函数此时是**空函数**（几条
 *         栈指令），完全不影响运行。需要看屏上的诊断行就把它改成 1。
 *         打点变量 g_bt_diag_step / g_bt_diag_tick 始终有效，
 *         用调试器 watch 即可，不需要刷屏。
 *********************************************************************/
void BT_WCH_DrawDiag(void);

bool        BT_WCH_IsAdvertising(void);
bool        BT_WCH_IsConnected(void);

/* 【2026-10-14】清掉设备侧所有绑定信息。
 * 设备端 SNV 与主机侧密钥不一致时（反复烧录整片擦除后常见）用它救场：
 * 设备端清空 + 主机端『忘记设备』，两端回到干净状态。 */
void        BT_WCH_EraseAllBonds(void);

/*********************************************************************
 * @fn      BT_WCH_SendHid
 * @brief   把 8 字节 boot 报文推给已连接的主机（走 GATT 通知）
 * @param   report  至少 8 字节
 * @param   len     长度（至少 8）
 * @return  true = 交给了协议栈
 * @note    对端没订阅（CCCD 未使能）或未连接时返回 false，
 *          这是正常现象，上层每帧重试即可。
 *********************************************************************/
bool        BT_WCH_SendHid(const uint8_t *report, uint8_t len);
uint16_t    BT_WCH_GetConnHandle(void);
const char *BT_WCH_GetName(void);

void BT_WCH_StartAdvertising(void);
void BT_WCH_StopAdvertising(void);

/*********************************************************************
 * @fn      BT_WCH_RestorePhy
 * @brief   把射频从 2.4G 参数交还 BLE 协议栈，并重新拉起广播
 * @note    RF_RoleInit() 会按协议栈自己的配置重写 LLE（地址/信道/CRC/长度），
 *          所以**调用点只调这一个函数就够了**：内部已经把广播"先清后开"
 *          走完，不需要外面再补一次 BT_WCH_StartAdvertising()。
 *********************************************************************/
void BT_WCH_RestorePhy(void);

#ifdef __cplusplus
}
#endif

#endif /* __BT_WCH_H__ */
