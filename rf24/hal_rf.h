/**************************************************************************
 * hal_rf.h —— 2.4G 射频硬件抽象层（HAL）
 *
 * 依据：规范 第 16.2 章「RF 驱动封装」
 *
 * 这一层把"怎么发一帧、怎么收一帧"和"上层协议怎么用"分开：
 *   * 真机后端：hal_rf_ch592.c —— 调 CH59x BLE 库的 RF_Config/RF_Tx/RF_Rx
 *   * PC  后端：tests\stub\hal_rf_sim.c —— 一块虚拟空口，两块虚拟板子对拖
 *
 * 所以协议层（proto_kb / proto_rc / link_*）**只 include 这个头文件**，
 * 在 PC 上就能完整跑通并自测。
 *
 * ------------------------------------------------------------------
 * 【CH592 与 nRF24 的差别，以及本层怎么抹平】
 * 规范正文（第 8、10 章）用的是一套 nRF24L01 风格的寄存器
 * （EN_AA / SETUP_RETR / RF_CH / TX_ADDR / RX_ADDR_P0 …），
 * 但 CH592 的 2.4G 是**BLE PHY 透传**，SDK 只给你：
 *     RF_Config(accessAddress, CRCInit, Channel, Frequency, LLEMode, cb, RxMaxlen)
 *     RF_Tx() / RF_Rx() / RF_Shut()
 * 硬件上没有"多管道地址过滤"，也没有硬件 Auto-ACK。
 *
 * 本层的映射：
 *   * 5 字节链路地址 → 取前 4 字节当 accessAddress（小端），
 *     第 5 字节留给软件（遥控模式里就是 dev_id）
 *   * "Auto-ACK / Auto-Retry" → LLE_MODE_AUTO（发完自动转收、收完自动回转）
 *     + 上层 link_reliability 的软件重传
 *   * 信道 / 速率 → Channel / LLEMode 的 PHY 位
 **************************************************************************/

#ifndef __HAL_RF_H__
#define __HAL_RF_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 速率（规范 2.1：键盘 2 Mbps，遥控 1 Mbps）
 *========================================================================*/
typedef enum
{
    RF_RATE_1M = 0,
    RF_RATE_2M = 1
} rf_rate_e;

/*==========================================================================
 * 回调事件
 *========================================================================*/
typedef enum
{
    HALRF_EVT_TX_DONE    = 1,   /* 发送成功                    */
    HALRF_EVT_TX_FAIL    = 2,   /* 发送失败（含超时/MAX_RT）   */
    HALRF_EVT_RX_DATA    = 3,   /* 收到数据                    */
    HALRF_EVT_RX_TIMEOUT = 4,   /* 收超时                      */
    HALRF_EVT_CRC_ERR    = 5    /* CRC 校验不过                */
} halrf_evt_t;

typedef void (*halrf_cb_t)(halrf_evt_t evt, int8_t rssi,
                           const uint8_t *buf, uint8_t len);

/*==========================================================================
 * 配置（规范 16.2）
 *========================================================================*/
typedef struct
{
    uint8_t   tx_addr[RF_ADDR_LEN];     /* 发送链路地址（5B）        */
    uint8_t   rx_addr[RF_ADDR_LEN];     /* 接收链路地址（5B）        */
    uint8_t   channel;                  /* 0..84                     */
    rf_rate_e rate;                     /* 1M / 2M                   */
    uint8_t   retry_cnt;                /* 0..15  软件重传次数       */
    uint8_t   retry_delay;              /* 0..15  单位 250 μs        */
    bool      crc16_en;                 /* true=CRC16, false=CRC8    */
    bool      auto_ack_en;              /* true=发完自动转收         */
    bool      dynamic_payload;          /* 保留（CH592 恒为变长）    */
} rf_config_t;

/*==========================================================================
 * 返回值
 *========================================================================*/
#define HALRF_OK            0       /* 成功                        */
#define HALRF_E_TIMEOUT   (-1)      /* 等超时                      */
#define HALRF_E_FAIL      (-2)      /* 硬件报失败                  */
#define HALRF_E_PARAM     (-3)      /* 参数不对                    */
#define HALRF_E_BUSY      (-4)      /* 上一次还没结束              */

/*==========================================================================
 * 初始化和配置
 *========================================================================*/

/*********************************************************************
 * @fn      HALRF_Init
 * @brief   配置并打开射频
 * @param   cfg  配置（不能为 0）
 *********************************************************************/
void HALRF_Init(const rf_config_t *cfg);

/*********************************************************************
 * @fn      HALRF_SetCallback
 * @brief   注册状态回调（在中断里被调用，别在里面干活，只置标志）
 *********************************************************************/
void HALRF_SetCallback(halrf_cb_t cb);

/*********************************************************************
 * @fn      HALRF_SetLink
 * @brief   切换链路地址 + 信道（遥控模式换时隙时用）
 * @note    CH592 上这一步要重写 accessAddress，比较贵；
 *          单地址方案（默认）只在开机调一次
 *********************************************************************/
void HALRF_SetLink(const uint8_t addr[RF_ADDR_LEN], uint8_t channel);

void HALRF_SetChannel(uint8_t ch);
void HALRF_SetRate(rf_rate_e rate);
void HALRF_SetTxAddr(const uint8_t addr[RF_ADDR_LEN]);
void HALRF_SetRxAddr(const uint8_t addr[RF_ADDR_LEN]);

uint8_t  HALRF_Channel(void);
rf_rate_e HALRF_Rate(void);

/*==========================================================================
 * 射频归属（PHY ownership）—— CH592 只有一套射频前端
 *
 * 背景：BLE 协议栈和 2.4G 私有协议（hal_rf_ch592.c）**共用同一个物理射频**。
 * BLE 建链时协议栈自己会把 LLE 配好（accessAddress / 频点 / CRC / 速率 /
 * rfStatusCB 回调）。此后任何一次 RF_Config() 都会把这套配置整体改写，
 * 协议栈随即失效（表现：能进 GAPROLE_ADVERTISING，但主机永远扫不到）。
 *
 * 所以谁在用射频，必须显式声明。约定：
 *   * HALRF_BtOwnsPhy(true)  —— BLE 独占。此后本模块所有会写 PHY 的调用
 *     （RF_Config / RF_SetFrequency / RF_Shut）**全部降级为"只写软件缓存"**，
 *     不再碰硬件；HALRF_Tx()/HALRF_RxStart() 直接返回 HALRF_E_FAIL。
 *   * HALRF_BtOwnsPhy(false) —— 把射频交还 2.4G。若此前已经缓存过配置，
 *     这里会补一次 RF_Config()，把 2.4G 的地址/频点真正装回去。
 *
 * 调用时机（本工程）见 User/Main.c 的 radio_set_phy_owner() / want_bt_phy()：
 *   * 进键盘模式时先判一次（挡住 UiKbdCh_Init()/rf_power_on() 那三次 RF_Config）
 *   * 主循环每圈按**通信仲裁的阶段**同步一次：
 *         还在探 2.4G 主机（SCAN_USB / SCAN_RF24）-> 交还 2.4G，要收主机 POLL
 *         SCAN_BLE 及以后                          -> 归 BLE（广播要靠这颗射频）
 *         （判据必须看阶段而不是"当前通道"：扫描期通道还是 NONE，
 *           只看通道会把 SCAN_BLE 也判成 2.4G，广播就再也起不来了）
 *   * 进遥控模式前交还 2.4G；由 2.4G 收回给 BLE 时，调用方必须补给
 *     协议栈一次 BT_WCH_RestorePhy()（RF_RoleInit），否则 LLE 还是 2.4G 的
 *========================================================================*/
void HALRF_BtOwnsPhy(bool on);

/* 当前射频是否归 BLE 协议栈（true = 2.4G 驱动处于"只写缓存"状态） */
bool HALRF_IsBtPhyOwner(void);

/*==========================================================================
 * 收发
 *========================================================================*/

/* 只发不管（异步，成功与否看回调） */
int HALRF_Tx(const uint8_t *buf, uint8_t len);

/* 发一帧并等发完（最多等 timeout_us）；返回 HALRF_OK / HALRF_E_* */
int HALRF_TxWait(const uint8_t *buf, uint8_t len, uint32_t timeout_us);

/* 只开收（异步） */
int HALRF_RxStart(void);

/* 开收并等一帧（最多等 timeout_us）
 * 收到返回 HALRF_OK，*len 给长度；超时返回 HALRF_E_TIMEOUT */
int HALRF_RxWait(uint8_t *buf, uint8_t *len, uint32_t timeout_us);

/*********************************************************************
 * @fn      HALRF_TxRx
 * @brief   发一帧，然后在 rx_window_us 内等对方的应答
 * @param   txbuf/txlen    要发的帧
 * @param   rxbuf/rxlen    收到的应答（可以是 0，表示不关心）
 * @param   rx_window_us   等应答的窗口（键盘 150 / 遥控 400）
 * @return  HALRF_OK=收到应答；HALRF_E_TIMEOUT=没收到
 * @note    CH592 上走 LLE_MODE_AUTO：RF_Tx 之后硬件自动转收
 *********************************************************************/
int HALRF_TxRx(const uint8_t *txbuf, uint8_t txlen,
               uint8_t *rxbuf, uint8_t *rxlen, uint32_t rx_window_us);

/*==========================================================================
 * 电源 / 复位
 *========================================================================*/
void HALRF_Shut(void);          /* 关射频，回 idle */
void HALRF_FlushTx(void);
void HALRF_FlushRx(void);
void HALRF_PowerDown(void);
void HALRF_PowerUp(void);

/*==========================================================================
 * 信号强度
 *========================================================================*/
int8_t HALRF_ReadRssi(void);

/*==========================================================================
 * 时间和 CRC（PC 上由测试控制，用来做确定性时序验证）
 *========================================================================*/
uint32_t HALRF_Micros(void);
void     HALRF_DelayUs(uint32_t us);

uint8_t  HALRF_Crc8(const uint8_t *d, uint16_t n);
uint16_t HALRF_Crc16(const uint8_t *d, uint16_t n);

/*==========================================================================
 * 上层放置回调数据的缓冲（由后端提供，避免动态分配）
 *========================================================================*/
typedef struct
{
    uint8_t  buf[RF_PLOAD_MAX];
    /* 【修复 P2-2】len/rssi 由 RF 中断回调写、主循环读，必须 volatile，
     * 否则编译器可能把它们缓存在寄存器里，等 done 变 1 后读到旧值。 */
    volatile uint8_t len;
    volatile int8_t  rssi;
    volatile uint8_t busy;      /* 正在发/收 */
    volatile uint8_t done;      /* 上一次操作结束 */
    volatile uint8_t ok;        /* 上一次操作成功 */
} halrf_ctx_t;

extern halrf_ctx_t g_halrf;

#ifdef __cplusplus
}
#endif

#endif /* __HAL_RF_H__ */
