/**************************************************************************
 * kmap_proto.h —— 按键映射配置协议（设备端）
 *
 * 【这套协议解决什么问题】
 *   网页（上位机）通过 USB 虚拟串口直接读写设备上的按键映射，
 *   改完存进 Flash，不用重新烧录固件。
 *
 * 【帧格式】（所有多字节小端）
 *   +--------+--------+--------+--------+----------+--------+
 *   | 0xA5   | 0x5A   | CMD    | SEQ    | LEN(2B)  | PAYLOAD| CRC8  |
 *   +--------+--------+--------+--------+----------+--------+
 *     帧头1    帧头2    命令     序号     负载长度   负载     校验
 *
 *   - 帧头固定 0xA5 0x5A，用来在一个字节流里做同步
 *   - SEQ 由上位机递增，设备原样回填，用来配对请求与应答
 *   - LEN 是 PAYLOAD 的字节数（不含帧头/CRC）
 *   - CRC8 覆盖【帧头到负载末尾】的全部字节，poly=0x07，
 *     和 rf24/proto_common.h 里的 crc8 保持一致（同一套算法，减少心智负担）
 *
 * 【为什么带 LEN 还带 CRC】
 *   串口会丢字节、粘包。有 LEN 才能正确切帧，有 CRC 才能发现内容错了。
 *   两者配合：先靠帧头+LEN 找边界，再用 CRC 确认这帧可信。
 *
 * 【命令一览】
 *   0x01 PING          探测设备，回 PONG，用来确认"对面是我的键盘"
 *   0x02 GET_INFO      读设备信息（版本/组数/每键字节数/当前组）
 *   0x10 GET_MAP       读一组映射（9 个 uint16）
 *   0x11 SET_KEY       写单个键
 *   0x12 SET_GROUP     写整组（9 个 uint16，一次写完）
 *   0x13 GET_NAME      读组名
 *   0x14 SET_NAME      写组名
 *   0x20 SET_GROUP_IDX 切换当前生效的映射组
 *   0x30 SAVE_FLASH    把当前映射存进 Flash（掉电不丢）
 *   0x31 LOAD_FLASH    从 Flash 重新载入映射
 *   0x32 RESET_DEFAULT 恢复出厂默认映射
 *
 *   应答：CMD | 0x80（如 GET_MAP 的应答是 0x90），SEQ 原样回填
 *   错误：CMD 0x7F，负载第 1 字节是错误码
 **************************************************************************/

#ifndef __KMAP_PROTO_H__
#define __KMAP_PROTO_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 常量
 *========================================================================*/
#define KMAP_SOF0               0xA5u
#define KMAP_SOF1               0x5Au

#define KMAP_CRC8_POLY          0x07u

/* 帧里负载最长多少字节（SET_NAME 要传组名，给宽一点） */
#define KMAP_MAX_PAYLOAD        64u
/* 整帧最大长度 = 2(头) + 1(CMD) + 1(SEQ) + 2(LEN) + PAYLOAD + 1(CRC) */
#define KMAP_MAX_FRAME          (7u + KMAP_MAX_PAYLOAD)

/* 与 key_matrix.h 一致；这里再声明一份，方便协议自洽校验 */
#define KMAP_GROUPS             5u
#define KMAP_KEYS               9u
/* 编码器槽（v3 新增）：1=正转 2=反转 3=单击 */
#define KMAP_ENC_SLOTS          3u

/* 组名最大长度（不含结尾 0） */
#define KMAP_NAME_MAX           16u

/*==========================================================================
 * 2. 命令码
 *========================================================================*/
#define KMAP_CMD_PING           0x01u
#define KMAP_CMD_GET_INFO       0x02u

#define KMAP_CMD_GET_MAP        0x10u
#define KMAP_CMD_SET_KEY        0x11u
#define KMAP_CMD_SET_GROUP      0x12u
#define KMAP_CMD_GET_NAME       0x13u
#define KMAP_CMD_SET_NAME       0x14u
/* ---- v3 新增：编码器槽（正转/反转/单击）----
 * 0x15 GET_ENCMAP  请求负载 [group]                     应答负载 3×uint16 [正转,反转,单击]
 * 0x16 SET_ENCKEY  请求负载 [group, which, val_lo, val_hi]
 *                  which = 1 正转 / 2 反转 / 3 单击      应答负载 同 SET_KEY（回读实际值）
 *
 * 语义与九个键完全一致：16 位映射值（高字节修饰键位图 + 低字节 HID Usage），
 * 值 0 = 该动作不按键。改完照旧发 0x30 SAVE_FLASH 落盘（kmap_store v3 记录）。
 * 上位机工具 tools/keymap_editor.html 已接好这两条命令。 */
#define KMAP_CMD_GET_ENCMAP     0x15u
#define KMAP_CMD_SET_ENCKEY     0x16u
/* ---- v4 新增：遥控模式的三张配置表（见 遥控模式开发文档.md §4）----
 *
 * 三张表都存在 rf24/rc_table.c 里（数据 Flash，双 bank 乒乓）。
 * 这里只做"参数搬运"，**一帧一条记录**，不做分片
 * （最大一条 31 B，远小于 KMAP_MAX_PAYLOAD=64）。
 *
 * 0x40 GET_COUNTS  []                            应答 [cat_cnt, tbl_cnt, bind_cnt]
 * 0x41 GET_CATALOG [idx]                         应答 [id, name[11]]
 * 0x42 SET_CATALOG [id, name[11]]                应答 回读 [id, name[11]]
 * 0x43 GET_TBL     [idx]                         应答 [name[11], key_num[12]]
 * 0x44 SET_TBL     [name[11], key_num[12]]       应答 回读 同格式
 * 0x45 GET_BIND    [idx]                         应答 [dev_name[20], tbl_name[11]]
 * 0x46 SET_BIND    [dev_name[20], tbl_name[11]]  应答 回读 同格式
 * 0x47 SAVE        []                            应答 空
 *
 * 注意：SET_* 每条都会**整块重写一个 bank**（擦 + 8 页写，约几十毫秒），
 * 所以网页侧别一条一条刷，用户点"保存"时统一写。 */
#define KMAP_CMD_RC_GET_COUNTS  0x40u
#define KMAP_CMD_RC_GET_CATALOG 0x41u
#define KMAP_CMD_RC_SET_CATALOG 0x42u
#define KMAP_CMD_RC_GET_TBL     0x43u
#define KMAP_CMD_RC_SET_TBL     0x44u
#define KMAP_CMD_RC_GET_BIND    0x45u
#define KMAP_CMD_RC_SET_BIND    0x46u
#define KMAP_CMD_RC_SAVE        0x47u

/* ---- 键盘宏（v5，2026-10-15）----
 *   一个键触发一串按键：宏内容 + 每组 9 个键的绑定表，独立占一块 Flash。
 *   见 keyboard/macro.h。 */
#define KMAP_CMD_MACRO_GET_BIND 0x50u   /* 负载 1B 组号 -> 应答 9B 绑定表      */
#define KMAP_CMD_MACRO_SET_BIND 0x51u   /* 负载 1B 组号 + 9B 绑定              */
#define KMAP_CMD_MACRO_GET      0x52u   /* 负载 1B 宏号 -> 应答 1B 步数 + 8×2B */
#define KMAP_CMD_MACRO_SET      0x53u   /* 负载 1B 宏号 + 1B 步数 + 8×2B       */
#define KMAP_CMD_MACRO_SAVE     0x54u   /* 无负载 -> 存进 Flash                */

#define KMAP_CMD_SET_GROUP_IDX  0x20u
#define KMAP_CMD_SAVE_FLASH     0x30u
#define KMAP_CMD_LOAD_FLASH     0x31u
#define KMAP_CMD_RESET_DEFAULT  0x32u

/* 应答 = 请求 | 0x80 */
#define KMAP_ACK(cmd)           ((uint8_t)((cmd) | 0x80u))
/* PING 的应答单独给个 PONG，方便肉眼在串口助手里认 */
#define KMAP_CMD_PONG           0x81u

/* 错误应答 */
#define KMAP_CMD_ERROR          0x7Fu

/* 错误码 */
#define KMAP_OK                 0x00u
#define KMAP_E_CRC              0x01u   /* CRC 校验失败     */
#define KMAP_E_LEN              0x02u   /* 长度不合法       */
#define KMAP_E_CMD              0x03u   /* 未知命令         */
#define KMAP_E_ARG              0x04u   /* 参数越界         */
#define KMAP_E_FLASH            0x05u   /* Flash 写入失败   */

/*==========================================================================
 * 3. 解析器状态
 *========================================================================*/
typedef struct
{
    uint8_t  buf[KMAP_MAX_FRAME];   /* 组帧缓冲         */
    uint16_t len;                   /* 已收字节数       */
    uint32_t frames_ok;             /* 统计：成功帧数   */
    uint32_t frames_bad;            /* 统计：失败帧数   */
    uint32_t bytes_in;              /* 统计：收到字节数 */
} kmap_parser_t;

/* 一帧解析结果 */
typedef struct
{
    uint8_t  cmd;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[KMAP_MAX_PAYLOAD];
} kmap_frame_t;

/*==========================================================================
 * 4. 工具
 *========================================================================*/

/* CRC8，poly=0x07，初值 0x00，不反转。
 * 与 rf24/proto_common.c 的实现保持一致。 */
uint8_t Kmap_Crc8(const uint8_t *p, uint16_t len);

/*********************************************************************
 * @fn      Kmap_BuildFrame
 * @brief   组一帧
 * @param   out      输出缓冲，至少 KMAP_MAX_FRAME 字节
 * @param   cmd      命令码
 * @param   seq      序号
 * @param   payload  负载，可为 0（len 必须为 0）
 * @param   len      负载长度
 * @return  整帧字节数；0 = 参数错
 *********************************************************************/
uint16_t Kmap_BuildFrame(uint8_t *out, uint8_t cmd, uint8_t seq,
                         const uint8_t *payload, uint16_t len);

/*********************************************************************
 * @fn      Kmap_ParserInit
 * @brief   初始化解析器
 *********************************************************************/
void Kmap_ParserInit(kmap_parser_t *ps);

/*********************************************************************
 * @fn      Kmap_ParserPush
 * @brief   喂字节。收到完整且校验通过的一帧就返回 true，并填 out
 * @param   ps    解析器
 * @param   byte  新到的字节
 * @param   out   解析结果（可为 0，只统计不取）
 * @return  true = 组出一帧
 * @note    丢字节/杂数据都能自动重新同步（靠帧头 0xA5 0x5A），
 *          不会因为半帧卡死
 *********************************************************************/
bool Kmap_ParserPush(kmap_parser_t *ps, uint8_t byte, kmap_frame_t *out);

/*==========================================================================
 * 5. 负载小端读写
 *========================================================================*/
static inline void Kmap_PutU16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t Kmap_GetU16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

#ifdef __cplusplus
}
#endif

#endif /* __KMAP_PROTO_H__ */
