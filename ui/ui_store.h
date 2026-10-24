/**************************************************************************
 * ui_store.h —— UI 设置项持久化（数据 Flash，掉电不丢失）
 *
 * 依据：文档「（五）设置」
 *   * 键盘灯开关
 *   * 键盘灯亮度（步进 5，范围 0~100）
 *   * 休眠时间（5 / 10 / 20 / 60 分钟 / 永不）
 *   * 键值映射组（3 套默认 + 2 套自定义）
 *
 * 【数据 Flash 分区 —— 千万别撞车】
 *   0x0000 ~ 0x00FF   game_score（最高分 + 难度/音效）
 *   0x0100 / 0x0200   rf_cfg（2.4G 遥控配置，A/B 双 Bank）
 *   0x0300            **本模块**
 **************************************************************************/

#ifndef __UI_STORE_H__
#define __UI_STORE_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 取值域（文档里的硬性数字）
 *========================================================================*/
#define UI_SLEEP_5MIN        0
#define UI_SLEEP_10MIN       1
#define UI_SLEEP_20MIN       2
#define UI_SLEEP_60MIN       3
#define UI_SLEEP_NEVER       4
#define UI_SLEEP_COUNT       5

#define UI_BRIGHT_MIN        0
#define UI_BRIGHT_MAX        100

/* 屏幕亮度（BLK/PB4 的 PWM）：可调范围 10~100，步长 5。
 * 下限 10 而不是 0：0 会把背光完全关掉，屏幕全黑之后什么都看不见、
 * 连设置页都摸不回来。 */
#define UI_SCR_BRIGHT_MIN    10
#define UI_SCR_BRIGHT_MAX    100
#define UI_SCR_BRIGHT_STEP   5
#define UI_BRIGHT_STEP       5

#define UI_MAPGROUP_COUNT    5      /* 3 套默认 + 2 套自定义 */

/* 键盘灯效预置数（和 rgb_led.h 的 RGB_FX_PRESET_COUNT 必须一致，
 * ui_pages.c 里有编译期断言盯着） */
#define UI_LEDFX_COUNT       5

/*==========================================================================
 * 2. 配置项
 *========================================================================*/
typedef struct
{
    uint8_t led_on;             /* 键盘灯开关             */
    uint8_t led_bright;         /* 0..100                 */
    uint8_t scr_bright;         /* 屏幕亮度 0..100（实际最小 5） */
    uint8_t sleep_idx;          /* UI_SLEEP_xxx           */
    uint8_t map_group;          /* 0..UI_MAPGROUP_COUNT-1 */
    uint8_t led_fx;             /* 0..UI_LEDFX_COUNT-1   灯效预置下标 */
    uint8_t key_hint;           /* 0/1 按键提示：按下哪个键哪个灯亮白 */
} ui_cfg_t;

/*==========================================================================
 * 3. 接口
 *========================================================================*/

/*********************************************************************
 * @fn      UiStore_Init
 * @brief   从数据 Flash 读一次设置（带 magic + CRC16 校验）
 * @note    校验不过就用出厂默认值，并且**不写 Flash**（等用户真改了再写）
 *********************************************************************/
void UiStore_Init(void);

/* 当前配置（可直接改字段，改完调 UiStore_Save()） */
ui_cfg_t *UiStore_Cfg(void);

/* 重新从 Flash 读一遍；返回 true = 读到了有效记录 */
bool UiStore_Load(void);

/*********************************************************************
 * @fn      UiStore_Save
 * @brief   把当前配置写进数据 Flash
 * @return  UI_STORE_OK / UI_STORE_E_FLASH
 *********************************************************************/
#define UI_STORE_OK         0u
#define UI_STORE_E_FLASH    1u

uint8_t UiStore_Save(void);

/* 恢复出厂默认（内存里改完不写 Flash；要落盘自己再调 Save） */
void UiStore_Default(void);

/*==========================================================================
 * 4. 小工具（页面用）
 *========================================================================*/

/* 休眠档位 -> 分钟数；UI_SLEEP_NEVER 返回 0 表示不休眠 */
uint16_t UiStore_SleepMinutes(uint8_t idx);

/* 列表上下选：dir = +1 / -1，循环 */
uint8_t  UiStore_NextSleep(uint8_t idx, int8_t dir);

/* 亮度加减（步进 5，自动夹在 0..100） */
uint8_t  UiStore_BrightStep(uint8_t cur, int8_t dir);
uint8_t  UiStore_ScrBrightStep(uint8_t cur, int8_t dir);   /* 屏幕亮度：10~100 步长 5 */

/* 键值映射组的显示名下标（页面拿去查字库），循环 */
uint8_t  UiStore_NextMapGroup(uint8_t idx, int8_t dir);

/* 灯效下标上下选：dir = +1 / -1，循环 */
uint8_t  UiStore_NextLedFx(uint8_t idx, int8_t dir);

/* 配置记录在 Flash 里占多少字节（测试用） */
uint16_t UiStore_RawSize(void);

/* 调试/测试用：把内存里正在用的记录取出来看 */
const void *UiStore_Raw(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_STORE_H__ */
