/**************************************************************************
 * kmap_store.h —— 按键映射的运行时存储与 Flash 持久化
 *
 * 【为什么要单独一层】
 *   key_matrix.c 里的映射本来只活在 RAM，默认值来自 const 的 s_map_default，
 *   关机就没了。网页要"改完不烧录也能一直生效"，就必须能存 Flash。
 *
 *   这一层负责：
 *     - 启动时优先从 Flash 载入映射（有就覆盖默认值）
 *     - 运行时改映射（单键/整组/组名/当前组）
 *     - 显式存盘（SAVE_FLASH 命令触发）
 *     - 恢复出厂默认
 *
 * 【Flash 分区（务必别撞车）】
 *   0x0000 ~ 0x00FF   game_score
 *   0x0100 / 0x0200   rf_cfg（A/B 双 Bank）
 *   0x0300 ~ 0x03FF   ui_store（16 字节 + 擦除对齐）
 *   0x0400 ~ 0x04FF   **本模块**（按键映射）     ← 新增，占一个块
 *   0x0500 ~ 0x05FF   rf_cfg 键盘对码记录（RF_CFG_PAGE_KB_OFF）
 *
 *   擦除粒度是 256 字节，所以每个模块各占一个整块，互不干扰。
 *
 * 【记录格式（0x0400 起，v3）】
 *   偏移  长度  内容
 *   0     2     magic = 0x4B4D（'M''K'，小端）
 *   2     2     版本 = 0x0003
 *   4     1     当前映射组 index
 *   5     1     组数（冗余存，读回来校验用）
 *   6     1     每组键数（冗余存）
 *   7     1     保留 = 0
 *   8     90    映射表：5 组 × 9 键 × uint16，小端
 *   98    30    编码器槽（v3 新增）：5 组 × 3 槽（正转/反转/单击）× uint16
 *   128   85    组名：5 × 17 字节（16 字节名 + 结尾 0）
 *   213   2     CRC16-CCITT-FALSE，覆盖 [0, 213)
 *   215   41    补 0 到 256
 *
 *   合计正好 256 字节，一个擦除块装得下。
 *   注：v3 把编码器三个槽加进来了；旧 v2 记录版本号对不上会直接判无效，
 *       落到默认值（KmapStore_Load 返回 false），网页重新保存一次即可。
 **************************************************************************/

#ifndef __KMAP_STORE_H__
#define __KMAP_STORE_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. Flash 布局常量
 *========================================================================*/
#define KMAP_FLASH_OFF          0x0400u     /* 数据 Flash 内偏移 */
#define KMAP_FLASH_ERASE_LEN    256u        /* 擦除粒度 */

#define KMAP_STORE_MAGIC        0x4B4Du     /* 'M' | 'K'<<8 */
#define KMAP_STORE_VER          0x0003u     /* v3：新增编码器槽（正转/反转/单击）；
                                             * v2 = 映射组 4->5；v1 = 最初 4 组 */

/* 记录内各段的偏移 */
#define KMAP_OFF_MAGIC          0u
#define KMAP_OFF_VER            2u
#define KMAP_OFF_CURGROUP       4u
#define KMAP_OFF_GROUPS         5u
#define KMAP_OFF_KEYS           6u
#define KMAP_OFF_MAP            8u          /* 90 字节 = 5 组 × 9 键 × 2 */
#define KMAP_OFF_ENC            98u         /* 30 字节 = 5 组 × 3 槽 × 2（v3） */
#define KMAP_OFF_NAME           128u        /* 85 字节 = 5 × 17 */
#define KMAP_OFF_CRC            213u        /* 2 字节  */
#define KMAP_OFF_END            215u

/* 组名单个最大长度（不含结尾 0） */
#define KMAP_STORE_NAME_MAX     16u
/* 每个组名在记录里占多少字节（含结尾 0） */
#define KMAP_STORE_NAME_SLOT    17u

/*==========================================================================
 * 2. 结果码
 *========================================================================*/
#define KMAP_STORE_OK           0u
#define KMAP_STORE_E_FLASH      1u      /* Flash 读写失败     */
#define KMAP_STORE_E_ARG        2u      /* 参数越界           */
#define KMAP_STORE_E_VERIFY     3u      /* 写入后校验不一致    */

/*==========================================================================
 * 3. 接口
 *========================================================================*/

/*********************************************************************
 * @fn      KmapStore_Init
 * @brief   模块初始化：读一次 Flash，有有效记录就应用，没有就用默认值
 * @note    会调用 KEY_SetMapValue / KEY_SetMapGroup 把数据灌进 key_matrix
 *********************************************************************/
void KmapStore_Init(void);

/*********************************************************************
 * @fn      KmapStore_Load
 * @brief   重新从 Flash 载入并应用
 * @return  true = 读到有效记录
 *********************************************************************/
bool KmapStore_Load(void);

/*********************************************************************
 * @fn      KmapStore_Save
 * @brief   把当前映射（从 key_matrix 现场读取）写进 Flash
 * @return  KMAP_STORE_OK / KMAP_STORE_E_xxx
 * @note    写入后会回读校验；整块擦除再写，保证是原子的
 *********************************************************************/
uint8_t KmapStore_Save(void);

/*********************************************************************
 * @fn      KmapStore_ResetDefault
 * @brief   恢复出厂默认映射（内存 + 组名），并写回 Flash
 * @return  KMAP_STORE_OK / KMAP_STORE_E_xxx
 *********************************************************************/
uint8_t KmapStore_ResetDefault(void);

/* 记录在 Flash 里占多少字节（测试用） */
uint16_t KmapStore_RawSize(void);

/* 取内存里组名（只读）；group 越界返回空串 */
const char *KmapStore_GroupName(uint8_t group);

/*********************************************************************
 * @fn      KmapStore_SetGroupName
 * @brief   改组名（内存，不写 Flash）
 * @param   group  组号 0..(组数-1)
 * @param   name   新名字（最长 KMAP_STORE_NAME_MAX）
 * @return  KMAP_STORE_OK / KMAP_STORE_E_ARG
 *********************************************************************/
uint8_t KmapStore_SetGroupName(uint8_t group, const char *name);

/* 有没有从 Flash 读过有效记录（调试用） */
bool KmapStore_HasFlashRecord(void);

/* 上次操作的结果码（调试用） */
uint8_t KmapStore_LastError(void);

#ifdef __cplusplus
}
#endif

#endif /* __KMAP_STORE_H__ */
