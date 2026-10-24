/**************************************************************************
 * proto_rc.h —— 遥控模式协议（PROTO_RC）
 *
 * 依据：规范 第 5 章（TDMA 超帧）、第 6 章（9 键名称）、第 12 章（状态机）、
 *        第 16.5 节（超帧调度器）、附录 D.2 / E / F
 *
 *   主机 CH592X：周期 16 ms，20 个 0.8 ms 时隙
 *        Slot 0  SYNC（广播）
 *        1..16   各从机 CMD + REPORT
 *        17      枚举窗口
 *        18      大块数据 / 紧急上报
 *        19      保留
 *
 *   从机 CH570Q：上电 dev_id=0，监听枚举地址；被分配 dev_id 后只在自己的
 *        时隙醒来，收 CMD → 回 REPORT（或 KEYMAP 分片）。
 *
 * ------------------------------------------------------------------
 * 【本实现补掉的规范缺口】
 *   1) 6.6 的时序图里有 ENUM_REQ 帧，但 13.2 的帧表里根本没有这个类型。
 *      实际能用的只有 SYNC.flags.bit0 = enum_open —— 本实现就用它当 ENUM_REQ。
 *   2) ENUM_ACK（0x13）里只有 dev_id + tx_addr[5]，没有能标识"这一帧是给
 *      谁的"的东西。在 CH592 这种"只有一个 accessAddress、没有管道地址过滤"
 *      的 PHY 上，所有还没枚举的从机都会收到它。本实现把 ACK 的第 7 字节
 *      （规范里是保留/填充）拿来放 hw_id 的低字节，从机对一下才认领。
 *   3) 规范 2.4 要求 16 个从机用 16 个不同的 RF 地址。CH592 换一次
 *      accessAddress 要重写 RF_Config，16 个时隙就是 16 次，代价不小。
 *      本实现默认走**单地址方案**（RF24_RC_PER_DEVICE_ADDR=0）：所有从机
 *      共用一个 accessAddress，靠 TDMA 时隙 + 帧里的 dev_id 区分。想按
 *      规范原样跑，把那个宏改成 1 即可。
 **************************************************************************/

#ifndef __PROTO_RC_H__
#define __PROTO_RC_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"
#include "link_hop.h"
#include "link_tdma.h"
#include "link_keymap.h"
#include "link_reliability.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 容量（SRAM 预算见 README）
 *    rc_dev_t 一个约 260 字节，6 个约 1.6KB
 *========================================================================*/
#ifndef RC_HOST_MAX_DEV
/* 【P2 修复】6 -> 4。与 __stack_size 640->1024、RC_KEYMAP_RX_SLOTS 2->1
 * 三者配套，否则 RAM 装不下（详见 Ld/Link.ld 顶部的账）。
 *
 * 背景：用 objdump 实测（不是 -fstack-usage，那个数字不可信）：
 *     main 最深调用链 688 B（main 80 + radio_task 16 + BT_WCH_Init 48
 *         + CH59x_BLEInit 96 + iprintf 64 + _vfiprintf_r 176 + ... + _fstat_r 16）
 *     LLE_IRQHandler 128 B（BLE 链路层中断，会抢占地主线程的栈）
 * 也就是**每次开机**走到 CH59x_BLEInit() 里的 puts/iprintf 时就要 688 B，
 * 原 640 B 栈从开机第一秒起就溢出了 48 B。
 * 全工程最大单帧 288 B（Lib_Write_Flash_592A），
 * 中断入口本身很便宜（TMR0/USB/HardFault 都是 0 B，LLE 128 B），
 * 且没有任何 handler 写 mstatus -> 中断不嵌套，不需要为嵌套预留。
 *
 * RAM 顶端可用 = 0x20006800 - _ebss = 2720 B（栈 + 堆共用）。
 * 取栈 1024 后堆只剩 1696 B，必须装得下 rc_host_t + 32。
 * rc_host_t 的大头是 dev[]，每个 rc_host_dev_t 约 250 B（9 键名称表）。
 * 6 设备 + 2 槽 = 2312 B（装不下，遥控模式本来就申请失败）
 * 4 设备 + 1 槽 = 1596 B（+32 = 1628 <= 1696，余 68 B）
 *
 * 代价：遥控主机同时记住 4 个从机而不是 6 个。UI 的 UIRC_DEV_MAX 仍是 6，
 * 只是主机侧列表变短，不会越界；单设备使用完全无感。
 * 想恢复 6 设备，必须同步把 __stack_size 降回 640 以下，
 * 或改小 RC_KEY_NAME_MAX（会动到 2.4G 协议的 TLV 格式，主机从机都要改）。 */
#define RC_HOST_MAX_DEV         4u
#endif
#ifndef RC_KEYMAP_RX_SLOTS
/* 【P2 修复】2 -> 1，与 Ld/Link.ld 的 __stack_size 640->1024 配套（方案 A）。
 *
 * 背景：CH592X 的 WCH 快速中断每个入口固定吃 256 B 栈（自动保存浮点上下文，
 * fsd ft0..fs7 共 224 B，-fstack-usage 看不到，只能 objdump 实测）。
 * 两级中断嵌套就要 512 B，原 640 B 栈在 BLE 连接瞬间必溢出错。
 * 栈加到 1024 后堆只剩 2040 B，装不下 SLOTS=2 的 rc_host_t。
 *
 * 代价：主机只能同时重组 1 个从机的按键名称。实际单设备使用完全无感；
 * 真要从两个从机同时上传名称，第二个会被挤掉（槽位被占），
 * 由 RC_KEYMAP_MISS 机制重传补回，最坏表现是"改名后偶发不生效"，
 * 重新进一次遥控页即可恢复，不会死机。
 * 每个 keymap_rx_t 约 224 B（tlv[RC_KEYMAP_TLV_MAX=207] + 17 B 头）。
 * 对多从机并发改名有强需求的，可改回 2，但必须同步把 __stack_size 降回
 * 640 或从 .bss 里让出至少 224 B，否则堆装不下 rc_host_t。 */
#define RC_KEYMAP_RX_SLOTS      1u
#endif

/* 每个从机用不同 accessAddress 吗（规范原意 = 1，省事 = 0） */
#ifndef RF24_RC_PER_DEVICE_ADDR
#define RF24_RC_PER_DEVICE_ADDR 0
#endif

/* 枚举时从机随机退避的档数（0..N-1，每档 150 μs） */
#define RC_ENUM_BACKOFF_STEPS   4u
#define RC_ENUM_BACKOFF_US      150u

/*==========================================================================
 * 2. ENUM_RSP.caps 位（规范 5.3.4）
 *========================================================================*/
#define RC_CAP_HAS_9KEY         (1u << 0)
#define RC_CAP_HAS_BATT         (1u << 1)
#define RC_CAP_HAS_LED          (1u << 2)

/* ENUM_ACK 第 7 字节 = 目标设备 hw_id 的低字节 */
#define RC_ENUM_ACK_OFF_HWID    7u

/*==========================================================================
 * 3. 主机侧
 *========================================================================*/
typedef enum
{
    RC_HOST_SCAN        = 0,    /* 扫枚举信道                    */
    RC_HOST_ENUM_WAIT   = 1,    /* 等 ENUM_RSP                   */
    RC_HOST_RUNNING     = 3,    /* 正常 TDMA 超帧                */
    RC_HOST_RESYNC      = 4     /* 全量重枚举                    */
} rc_host_state_t;

/* 从机在上传名称的哪个阶段（主机侧看） */
typedef enum
{
    RC_DEV_ENUMING  = 0,
    RC_DEV_READY    = 2,
    RC_DEV_OFFLINE  = 3
} rc_dev_phase_t;

typedef struct
{
    uint8_t  used;
    uint8_t  dev_id;
    uint8_t  phase;                 /* rc_dev_phase_t               */
    uint8_t  miss_cnt;
    uint32_t last_seen_ms;

    /* ---- ENUM_RSP 带回来的 ---- */
    uint8_t  hw_id[4];
    uint8_t  hw_ver;
    uint8_t  sw_ver;
    uint8_t  caps;
    uint16_t key_mask;              /* bit0..8 = K1..K9 物理存在 */
    int8_t   rssi;

    /* ---- REPORT 带回来的 ---- */
    /* ---- ENUM_RSP 带回的设备名（主机靠它查动作编号表）---- */
    uint8_t  dev_name_len;
    char     dev_name[RC_DEV_NAME_MAX + 1u];

    uint8_t  status;
    uint8_t  adc_batt;
    uint16_t batt_mv;

} rc_host_dev_t;

/* 待发帧队列（每个 dev_id 最多一帧，下一轮它的时隙发出去）
 *
 * 注意：主机在从机的时隙里**只能发一帧**，而协议里 Host→Dev 的帧不止
 * RC_CMD 一种（还有 RC_KEYMAP_MISS / RC_KEYMAP_ACK / RC_RENAME_KEY）。
 * 所以队列里存的是"一整帧"，而不是只存一个命令码。
 */
typedef struct
{
    uint8_t  valid;
    uint8_t  len;
    uint8_t  retry;
    uint8_t  data[RF_MTU];          /* 完整一帧，data[0] 是 frame_ctrl */
} rc_cmd_t;

typedef struct
{
    rc_host_state_t state;
    tdma_t          tdma;
    hop_ctx_t       hop;
    link_stat_t     stat;

    rc_host_dev_t   dev[RC_HOST_MAX_DEV];       /* 设备表             */
    /* 注意：这两个数组是**按协议 dev_id 直接索引**的，而 dev_id 的空间是
     * 1..RF_RC_MAX_DEV（协议定死的，从机侧也一样），不是 RC_HOST_MAX_DEV。
     * RC_HOST_MAX_DEV 只限制"主机同时记住几个从机的状态"，是另一回事。
     * 曾经按 RC_HOST_MAX_DEV 定尺，结果 dev_id=5/6 的从机一上报就越界写
     * 隔壁成员，编译器还会报 -Waggressive-loop-optimizations。 */
    rc_cmd_t        cmdq[RF_RC_MAX_DEV + 1u];     /* 命令队列（按 dev_id）*/

    uint8_t         rc_addr[RF_ADDR_LEN];       /* 本网 accessAddress */
    uint8_t         enum_addr[RF_ADDR_LEN];
    uint8_t         pair_seed;
    uint32_t        ms;                         /* 1ms 计数           */
    uint32_t        sf_total;
    uint16_t        last_rpt_ms[RF_RC_MAX_DEV + 1u]; /* 按 dev_id 索引，同上 */
    uint8_t         resync_flag;
} rc_host_t;

void Rc_HostInit(rc_host_t *h, uint8_t pair_seed);

/* 1ms 时钟（看门狗、超时用） */
void Rc_HostTickMs(rc_host_t *h);

/*********************************************************************
 * @fn      Rc_HostSuperframe
 * @brief   跑完整的一个超帧（16 ms，内部会阻塞到时隙边界）
 * @note    调用方在大循环里不停调它；它自己管时隙和跳频
 *********************************************************************/
void Rc_HostSuperframe(rc_host_t *h);

/* ---- 设备表查询 ---- */
uint8_t        Rc_HostCount(const rc_host_t *h);            /* 已枚举到几个 */
rc_host_dev_t *Rc_HostDev(rc_host_t *h, uint8_t idx);       /* 0..count-1    */
rc_host_dev_t *Rc_HostFind(rc_host_t *h, uint8_t dev_id);
rc_host_state_t Rc_HostState(const rc_host_t *h);

/*********************************************************************
 * @fn      Rc_HostSendCmd
 * @brief   给某个从机排一条通用命令（RC_CMD），下一轮它的时隙发出去
 * @return  true = 排上了
 *********************************************************************/
bool Rc_HostSendCmd(rc_host_t *h, uint8_t dev_id, uint8_t cmd,
                    const uint8_t *param, uint8_t len);

/*********************************************************************
 * @fn      Rc_HostSendFrame
 * @brief   给某个从机排一整帧（可以是 RC_KEYMAP_MISS / ACK / RENAME_KEY）
 * @param   frame  完整帧（frame_ctrl 打头）
 * @param   len    帧长（<= 32）
 *********************************************************************/
bool Rc_HostSendFrame(rc_host_t *h, uint8_t dev_id, const uint8_t *frame, uint8_t len);

/* 认领一个刚上电的从机……（测试/上层可直接摆设备表，一般不用手动调） */
void Rc_HostClear(rc_host_t *h);

/*==========================================================================
 * 4. 帧组包 / 解包（主机从机共用，PC 上可以单独测）
 *========================================================================*/
void    Rc_BuildSync(uint8_t *out8, uint16_t sf_id, uint16_t slot_bitmap,
                     uint8_t hop_channel, uint8_t lfsr_seed, uint8_t flags);
uint8_t Rc_ParseSync(const uint8_t *in8, uint16_t *sf_id, uint16_t *slot_bitmap,
                     uint8_t *hop_channel, uint8_t *lfsr_seed, uint8_t *flags);

void    Rc_BuildCmd(uint8_t *out8, uint8_t dev_id, uint8_t cmd,
                    const uint8_t *param, uint8_t len);
uint8_t Rc_ParseCmd(const uint8_t *in8, uint8_t *dev_id, uint8_t *cmd,
                    uint8_t *param, uint8_t *len);

void    Rc_BuildEnumAck(uint8_t *out8, uint8_t dev_id, const uint8_t addr[RF_ADDR_LEN],
                        uint8_t hw_id_low);
uint8_t Rc_ParseEnumAck(const uint8_t *in8, uint8_t *dev_id,
                        uint8_t addr[RF_ADDR_LEN], uint8_t *hw_id_low);

void    Rc_BuildReport(uint8_t *out8, uint8_t status, uint16_t key_state,
                       uint8_t key_edge, uint8_t key_action,
                       uint8_t adc_batt, uint8_t config_ver);
uint8_t Rc_ParseReport(const uint8_t *in8, uint8_t *status, uint16_t *key_state,
                       uint8_t *key_edge, uint8_t *key_action,
                       uint8_t *adc_batt, uint8_t *config_ver);



/*==========================================================================
 * 5. 从机侧（规范 12.2 / 附录 E）
 *========================================================================*/
typedef enum
{
    RC_DEV_IDLE        = 0,     /* dev_id=0，监听枚举地址       */
    RC_DEV_ENUM_SENT   = 1,     /* ENUM_RSP 发出去了，等 ACK    */
    RC_DEV_KEYMAP_SEND = 2,     /* 正在传 9 键名称              */
    RC_DEV_RUNNING     = 3,     /* TDMA 正常跑                  */
    RC_DEV_RESYNC      = 4,     /* 扫 8 频点找 SYNC             */
    RC_DEV_SLEEP       = 5
} rc_dev_state_t;

typedef struct
{
    rc_dev_state_t state;
    rc_keymap_t    km;                      /* 本机 9 键配置        */

    uint8_t        dev_id;                  /* 0 = 还没被分配       */
    uint8_t        tx_addr[RF_ADDR_LEN];
    uint8_t        pair_seed;
    uint8_t        channel;                 /* 当前频点             */

    /* 身份（烧录时写入 Flash 的 ChipID / 版本号） */
    uint8_t        hw_id[4];
    uint8_t        hw_ver;
    uint8_t        sw_ver;

    /* 按键状态（由上层按键扫描喂进来） */
    uint16_t       key_state;
    uint16_t       key_last;
    uint8_t        key_edge;
    uint8_t        adc_batt;
    uint8_t        batt_low;

    /* 名称分片 */
    uint8_t        tlv[RC_KEYMAP_TLV_MAX];
    uint16_t       tlv_len;
    uint8_t        frag_total;
    uint8_t        frag_next;
    uint8_t        frag_resent_mask;

    /* 链路 */
    uint8_t        miss_sf;                 /* 连续几个超帧没听到 SYNC */
    uint32_t       last_sync_ms;
    uint32_t       sf_start_us;
    uint16_t       sf_id;
    uint8_t        hop_index;               /* 本超帧的跳频点下标（来自 SYNC）*/
    uint8_t        backoff;                 /* 枚举退避档位         */
    uint32_t       key_evt_cnt;
} rc_dev_t;

void Rc_DevInit(rc_dev_t *d, const rc_keymap_t *km, uint8_t pair_seed);

/* 设置身份信息（hw_id 一般是芯片 UID/ChipID） */
void Rc_DevSetIdentity(rc_dev_t *d, const uint8_t hw_id[4], uint8_t hw_ver, uint8_t sw_ver);

/* 枚举退避：听到 SYNC(enum_open) 之后该等多少微秒再回 ENUM_RSP */
uint32_t Rc_DevEnumBackoffUs(const rc_dev_t *d);

/* 喂按键状态（bit0..8 = K1..K9） */
void Rc_DevSetKeys(rc_dev_t *d, uint16_t key_state);

/* 电池 ADC（高 8 位） */
void Rc_DevSetBattery(rc_dev_t *d, uint8_t adc_batt, bool low);

/* 组装 ENUM_RSP（32B）；hw_id / 版本号用 Rc_DevSetIdentity 设过的 */
uint8_t Rc_DevBuildEnumRsp(const rc_dev_t *d, uint8_t *out32);

/*********************************************************************
 * @fn      Rc_DevOnSync
 * @brief   从机收到 SYNC
 * @return  true = 需要立刻回 ENUM_RSP（枚举窗口 + 退避轮到我了）
 *********************************************************************/
bool Rc_DevOnSync(rc_dev_t *d, const uint8_t *sync8, uint32_t now_us, uint32_t now_ms);

/*********************************************************************
 * @fn      Rc_DevServeSlot
 * @brief   从机在"自己的时隙"里服务一次：收 CMD → 回帧
 * @param   out32/outlen   要回出去的帧
 * @return  true = 有帧要发
 * @note    这是从机侧唯一碰射频的地方；其余时间上层应该让它睡觉。
 *********************************************************************/
bool Rc_DevServeSlot(rc_dev_t *d, const uint8_t *cmdBuf, uint8_t cmdLen,
                     uint8_t *out32, uint8_t *outlen);

/* 组装 REPORT（8B），顺便做 key_edge 边沿检测 */
void Rc_DevBuildReport(rc_dev_t *d, uint8_t *out8);

/* 收到 ENUM_ACK */
uint8_t Rc_DevOnEnumAck(rc_dev_t *d, const uint8_t *ack8);

/* 收到 KEYMAP_ACK / KEYMAP_MISS 后该重发哪几片 */

/* 收到 CMD_GET_KEYMAP：重头再传一遍 */

/* 1ms 时钟：超时就把链路降级 */
void Rc_DevTickMs(rc_dev_t *d, uint32_t now_ms);

/* 从机该用哪个频点（跟着 SYNC 走） */
void Rc_DevSetChannel(rc_dev_t *d, uint8_t ch);

#ifdef __cplusplus
}
#endif

#endif /* __PROTO_RC_H__ */
