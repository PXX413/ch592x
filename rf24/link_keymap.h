/**************************************************************************
 * link_keymap.h —— 9 键功能名称元数据通道（分片 + TLV + 改名 + 校验）
 *
 * 依据：规范 第 6 章、第 18.2 章、第 11.2 章（Flash 布局）
 *
 * 解决的问题：从机要把 K1–K9 各自的功能名称（≤19 字节 ASCII）报给主机，
 * 9 键最坏 198 字节 >> MTU 32 字节，所以必须分片。
 *
 * ------------------------------------------------------------------
 * 【线上 TLV 格式】（本实现的取舍，理由见 proto_common.h 顶部注释）
 *
 *     +--------+--------+--------+-------------------+
 *     | key_id |  act   |  len   |  name[0..len-1]   |
 *     |  1B    |  1B    |  1B    |   len 字节        |
 *     +--------+--------+--------+-------------------+
 *
 *   act 一个字节里塞了两样东西（省一个字节，正好凑成规范 6.1 的 9×22=198）：
 *        bit[2:0] = key_action_e
 *        bit[7:3] = scene 参数（只有 ACT_SCENE 用得到）
 *
 *   len = 0..19  该键有名字
 *   len = 0xFF   该键**物理不存在**（主机 UI 直接隐藏）
 *   act = ACT_RESERVED 且 len = 0  → 该键存在但没配，UI 灰显
 *
 * 【本实现修掉的规范缺陷】
 *   1) 6.5 的示例代码把分片按**到达顺序** memcpy 进 tlv_buf —— 只要片乱序
 *      到达，重组出来的字节流就是错的。本实现按 frag_index 算出固定
 *      偏移（idx×25）再落盘，乱序到达也不影响。
 *   2) 6.3 说"7 片"是按 28B/片算的，但帧布局只给了 25B。本实现按帧布局来，
 *      frag_total 动态计算（最坏 8 片）。
 **************************************************************************/

#ifndef __LINK_KEYMAP_H__
#define __LINK_KEYMAP_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. act 字节的拆分（action + scene 参数）
 *========================================================================*/
#define RC_ACT_BYTE(action, scene) \
        ((uint8_t)((((uint8_t)(action)) & 0x07u) | (uint8_t)((((uint8_t)(scene)) & 0x1Fu) << 3)))
#define RC_ACT_GET(b)           ((uint8_t)((b) & 0x07u))
#define RC_SCENE_GET(b)         ((uint8_t)(((b) >> 3) & 0x1Fu))

#define RC_KEY_LEN_ABSENT       0xFFu   /* len 字段：该键物理不存在 */
#define RC_KEY_LEN_NONE         0x00u   /* len 字段：该键没配名字   */

/*==========================================================================
 * 2. 键 / 设备配置
 *========================================================================*/
typedef struct
{
    uint8_t act;                                /* action | scene<<3        */
    uint8_t len;                                /* 0..19 或 0xFF            */
    char    name[RC_KEY_NAME_MAX + 1u];         /* 以 '\0' 收尾             */
} rc_key_t;                                     /* 22 字节                  */

typedef struct
{
    uint8_t  dev_name_len;
    char     dev_name[RC_DEV_NAME_MAX + 1u];
    rc_key_t key[RC_KEY_COUNT];
    uint8_t  config_ver;                        /* 改名就 +1                */
    uint16_t key_mask;                          /* bit0..8 = 该键物理存在（9 个键要 9 位）*/
} rc_keymap_t;                                  /* 221 字节                 */

/*==========================================================================
 * 3. 从机侧：打包
 *========================================================================*/

/*********************************************************************
 * @fn      Keymap_Default
 * @brief   填一份出厂默认配置（9 键都在、都叫 KEY1..KEY9、行为 MOMENTARY）
 *********************************************************************/
void Keymap_Default(rc_keymap_t *m, const char *dev_name);

/*********************************************************************
 * @fn      Keymap_BuildTlv
 * @brief   把 9 键配置编成 TLV 字节流
 * @param   out  输出缓冲
 * @param   cap  缓冲大小（至少要 RC_KEYMAP_TLV_MAX）
 * @return  编出来的字节数（0 表示失败）
 *********************************************************************/
uint16_t Keymap_BuildTlv(const rc_keymap_t *m, uint8_t *out, uint16_t cap);

/*********************************************************************
 * @fn      Keymap_FragCount
 * @brief   这么长的 TLV 需要几片（frag_total 字段的值）
 *********************************************************************/
uint8_t Keymap_FragCount(uint16_t tlv_len);

/*********************************************************************
 * @fn      Keymap_BuildFrag
 * @brief   组装第 frag_idx 片（32 字节整帧，含 CRC-8）
 * @param   m         配置
 * @param   dev_id    从机编号（填进帧里）
 * @param   frag_idx  0 .. frag_total-1
 * @param   out32     输出 32 字节
 * @return  这一片实际带了多少字节载荷（0 = 该片不存在）
 * @note    内部要一个 198 字节的 TLV 临时数组。从机端一片要跨一个超帧，
 *          每 16ms 重新编一次 TLV 太浪费栈，建议改用下面的
 *          Keymap_BuildTlv() + Keymap_BuildFragTlv() 两段式。
 *********************************************************************/
uint8_t Keymap_BuildFrag(const rc_keymap_t *m, uint8_t dev_id,
                         uint8_t frag_idx, uint8_t *out32);

/*********************************************************************
 * @fn      Keymap_BuildFragTlv
 * @brief   从一个已经编好的 TLV 流里切第 frag_idx 片（从机端推荐用法）
 * @param   tlv/tlv_len  先调 Keymap_BuildTlv() 编好的流
 *********************************************************************/
uint8_t Keymap_BuildFragTlv(const uint8_t *tlv, uint16_t tlv_len, uint8_t dev_id,
                            uint8_t cfg_ver, uint8_t frag_idx, uint8_t *out32);

/*********************************************************************
 * @fn      Keymap_ParseFrag
 * @brief   解析一片的头部（不改接收状态）
 * @param   frag32    32 字节
 * @param   dev_id    输出：这一片属于哪个从机
 * @param   cfg_ver   输出：配置版本
 * @param   total     输出：总分片数
 * @param   index     输出：本片序号
 * @param   plen      输出：本片有效载荷长度
 * @return  RC_OK / RC_E_CRC / RC_E_PARAM
 *********************************************************************/
uint8_t Keymap_ParseFrag(const uint8_t *frag32, uint8_t *dev_id, uint8_t *cfg_ver,
                         uint8_t *total, uint8_t *index, uint8_t *plen);

/*==========================================================================
 * 4. 主机侧：重组
 *========================================================================*/
typedef struct
{
    uint8_t  used;                      /* 这个槽位在用吗            */
    uint8_t  dev_id;
    uint8_t  config_ver;
    uint8_t  frag_total;
    uint8_t  frag_bitmap;               /* bit[i] = 第 i 片收到了    */
    uint8_t  last_ms_valid;
    uint32_t last_ms;                   /* 最近一次收到片的时间       */
    uint16_t tlv_len;                   /* 已收到的总长度（按最高片算）*/
    uint8_t  tlv[RC_KEYMAP_TLV_MAX];
} keymap_rx_t;

/*********************************************************************
 * @fn      Keymap_RxInit
 * @brief   把重组槽位全部复位
 *********************************************************************/
void Keymap_RxInit(keymap_rx_t *rx, uint8_t slots);

/*********************************************************************
 * @fn      Keymap_Feed
 * @brief   喂一片进去；收齐了就自动解析
 * @param   rx       重组槽位数组（每个从机一个）
 * @param   slots    槽位个数
 * @param   frag32   32 字节整帧
 * @param   now_ms   当前毫秒（给 200ms 收齐窗口用）
 * @param   out      收齐并解析成功时，结果写这里（可以为 0）
 * @return  RC_OK           收到一片，还没齐
 *          RC_OK_DONE(=1)   收齐并解析成功（out 里有内容）
 *          RC_E_CRC / RC_E_PARAM / RC_E_BUSY ...
 *********************************************************************/
#define RC_KEYMAP_DONE          1u

uint8_t Keymap_Feed(keymap_rx_t *rx, uint8_t slots, const uint8_t *frag32,
                    uint32_t now_ms, rc_keymap_t *out);

/*********************************************************************
 * @fn      Keymap_SlotExpire
 * @brief   扫一遍超时的重组槽（超过 200ms 没齐）
 * @return  第一个超时槽里缺的片位图（0 = 没有超时）
 *********************************************************************/
uint8_t Keymap_SlotExpire(keymap_rx_t *rx, uint8_t slots, uint32_t now_ms);

bool    Keymap_IsComplete(const keymap_rx_t *k);
uint8_t Keymap_MissingBitmap(const keymap_rx_t *k);

/*********************************************************************
 * @fn      Keymap_ParseTlv
 * @brief   把一条完整的 TLV 流解析回 rc_keymap_t
 * @return  RC_OK / RC_E_PARAM / RC_E_LEN
 *********************************************************************/
uint8_t Keymap_ParseTlv(const uint8_t *tlv, uint16_t len, rc_keymap_t *out);

/*********************************************************************
 * @fn      Keymap_BuildMissingReq
 * @brief   造一个"补片请求"帧（RC_KEYMAP_MISS，2B）
 * @param   dev_id   目标从机
 * @param   bitmap   缺哪些片
 * @param   out2     输出 2 字节
 *********************************************************************/
void Keymap_BuildMissingReq(uint8_t dev_id, uint8_t bitmap, uint8_t *out2);

/* 解析补片请求：返回缺片位图（目标从机由 TDMA 时隙决定） */
uint8_t Keymap_ParseMissingReq(const uint8_t *in2);

/*==========================================================================
 * 5. 改名（Host → Device）
 *========================================================================*/

/*********************************************************************
 * @fn      Keymap_BuildRename
 * @brief   组一帧 RC_RENAME_KEY（32B）
 * @param   scene_param  只有 ACT_SCENE 用得到，其它填 0
 * @return  RC_OK / RC_E_LEN（名字超长会被截断并返回 RC_E_LEN）
 * @note    规范 6.9.1 那一版是 24B、名字只有 18B，装不下 KEY_NAME_MAX=19；
 *          而且 `frame_ctrl = 0x40|dev_id` 会和 0x41/0x44 撞。这一版改成
 *          32B、dev_id 单独一个字节、名字放满 19。
 *********************************************************************/
uint8_t Keymap_BuildRename(uint8_t dev_id, uint8_t key_id, uint8_t action,
                           uint8_t scene_param, const char *name, uint8_t name_len,
                           uint8_t *out32);

/*********************************************************************
 * @fn      Keymap_HandleRename
 * @brief   从机侧处理一帧 RC_RENAME_KEY
 * @param   cmd32   收到的 32 字节
 * @param   m       本地配置（会被就地改）
 * @param   ack3    输出 3 字节 RC_RENAME_ACK {0x42, ver, err}
 * @return  RC_OK / RC_E_*（同时也会体现在 ack3[2] 里）
 *********************************************************************/
uint8_t Keymap_HandleRename(const uint8_t *cmd32, rc_keymap_t *m, uint8_t *ack3);

/*********************************************************************
 * @fn      Keymap_ValidateName
 * @brief   名称合法性检查（规范 18.2）
 * @param   len  名称长度
 * @return  RC_OK / RC_E_LEN / RC_E_NAME_INVALID
 * @note    v1.0 不接受 UTF-8（>0x7E 一律拒），也拒绝 '/', '\\', '%'
 *********************************************************************/
uint8_t Keymap_ValidateName(const char *name, uint8_t len);

/*==========================================================================
 * 6. Flash 布局用的辅助（规范 11.2 的 0x026..0x0F4，共 207 字节）
 *
 *   偏移（相对本区起始）  字段                  字节
 *        +0              key_name_len[9]         9
 *        +9              key_name_pool[9][20]   180
 *        +189            key_action[9]            9
 *        +198            key_scene_param[9]       9
 *
 *   整个设备配置记录（含 magic / dev_id / tx_addr / pair_seed / CRC16）
 *   由 rf_cfg.c 负责拼装，本模块只管上面这 207 字节。
 *========================================================================*/
#define RC_FLASH_NAME_OFF       0x026u
#define RC_FLASH_NAME_REGION_LEN 207u

/* 把 9 键的名称/行为压进 207 字节 */
void Keymap_PackNames(const rc_keymap_t *m, uint8_t *out);

/* 从 207 字节还原（长度字段非法时按"没配名字"处理，绝不越界） */
void Keymap_UnpackNames(const uint8_t *in, rc_keymap_t *m);

/*==========================================================================
 * 7. 小工具
 *========================================================================*/

/* 该键是否物理存在 */
static inline bool Key_IsPresent(const rc_key_t *k)
{
    return (k->len != (uint8_t)RC_KEY_LEN_ABSENT);
}

/* 该键有没有起名字 */
static inline bool Key_HasName(const rc_key_t *k)
{
    return (k->len > 0u) && (k->len <= (uint8_t)RC_KEY_NAME_MAX);
}

/* 主机 UI 上该显示什么：有名字用名字，没名字显示 "KEY3" */
void Key_DisplayName(const rc_key_t *k, uint8_t key_id, char *out, uint8_t cap);

#ifdef __cplusplus
}
#endif

#endif /* __LINK_KEYMAP_H__ */
