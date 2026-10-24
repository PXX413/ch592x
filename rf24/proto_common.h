/**************************************************************************
 * proto_common.h —— CH592X / CH570Q 2.4G 私有协议公共层
 *
 * 依据：《CH592X / CH570Q 2.4G 私有协议详细规范 v1.0》
 *        第 3 章（帧格式总览）、第 13 章（速查表）、第 16 章（SDK 落地）、附录 D
 *
 * 本层只放"两边都要用"的东西：
 *   1) frame_ctrl 的位域拆解宏
 *   2) 全部帧类型编号 + 每一帧的长度
 *   3) 按键行为枚举、错误码、status 位
 *   4) CRC-8 / CRC-16、小端读写、位操作
 *
 * 不依赖任何硬件头文件，PC 上可以直接编译测试。
 *
 * ------------------------------------------------------------------
 * 【规范里的三处自相矛盾，本实现这样处理】（详见 README_2.4G协议说明.md）
 *
 * 1) 协议判别：规范 3.1 说 frame_ctrl[7:6] 是 protocol 位，附录 D.2 又说
 *    "高 4 位不参与判别，由信道+地址+长度共同判别"。两者不可能同时成立
 *    （0x40|0x10=0x50 会和别的打架）。本实现**采用附录 D.2 的最终约定**：
 *    frame_ctrl 只用来路由帧类型，协议靠信道 + 链路地址 + 帧长区分。
 *
 * 2) KEYMAP 分片：规范 6.3 的帧布局给 payload 留的是 **25 字节**（偏移
 *    6..30，CRC8 在 31），但 6.1 又写"每片 28 字节、共 7 片"。25×7=175，
 *    装不下 9 键最坏 198 字节。本实现**按帧布局的 25 字节**来，
 *    frag_total 变成**由内容算出来的动态值**（最坏 8 片），字段本身支持。
 *
 * 3) 9 键名称 TLV：6.2 要求"名称必须与行为类型成对上报"，6.1 的字节预算
 *    (9×22=198) 也算进了行为字节，但 6.4 的 TLV 图里没有 action。
 *    本实现**按 6.1 的预算**把 action 放进 TLV，即
 *        { key_id(1) | action(1) | len(1) | name[len] }
 *
 * 4) RENAME_KEY：6.9.1（24B，name 只有 18B）和 D.2.4（28B 字段）对不上，
 *    而且 6.9.1 的 `frame_ctrl = 0x40 | dev_id` 会和 0x41/0x44 撞车。
 *    本实现改成 **32B、dev_id 单独占一字节**，name 放满 19 字节，
 *    scene 参数塞进 action 字节的高 5 位（只有 ACT_SCENE 用得到）。
 **************************************************************************/

#ifndef __PROTO_COMMON_H__
#define __PROTO_COMMON_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 物理层常量（规范 2.1 / 2.4 / 2.5）
 *========================================================================*/
#define RF_MTU                  32          /* 单次射频最大有效载荷 */
#define RF_PLOAD_MAX            32
#define RF_ADDR_LEN             5           /* 地址 5 字节 */
#define RF_CRC16_INIT           0xFFFFu
#define RF_CRC8_INIT            0x00u
/* 合法信道上限： CH = (f_MHz - 2400) / 2，每信道 2MHz。
 * ISM 上限 2483.5MHz -> (2483.5-2400)/2 = 41.75 -> 最大整数 41（= 2482MHz）。
 * 【修复 P0-2】原来写 84 —— 那是"每信道 1MHz"的算法，等于把 42~84 这些
 * 落在 ISM 频段外的频点全部放行（跳频表里的 50/47/54 分别会发到
 * 2500/2494/2508MHz）。 */
#define RF_MAX_CHANNEL          41          /* 上限 41 = 2482MHz */

#define RF_IFG_US               150         /* 两帧之间最小间隔 */
#define RF_KB_RX_WINDOW_US      150         /* 键盘模式 POLL→RESP 窗口 */
#define RF_RC_RX_WINDOW_US      400         /* 遥控模式 CMD→REPORT 窗口 */

/* 地址前缀（规范 2.4） */
#define RF_ADDR_KB_PREFIX0      0x71u       /* 'q' */
#define RF_ADDR_KB_PREFIX1      0x6Eu       /* 'n' */
#define RF_ADDR_KB_PREFIX2      0x61u       /* 'a' */
#define RF_ADDR_KB_PREFIX3      0x52u       /* 'R' */

#define RF_ADDR_RC_HOST0        0x52u       /* 'R' */
#define RF_ADDR_RC_HOST1        0x43u       /* 'C' */
#define RF_ADDR_RC_HOST2        0x5Fu       /* '_' */
#define RF_ADDR_RC_HOST3        0x48u       /* 'H' */
#define RF_ADDR_RC_HOST4        0x00u

#define RF_ADDR_RC_DEV0         0x52u       /* 'R' */
#define RF_ADDR_RC_DEV1         0x43u       /* 'C' */
#define RF_ADDR_RC_DEV2         0x5Fu       /* '_' */
#define RF_ADDR_RC_DEV3         0x44u       /* 'D' */
/* dev_id 1..16 → 后缀 0x11..0x20（注意 16 需要 5 个 bit，别用 &0x0F） */
#define RF_ADDR_RC_DEV4(dev)    ((uint8_t)(0x10u + ((dev) & 0x1Fu)))

#define RF_ADDR_RC_ENUM0        0xAAu       /* 未枚举从机监听地址 */
#define RF_ADDR_RC_ENUM1        0xBBu
#define RF_ADDR_RC_ENUM2        0xCCu
#define RF_ADDR_RC_ENUM3        0xDDu
#define RF_ADDR_RC_ENUM4        0x01u

#define RF_RC_ENUM_CHANNEL      20          /* 枚举信道 2.440GHz */

/* 一台主机最多带几台从机。
 *
 * 【P0 修复】由 16 降到 6。规范里的 1:16 是**协议上限**，但本实现的主机
 * 结构体 rc_host_t 里每个 dev_id 都要占一份命令队列（rc_cmd_t 35B）和一份
 * 上报时间戳，16 份就是 17×(35+2)=629B；而整块结构体是**一次性动态申请**的，
 * 多出来的量直接把它顶出堆容量（实测 sizeof(rc_host_t)=2688B > 堆 2568B），
 * 遥控模式连申请都申请不到，屏幕停在 0 设备还不报错。
 *
 * 主机侧设备表 RC_HOST_MAX_DEV 与遥控页 UI 上限 UIRC_DEV_MAX 本来就都是 6，
 * 所以这里对齐成 6：
 *   * 第 7 台及以后不再分配 dev_id（Tdma_AllocSlot 返回 0 = 满了）
 *   * 协议帧里的 dev_id 校验范围随之收成 1..6（link_keymap / proto_rc /
 *     rf_cfg 里的判断都引用本宏，自动跟着走）
 * 要改回 16，必须先在 Ld/Link.ld 里把堆让出来（内存账见 User/Main.c）。 */
#define RF_RC_MAX_DEV           6           /* 1 主机 : 6 从机（理由见上） */

/*==========================================================================
 * 2. frame_ctrl 拆解（规范 3.1 + 附录 D.2 最终约定）
 *
 * 【重要】规范 3.1 说 type 只占 bit[4:0]、bit5 是 AC 标志，但第 13 章的
 * 帧表里 RC_REPORT=0x20、RC_BATT=0x21、RC_KEYMAP_FRAG=0x30 …… 全都把
 * bit5 当成了类型的一部分 —— 两者不可能同时成立：
 *     RC_REPORT(0x20) & 0x1F == 0  →  会和 KB_ACK(0x00) 撞车。
 *
 * 本实现按附录 D.2 的"最终约定"办：**frame_ctrl 整个字节就是帧类型**，
 * 只有 bit7 留作保留位（路由时掩掉），协议靠信道 + 地址 + 帧长区分。
 *========================================================================*/
#define FRAME_CTRL_TYPE_MASK    0x7Fu
#define FRAME_CTRL_TYPE_BIT7    0x80u

#define FRAME_TYPE(fc)          ((uint8_t)((fc) & FRAME_CTRL_TYPE_MASK))

/* 协议区间判别（按类型号码段，别再按 bit[7:6] 去判） */
#define IS_KB(fc)               (FRAME_TYPE(fc) <= KB_PAIR)
#define IS_RC(fc)               ((FRAME_TYPE(fc) >= RC_SYNC) && (FRAME_TYPE(fc) <= RC_SET_MACRO))

/* 规范 3.1 里的 AC 位：本实现没有用到带 AC 位的帧，留着只为兼容旧代码 */
#define FRAME_CTRL_AC_MASK      0x20u
#define IS_ACK(fc)              (((fc) & FRAME_CTRL_AC_MASK) != 0u)

/*==========================================================================
 * 3. 帧类型字典（规范 第 13 章 + 附录 D）
 *========================================================================*/

/* ---- 键盘模式 PROTO_KB，0x00–0x0F ---- */
#define KB_ACK                  0x00u       /* Device→Host  1B  纯 ACK            */
#define KB_POLL                 0x01u       /* Host→Device  5B  轮询             */
#define KB_HID_6KRO             0x02u       /* Device→Host  8B  6 键 HID 报告    */
#define KB_HID_NKRO             0x03u       /* Device→Host 32B  全键无冲报告     */
#define KB_RETRIEVE             0x04u       /* Host→Device  2B  补帧请求         */
#define KB_LED                  0x05u       /* Host→Device  2B  LED 状态同步     */
#define KB_PAIR                 0x0Fu       /* 双向        16B  配对协商         */

/* ---- 遥控模式 链路层，0x10–0x1F ---- */
#define RC_SYNC                 0x10u       /* Host→All     8B  超帧同步         */
#define RC_CMD                  0x11u       /* Host→Dev     8B  通用命令         */
#define RC_ENUM_RSP             0x12u       /* Dev→Host    32B  枚举响应         */
#define RC_ENUM_ACK             0x13u       /* Host→Dev     8B  枚举确认         */
#define RC_SLEEP                0x15u       /* Host→Dev     2B  休眠命令         */
#define RC_RESET                0x16u       /* Host→Dev     2B  软复位           */
#define RC_PING                 0x17u       /* 双向         3B  心跳             */

/* ---- 遥控模式 上报层，0x20–0x2F ---- */
#define RC_REPORT               0x20u       /* Dev→Host     8B  状态上报         */
#define RC_BATT                 0x21u       /* Dev→Host     3B  电池电压         */
#define RC_TEMP                 0x22u       /* Dev→Host     3B  温度             */
#define RC_LOG                  0x28u       /* Dev→Host    ≤32B 调试日志         */
#define RC_EMERGENCY            0x29u       /* Dev→Host     8B  按键唤醒抢占帧   */

/* ---- 遥控模式 按键名称层，0x30–0x3F ---- */

/* ---- 遥控模式 配置层，0x40–0x4F ---- */
#define RC_SET_SCENE            0x43u       /* Host→Dev     4B  设置 SCENE 参数  */
#define RC_SET_MACRO            0x44u       /* Host→Dev   ≤32B  设置宏           */

/* ---- 每一帧的固定长度（变长帧给最小值，运行时用返回长度） ---- */
#define KB_ACK_LEN              1u
#define KB_POLL_LEN             5u
#define KB_HID_6KRO_LEN         8u
#define KB_HID_NKRO_LEN         32u
#define KB_RETRIEVE_LEN         2u
#define KB_LED_LEN              2u
#define KB_PAIR_LEN             16u

#define RC_SYNC_LEN             8u
#define RC_CMD_LEN              8u
#define RC_ENUM_RSP_LEN         32u
#define RC_ENUM_ACK_LEN         8u
#define RC_SLEEP_LEN            2u
#define RC_RESET_LEN            2u
#define RC_PING_LEN             3u

#define RC_REPORT_LEN           8u
#define RC_BATT_LEN             3u
#define RC_TEMP_LEN             3u
#define RC_EMERGENCY_LEN        8u


#define RC_SET_SCENE_LEN        4u

/*==========================================================================
 * 4. PAIR 帧阶段（规范 4.3.3）
 *========================================================================*/
#define KB_PAIR_REQ             0u
#define KB_PAIR_RESP            1u
#define KB_PAIR_CONFIRM         2u
#define KB_PAIR_COMPLETE        3u

/*==========================================================================
 * 5. RC_CMD 的命令码（规范 5.3.2）
 *========================================================================*/
#define RC_CMD_NOP              0x00u
#define RC_CMD_SET              0x01u
#define RC_CMD_SLEEP            0x04u
#define RC_CMD_ACTION           0x06u       /* Host->Dev  执行动作编号（带 dev_id 寻址）*/
#define RC_CMD_RESET            0x05u

/*==========================================================================
 * 6. REPORT.status 位（附录 D.2.2）
 *========================================================================*/
#define RC_ST_ONLINE            (1u << 0)
#define RC_ST_ERROR             (1u << 1)
#define RC_ST_BATT_LOW          (1u << 2)
#define RC_ST_KEY_CHANGED       (1u << 3)
#define RC_ST_VER_MISMATCH      (1u << 4)
#define RC_ST_ENUM_COMPLETE     (1u << 5)
#define RC_ST_EMERGENCY         (1u << 6)

/*==========================================================================
 * 7. SYNC.flags 位（规范 5.3.1）
 *========================================================================*/
#define RC_SYNC_FLAG_ENUM_OPEN  (1u << 0)

/*==========================================================================
 * 8. 按键行为类型（规范 6.2）
 *========================================================================*/
typedef enum
{
    ACT_MOMENTARY   = 0x00,     /* 按下=1，松开=0（瞬时按钮） */
    ACT_TOGGLE      = 0x01,     /* 按一下翻转（开关）        */
    ACT_LATCH       = 0x02,     /* 按住有效（方向键）        */
    ACT_INCREMENT   = 0x03,     /* 每按 +1                   */
    ACT_DECREMENT   = 0x04,     /* 每按 -1                   */
    ACT_SCENE       = 0x05,     /* 场景调用（带 1B 参数）    */
    ACT_MACRO       = 0x06,     /* 宏                        */
    ACT_RESERVED    = 0x07      /* 保留 / 该键物理不存在     */
} key_action_e;

#define KEY_ACTION_VALID(a)     ((a) <= (uint8_t)ACT_RESERVED)

/*==========================================================================
 * 9. 9 键名称通道的尺寸（规范 6.1–6.4）
 *========================================================================*/
#define RC_KEY_COUNT            9u          /* K1..K9                   */
#define RC_KEY_NAME_MAX         19u         /* 单键名称最长 19 字节     */
#define RC_KEYMAP_HDR_LEN       6u          /* 帧头到 payload_len 共 6B */
#define RC_KEYMAP_PAYLOAD       25u         /* 每片有效载荷             */
#define RC_KEYMAP_CRC_IDX       31u         /* 片内 CRC-8 位置          */
#define RC_KEYMAP_TLV_MAX       ((RC_KEY_COUNT) * (RC_KEY_NAME_MAX + 3u))  /* 198 */

#define RC_DEV_NAME_MAX         19u         /* 设备名称最长 19 字节     */
#define RC_RENAME_NAME_MAX      RC_KEY_NAME_MAX     /* 改名帧也是 19B    */
#define RC_KEYMAP_RX_TIMEOUT_MS 200u        /* 收齐窗口（规范 6.3）     */
#define RC_KEYMAP_MAX_FRAGS     8u          /* 25B/片、最坏 198B → 8 片 */

/*==========================================================================
 * 10. 错误码（规范 附录 B）
 *========================================================================*/
#define RC_OK                   0x00u
#define RC_E_PARAM              0x01u
#define RC_E_LEN                0x02u
#define RC_E_CRC                0x03u
#define RC_E_VER                0x04u
#define RC_E_FULL               0x05u
#define RC_E_TIMEOUT            0x06u
#define RC_E_FLASH              0x07u
#define RC_E_RF                 0x08u
#define RC_E_NAME_INVALID       0x09u
#define RC_E_BUSY               0x0Au

/*==========================================================================
 * 11. 协议版本（规范 21.1）
 *========================================================================*/
#define PROTO_VERSION_MAJOR     1u
#define PROTO_VERSION_MINOR     0u
#define PROTO_VERSION_BYTE      ((uint8_t)((PROTO_VERSION_MAJOR << 4) | PROTO_VERSION_MINOR))

/*==========================================================================
 * 12. CRC / 字节序 / 位操作
 *========================================================================*/

/*********************************************************************
 * @fn      Rf_Crc8
 * @brief   CRC-8（poly 0x07，init 0x00，不反转，不异或）
 * @note    用于片内校验、改名帧校验
 *********************************************************************/
uint8_t Rf_Crc8(const uint8_t *p, uint16_t n);

/*********************************************************************
 * @fn      Rf_Crc16
 * @brief   CRC-16/CCITT-FALSE（poly 0x1021，init 0xFFFF）
 * @note    规范 2.1 写的是 "CRC-16 (0xA001)"、16.2 又写 init 0xFFFF，
 *          两者对不上；这里统一成 CCITT-FALSE，和工程里 game_score 的
 *          CRC 算法保持一致，避免一个工程两套 CRC。
 *********************************************************************/
uint16_t Rf_Crc16(const uint8_t *p, uint16_t n);

/* 小端读写（规范 2.2） */
static inline void Rf_PutU16LE(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t Rf_GetU16LE(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void Rf_PutU32LE(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t Rf_GetU32LE(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* 位操作 */
#define RF_BIT(n)               ((uint32_t)1u << (n))
#define RF_BIT_TEST(v, n)       (((v) & RF_BIT(n)) != 0u)
#define RF_BIT_SET(v, n)        ((v) |= RF_BIT(n))
#define RF_BIT_CLR(v, n)        ((v) &= ~RF_BIT(n))

/*==========================================================================
 * 13. 小工具
 *========================================================================*/

/* 某一帧类型在本实现里规定的固定长度；变长/未知类型返回 0 */
uint8_t Rf_FrameLen(uint8_t frame_ctrl);

/* 帧类型的中文/英文短名（调试打印用，只返回 ASCII，不会出现中文） */
const char *Rf_FrameName(uint8_t frame_ctrl);

/* 这个帧类型是不是遥控模式（0x10–0x4F） */
static inline bool Rf_IsRcType(uint8_t fc)
{
    uint8_t t = FRAME_TYPE(fc);
    return (t >= RC_SYNC) && (t <= RC_SET_MACRO);
}

/* 这个帧类型是不是键盘模式（0x00–0x0F） */
static inline bool Rf_IsKbType(uint8_t fc)
{
    return (FRAME_TYPE(fc) <= KB_PAIR);
}

/* 把 4 字节小端地址和 1 字节后缀拼成 5 字节链路地址 */
void Rf_MakeAddr(uint8_t out[RF_ADDR_LEN], uint32_t prefix_le, uint8_t suffix);

/* 生成遥控模式主机地址 */
void Rf_MakeRcHostAddr(uint8_t out[RF_ADDR_LEN]);

/* 生成遥控模式某个从机的地址；dev_id = 0 时给枚举地址 */
void Rf_MakeRcDevAddr(uint8_t out[RF_ADDR_LEN], uint8_t dev_id);

#ifdef __cplusplus
}
#endif

#endif /* __PROTO_COMMON_H__ */
