/**************************************************************************
 * proto_kb.h —— 键盘模式协议（PROTO_KB）
 *
 * 依据：规范 第 4 章、第 13.1 节、附录 D.1、第 15.1 章
 *
 *   拓扑 1:1，主机每 1 ms 发一个 POLL（5B），从机在 150 μs 窗口内回
 *   HID 报告（6KRO 8B / NKRO 32B）。
 *   跳频 3 个频点，每 32 帧切一次；丢帧用 KB_RETRIEVE 补。
 *
 * 【本实现修掉的规范缺陷】
 *   4.6 的伪代码写 `kb_hop_tbl[(seq >> 5) & 0x03]`，但表只有 3 个元素
 *   （下标 0..2），& 0x03 会算出 3 → 数组越界。本实现改成 `% 3`。
 **************************************************************************/

#ifndef __PROTO_KB_H__
#define __PROTO_KB_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"
#include "link_hop.h"
#include "link_reliability.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. HID 报告（规范 15.1）
 *========================================================================*/
#define HID_6KRO_KEYS           6u
#define HID_NKRO_BYTES          28u         /* 224 bit = 224 个 HID 键码 */

typedef struct
{
    uint8_t modifiers;
    uint8_t keys[HID_6KRO_KEYS];            /* 6KRO 模式用            */
    uint8_t nkro[HID_NKRO_BYTES];           /* NKRO 模式用            */
} hid_report_t;

/* 修饰键位（标准 HID boot 报文的 byte2） */
#define HID_MOD_LCTRL           0x01u
#define HID_MOD_LSHIFT          0x02u
#define HID_MOD_LALT            0x04u
#define HID_MOD_LGUI            0x08u
#define HID_MOD_RCTRL           0x10u
#define HID_MOD_RSHIFT          0x20u
#define HID_MOD_RALT            0x40u
#define HID_MOD_RGUI            0x80u

/* 常被用到的 HID 键码 */
#define HID_KEY_ENTER           0x28u
#define HID_KEY_ESC             0x29u
#define HID_KEY_BACKSPACE       0x2Au
#define HID_KEY_VOL_UP          0x80u
#define HID_KEY_VOL_DOWN        0x81u

/* 某个 HID 键码在 NKRO 位图里的操作 */
void    Hid_NkroSet(hid_report_t *r, uint8_t usage, bool down);
bool    Hid_NkroTest(const hid_report_t *r, uint8_t usage);
void    Hid_6KroSet(hid_report_t *r, uint8_t usage, bool down);
bool    Hid_6KroTest(const hid_report_t *r, uint8_t usage);
void    Hid_Clear(hid_report_t *r);
bool    Hid_IsEmpty(const hid_report_t *r);

/*==========================================================================
 * 2. 配对信息（规范 4.3.3 / 15.1）
 *========================================================================*/
typedef struct
{
    uint8_t  addr[RF_ADDR_LEN];
    uint8_t  channel;
    uint8_t  hop[HOP_KB_COUNT];
    bool     nkro_en;
} kb_pair_info_t;

void Kb_PairDefault(kb_pair_info_t *p);

/*********************************************************************
 * @fn      Kb_BuildPair
 * @brief   组一个 PAIR 帧（16B）
 * @param   stage  KB_PAIR_REQ / RESP / CONFIRM / COMPLETE
 * @param   rnd    4 字节防重放随机数（防止中间人重放）
 *********************************************************************/
void Kb_BuildPair(uint8_t *out16, uint8_t stage, const kb_pair_info_t *p, uint32_t rnd);

/*********************************************************************
 * @fn      Kb_ParsePair
 * @brief   解析 PAIR 帧
 * @return  RC_OK / RC_E_PARAM / RC_E_CRC
 *********************************************************************/
uint8_t Kb_ParsePair(const uint8_t *in16, uint8_t *stage, kb_pair_info_t *p, uint32_t *rnd);

/*==========================================================================
 * 3. 帧组包 / 解包
 *========================================================================*/
/* POLL 帧固定 5 字节：frame_ctrl + seq + host_ts_l + link_quality + flags */
#ifndef KB_POLL_LEN
#define KB_POLL_LEN     5u
#endif

void    Kb_BuildPoll(uint8_t *out5, uint8_t seq, uint8_t host_ts_l,
                     uint8_t link_quality, uint8_t flags);
/* 【修复 P2-3】增加 inlen：POLL 是定长 5 字节帧，长度不够就不该去读
 * in5[1..4]。原来没有长度形参，收到短帧（或噪声）时会读到调用方缓冲之外，
 * 解析出的 seq/flags 是垃圾值 —— 表现就是"幽灵按键"（键状态被莫名改掉）。 */
uint8_t Kb_ParsePoll(const uint8_t *in5, uint8_t inlen, uint8_t *seq,
                     uint8_t *host_ts_l, uint8_t *link_quality, uint8_t *flags);

uint8_t Kb_BuildHid6(uint8_t *out8, uint8_t seq, const hid_report_t *r);
uint8_t Kb_BuildHidNkro(uint8_t *out32, uint8_t seq, const hid_report_t *r);
uint8_t Kb_ParseHid(const uint8_t *in, uint8_t len, uint8_t *seq, hid_report_t *r);

void    Kb_BuildRetrieve(uint8_t *out2, uint8_t lost_seq);
uint8_t Kb_ParseRetrieve(const uint8_t *in2, uint8_t *lost_seq);
void    Kb_BuildLed(uint8_t *out2, uint8_t led);
uint8_t Kb_ParseLed(const uint8_t *in2, uint8_t *led);

/* KB_POLL.flags 位 */
#define KB_FLAG_REQ_NKRO        0x01u

/*==========================================================================
 * 4. 主机侧状态机（规范 4.6 / 16.4）
 *========================================================================*/
typedef struct
{
    kb_pair_info_t pair;
    hid_report_t   last_report;
    link_stat_t    stat;
    hop_ctx_t      hop;
    uint8_t        seq;             /* 每一帧 +1，用来去重/补帧         */
    uint8_t        host_ms;         /* 1ms 计数器低 8 位                */
    uint8_t        led;             /* 键盘灯状态                       */
    uint8_t        retrieving;      /* 1 = 这一轮发的是 RETRIEVE        */
    uint8_t        retrieve_seq;    /* 要补的是哪一帧                   */
    uint8_t        nkro_req_pending;
    uint32_t       last_poll_us;
    uint32_t       polls;           /* 成功了多少轮                     */
    uint32_t       timeouts;
} kb_host_t;

void Kb_HostInit(kb_host_t *h, const kb_pair_info_t *pair);

/*********************************************************************
 * @fn      Kb_HostPoll
 * @brief   跑一个 1 ms 周期：发 POLL（或补帧 RETRIEVE）→ 等 150 μs 应答
 * @param   out  收到的 HID 报告（可以为 0，只统计不发出去）
 * @return  HALRF_OK = 收到报告
 *          HALRF_E_TIMEOUT = 这轮没收到（已记进统计；连丢 4 帧后
 *                            下一轮会自动改成发 RETRIEVE 补帧）
 *          HALRF_E_* = 硬件错
 * @note    返回后立刻返回，调用方负责"对齐到下一个 1 ms"。
 *********************************************************************/
int Kb_HostPoll(kb_host_t *h, hid_report_t *out);

/* 主机现在是不是在补帧（测试/观测用） */
bool Kb_HostIsRetrieving(const kb_host_t *h, uint8_t *retrieve_seq);

/*********************************************************************
 * @fn      Kb_HostTickMs
 * @brief   喂 1 ms 时钟（host_ts_l 和看门狗用）
 *********************************************************************/
void Kb_HostTickMs(kb_host_t *h);

/*==========================================================================
 * 5. 从机侧状态机（规范 12.1 / 15.1）
 *========================================================================*/
typedef enum
{
    KB_DEV_UNPAIRED = 0,        /* CH=20 监听配对请求   */
    KB_DEV_PAIRING  = 1,        /* 配对协商中           */
    KB_DEV_CONNECTED= 2,        /* 1ms 应答 POLL        */
    KB_DEV_LOWPOWER = 3,        /* 长时间没 POLL → 睡   */
    KB_DEV_RECOVER  = 4         /* 连续丢帧 → 重建链路  */
} kb_dev_state_t;

typedef struct
{
    kb_pair_info_t  pair;
    hid_report_t    report;         /* 当前按键状态             */
    kb_report_cache_t cache;        /* 最近一次发出去的报文     */
    link_stat_t     stat;
    hop_ctx_t       hop;
    kb_dev_state_t  state;
    uint8_t         led;
    uint8_t         miss;           /* 连续多少个 POLL 没应答   */
    uint32_t        last_poll_ms;
    uint32_t        reports_sent;
    bool            dirty;          /* 按键状态变了，需要上报     */
} kb_dev_t;

void Kb_DevInit(kb_dev_t *d);

/*********************************************************************
 * @fn      Kb_DevOnPoll
 * @brief   从机收到一个 POLL，组好应答帧放进 out
 * @param   in5     收到的 POLL
 * @param   inlen   in5 的实际字节数（< KB_POLL_LEN 直接拒收，见 P2-3）
 * @param   out32   输出缓冲（至少 32B）
 * @param   outlen  输出：应答长度（6KRO 8B / NKRO 32B）
 * @return  true = 有应答要发；false = 不用发（不是给我们的 / 参数不对）
 * @note    【修复 P2-3】inlen 由调用方按"射频实际收到的字节数"传入。
 *********************************************************************/
bool Kb_DevOnPoll(kb_dev_t *d, const uint8_t *in5, uint8_t inlen,
                  uint8_t *out32, uint8_t *outlen);

/*********************************************************************
 * @fn      Kb_DevOnRetrieve
 * @brief   收到补帧请求 → 把缓存的报告重发一次
 *********************************************************************/
bool Kb_DevOnRetrieve(kb_dev_t *d, const uint8_t *in2, uint8_t *out32, uint8_t *outlen);

/* 更新按键状态（由按键扫描调用），内部会置 dirty */
void Kb_DevSetReport(kb_dev_t *d, const hid_report_t *r);

/*********************************************************************
 * @fn      Kb_DevTickMs
 * @brief   1ms 时钟：没 POLL 超过 5ms 进低功耗，连续丢帧进 RECOVER
 *********************************************************************/
void Kb_DevTickMs(kb_dev_t *d, uint8_t miss_limit);

/* 从机当前该用哪个频点（每 32 帧切一次，规范 4.4） */
uint8_t Kb_HopChannelFor(uint8_t seq, const kb_pair_info_t *p);

#ifdef __cplusplus
}
#endif

#endif /* __PROTO_KB_H__ */
