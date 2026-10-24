/**************************************************************************
 * rf_cfg.h —— 配置持久化（数据 Flash，掉电不丢）
 *
 * 依据：规范 第 11 章（Flash 布局）、第 18.3 章（双 Bank 防掉电损坏）
 *
 * CH570Q（从机，遥控模式）记录 = 256 字节：
 *     0x000 magic 'RC'(0x5243)      0x002 config_ver
 *     0x003 dev_id                  0x004 tx_addr[5]
 *     0x009 pair_seed               0x00B hw_id[4]
 *     0x00F hw_ver                  0x010 sw_ver
 *     0x011 dev_name_len            0x012 dev_name[20]
 *     0x026 key_name 区（207B，见 link_keymap.h）
 *     0x0F5 key_mask                0x0F6 reserved[8]
 *     0x0FE crc16
 *
 * CH570Q（从机，键盘模式）配对记录 = 32 字节：
 *     0x000 magic 'KB'(0x4B42)      0x002 paired
 *     0x003 addr[5]                 0x008 channel
 *     0x009 hop[3]                  0x00C nkro_en
 *     0x00D led_state               0x00E crc16
 *
 * 两个 Bank（A/B）轮流写：先写 A 再写 B，读的时候谁的 CRC 对用谁。
 * 这样"写一半掉电"最多丢掉最后一次改动，绝不会读出乱码。
 *
 * 数据 Flash 偏移：工程里 game_score 占了 0x0000~0x00FF，
 * 所以这里从 0x0100 开始用。
 **************************************************************************/

#ifndef __RF_CFG_H__
#define __RF_CFG_H__

#include <stdint.h>
#include <stdbool.h>
#include "proto_common.h"
#include "link_hop.h"
#include "link_keymap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 数据 Flash 页偏移（相对数据 Flash 基址 0x00070000） */
#define RF_CFG_PAGE_A_OFF       0x0100u     /* 遥控主机记录 bank A */
#define RF_CFG_PAGE_B_OFF       0x0200u     /* 遥控主机记录 bank B */
#define RF_CFG_PAGE_LEN         256u

/* 键盘（从机侧）对码记录单独占一页。
 * 【修复 P2-4】它原来与 RF_CFG_PAGE_A_OFF 共用 0x0100 且同样是整页擦写，
 * 两边互相抹除。现在挪到自己一页，与其它模块也不冲突：
 *     0x0000 game_score / 0x0100 RC-A / 0x0200 RC-B
 *     0x0300 ui_store   / 0x0400 kmap_store / 0x0500 本记录 */
#define RF_CFG_PAGE_KB_OFF      0x0500u

#define RF_CFG_MAGIC_RC         0x5243u     /* 'R','C' */
#define RF_CFG_MAGIC_KB         0x4B42u     /* 'K','B' */

/*==========================================================================
 * 遥控模式从机配置
 *========================================================================*/
typedef struct
{
    uint8_t  config_ver;
    uint8_t  dev_id;                /* 0 = 还没被枚举               */
    uint8_t  tx_addr[RF_ADDR_LEN];
    uint16_t pair_seed;
    uint8_t  hw_id[4];
    uint8_t  hw_ver;
    uint8_t  sw_ver;
    rc_keymap_t km;                 /* 设备名 + 9 键名称 + 行为     */
    uint16_t key_mask;              /* bit0..8 = K1..K9 物理存在 */
    uint8_t  valid;                 /* 读了之后：1 = Flash 里有有效记录 */
} rf_cfg_rc_t;

/*********************************************************************
 * @fn      RfCfg_RcLoad
 * @brief   读遥控配置（A/B 两个 Bank 谁 CRC 对用谁）
 * @return  true = 读到了有效记录；false = 用默认值（已经填好出厂默认）
 *********************************************************************/
bool RfCfg_RcLoad(rf_cfg_rc_t *cfg);

/*********************************************************************
 * @fn      RfCfg_RcSave
 * @brief   双 Bank 保存（先 A 后 B），写完回读校验
 * @return  RC_OK / RC_E_FLASH
 *********************************************************************/
uint8_t RfCfg_RcSave(const rf_cfg_rc_t *cfg);

/* 把一份记录序列化成 256 字节（给上层/测试用） */
uint16_t RfCfg_RcPack(const rf_cfg_rc_t *cfg, uint8_t *out, uint16_t cap);

/* 从 256 字节反序列化；CRC 不对返回 false */
bool RfCfg_RcUnpack(const uint8_t *in, uint16_t len, rf_cfg_rc_t *cfg);

/* 出厂默认 */
void RfCfg_RcDefault(rf_cfg_rc_t *cfg);

/*==========================================================================
 * 键盘模式配对记录
 *========================================================================*/
typedef struct
{
    uint8_t paired;
    uint8_t addr[RF_ADDR_LEN];
    uint8_t channel;
    uint8_t hop[HOP_KB_COUNT];
    uint8_t nkro_en;
    uint8_t led_state;
    uint8_t valid;
} rf_cfg_kb_t;

bool    RfCfg_KbLoad(rf_cfg_kb_t *cfg);
uint8_t RfCfg_KbSave(const rf_cfg_kb_t *cfg);
void    RfCfg_KbDefault(rf_cfg_kb_t *cfg);

/*==========================================================================
 * 底层（PC 测试时可以替换掉）
 *========================================================================*/
uint16_t RfCfg_Crc(const uint8_t *p, uint16_t n);

/* 一次页擦 + 写（内部关中断） */
void RfCfg_PageWrite(uint16_t off, const uint8_t *data, uint16_t len);
/* 读一页 */
void RfCfg_PageRead(uint16_t off, uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __RF_CFG_H__ */
