/**************************************************************************
 * key_matrix.h —— CH592X 3x3 矩阵键盘驱动（支持组合按键）
 *
 * 目标芯片 : CH592X / CH592F（WCH RISC-V，MounRiver Studio，-std=gnu99）
 * 依赖     : CH592 EVT 标准外设驱动 CH59x_common.h（GPIO / SYS）
 * 特性     :
 *   1) 逐行扫描 + 上拉输入，无二极管也能用
 *   2) 整幅位图去抖，天然支持"组合按键"（同时按多个键）
 *   3) 组合键判定窗口：按下第一个键后等 KEY_COMBO_WINDOW_MS，
 *      期间又按下别的键就合并成一次"组合键"事件，不会误报成两次单键
 *   4) 组合键查表：位图 → 组合键编号（编号自己定义）
 *   5) 按下/松开边沿事件 + 当前位图 + 按键个数（≥3 键时给出"可能含幽灵键"标志）
 *   6) 非阻塞：由调用者每 KEY_SCAN_PERIOD_MS 调一次 KEY_Scan()
 *
 * 键位编号（K1~K9）：
 *       列1     列2     列3
 *  行1   K1      K2      K3
 *  行2   K4      K5      K6
 *  行3   K7      K8      K9
 *   位图 bit0~bit8 依次对应 K1~K9，宏 KEY_BIT(n) 取第 n 个键的位
 **************************************************************************/

#ifndef __KEY_MATRIX_H__
#define __KEY_MATRIX_H__

#include <stdint.h>
#include <stdbool.h>
#include "CH59x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 引脚配置（按实际接线改这里；portb: 0 = PA，1 = PB）
 *
 *    【重要】CH592X 可用引脚（WCH CH59x_gpio.h 定义）：
 *        PA4~PA15、PB0、PB4、PB6、PB7、PB10~PB15、PB22、PB23
 *      **没有 PB8 / PB9**，也没有 PA0~PA3 —— 写进代码能编译，但引脚不存在。
 *      （下面第 2 节有编译期检查，填错会直接报错）
 *
 *    【重要】PA8/PA9 是 UART1（串口打印）的默认引脚！本驱动把它们当列 2/列 3 用，
 *      所以串口调试要么换脚、要么重映射：
 *        GPIOPinRemap(ENABLE, RB_PIN_UART1);   (UART1 移到 PB12=RXD / PB13=TXD)
 *      （main_example.c 里已经这样处理了；你自己的工程里若还有 UART1 也要一起改）
 *
 *    【警告】PB14/PB15 是芯片的仿真调试口（SWD）。用它们当普通 IO 必须先关掉
 *      调试功能，而关掉后 WCH-Link 可能无法再直接下载（要 ISP 恢复）。
 *      → 如果你还想要调试口，把列 1 换到 PB10 / PA4 / PA5 等空闲脚。
 *========================================================================*/
/* ---- 列：输入 + 上拉，按键按下时被行拉低 ---- */
#ifndef KEY_C1_PORTB
#define KEY_C1_PORTB    1
#endif
#ifndef KEY_C1_PIN
#define KEY_C1_PIN      GPIO_Pin_15     /* PB15（调试口，见上面警告） */
#endif

#ifndef KEY_C2_PORTB
#define KEY_C2_PORTB    0               /* 0 = PA */
#endif
#ifndef KEY_C2_PIN
#define KEY_C2_PIN      GPIO_Pin_9      /* PA9  */
#endif

#ifndef KEY_C3_PORTB
#define KEY_C3_PORTB    0               /* 0 = PA */
#endif
#ifndef KEY_C3_PIN
#define KEY_C3_PIN      GPIO_Pin_8      /* PA8  */
#endif

/* ---- 行：扫描时逐个拉低，其余行保持高阻输入 ---- */
#ifndef KEY_R1_PORTB
#define KEY_R1_PORTB    0               /* 0 = PA */
#endif
#ifndef KEY_R1_PIN
#define KEY_R1_PIN      GPIO_Pin_10     /* PA10 */
#endif

#ifndef KEY_R2_PORTB
#define KEY_R2_PORTB    1               /* 1 = PB */
#endif
#ifndef KEY_R2_PIN
#define KEY_R2_PIN      GPIO_Pin_6      /* PB6  */
#endif

#ifndef KEY_R3_PORTB
#define KEY_R3_PORTB    1               /* 1 = PB */
#endif
#ifndef KEY_R3_PIN
#define KEY_R3_PIN      GPIO_Pin_0      /* PB0  */
#endif

/*==========================================================================
 * 2. 编译期检查：把不存在的引脚挡在编译阶段
 *========================================================================*/
#if ((KEY_C1_PORTB == 0) && (KEY_C1_PIN < GPIO_Pin_4)) || \
    ((KEY_C2_PORTB == 0) && (KEY_C2_PIN < GPIO_Pin_4)) || \
    ((KEY_C3_PORTB == 0) && (KEY_C3_PIN < GPIO_Pin_4)) || \
    ((KEY_R1_PORTB == 0) && (KEY_R1_PIN < GPIO_Pin_4)) || \
    ((KEY_R2_PORTB == 0) && (KEY_R2_PIN < GPIO_Pin_4)) || \
    ((KEY_R3_PORTB == 0) && (KEY_R3_PIN < GPIO_Pin_4))
#error "CH592X 没有 PA0~PA3，请改用 PA4~PA15"
#endif

#if ((KEY_C1_PORTB == 1) && ((KEY_C1_PIN == GPIO_Pin_8) || (KEY_C1_PIN == GPIO_Pin_9))) || \
    ((KEY_C2_PORTB == 1) && ((KEY_C2_PIN == GPIO_Pin_8) || (KEY_C2_PIN == GPIO_Pin_9))) || \
    ((KEY_C3_PORTB == 1) && ((KEY_C3_PIN == GPIO_Pin_8) || (KEY_C3_PIN == GPIO_Pin_9))) || \
    ((KEY_R1_PORTB == 1) && ((KEY_R1_PIN == GPIO_Pin_8) || (KEY_R1_PIN == GPIO_Pin_9))) || \
    ((KEY_R2_PORTB == 1) && ((KEY_R2_PIN == GPIO_Pin_8) || (KEY_R2_PIN == GPIO_Pin_9))) || \
    ((KEY_R3_PORTB == 1) && ((KEY_R3_PIN == GPIO_Pin_8) || (KEY_R3_PIN == GPIO_Pin_9)))
#error "CH592X 没有 PB8/PB9（GPIOB 只有 PB0/PB4/PB6/PB7/PB10~PB15/PB22/PB23），请改用 PB4/PB7/PB10 等"
#endif

/*==========================================================================
 * 3. 时间参数（都是"毫秒"，按 KEY_SCAN_PERIOD_MS 累加计时，不占用定时器）
 *========================================================================*/
#ifndef KEY_SCAN_PERIOD_MS
#define KEY_SCAN_PERIOD_MS      5       /* KEY_Scan() 的调用间隔 */
#endif
#ifndef KEY_DEBOUNCE_MS
#define KEY_DEBOUNCE_MS         15      /* 去抖时间：连续稳定这么久才算按下 */
#endif
#ifndef KEY_COMBO_WINDOW_MS
#define KEY_COMBO_WINDOW_MS     40      /* 组合键窗口：最后一个键按下后再等这么久才上报 */
#endif
#ifndef KEY_ROW_SETTLE_US
#define KEY_ROW_SETTLE_US       30      /* 换行后等待电平稳定的时间（上拉 RC） */
#endif

/*==========================================================================
 * 4. 组合键表（位图 -> 组合键编号，编号自己定义，0 表示"不是组合键"）
 *    只用单键时这张表不生效；要加组合键就在这里加一行。
 *========================================================================*/
#define KEY_BIT(n)              ((uint16_t)(1u << ((n) - 1u)))      /* n = 1~9 */

#ifndef KEY_COMBO_TABLE
#define KEY_COMBO_TABLE                                             \
    { { KEY_BIT(1) | KEY_BIT(2), 0x01 },   /* K1+K2 */               \
      { KEY_BIT(2) | KEY_BIT(3), 0x02 },   /* K2+K3 */               \
      { KEY_BIT(1) | KEY_BIT(3), 0x03 },   /* K1+K3 */               \
      { KEY_BIT(4) | KEY_BIT(5), 0x04 },   /* K4+K5 */               \
      { KEY_BIT(1) | KEY_BIT(9), 0x05 } }  /* K1+K9（对角组合） */
#endif

/*==========================================================================
 * 5. 事件结构
 *========================================================================*/
typedef struct
{
    uint16_t map;       /* 当前稳定按下位图（bit0~bit8 = K1~K9） */
    uint16_t pressed;   /* 本次新按下的键（上升沿） */
    uint16_t released;  /* 本次松开的键（下降沿） */
    uint8_t  count;     /* 当前按下键数 */
    uint8_t  combo;     /* 组合键编号（查表），0 = 不是组合键 */
    uint8_t  ghost;     /* 1 = 本次上报时抑制掉了幽灵键（无二极管矩阵固有现象） */
} KeyEvent_t;

/*==========================================================================
 * 6. 接口
 *========================================================================*/

/* 初始化（上电调用一次）：配置行列 IO，必要时关闭仿真调试口 */
void KEY_Init(void);

/* 扫描一次 —— 主循环或定时器里调用，调用间隔**不再要求等于 5ms**。
 *
 * 【时基】去抖窗（KEY_DEBOUNCE_MS）和组合键窗（KEY_COMBO_WINDOW_MS）
 * 都按"真实毫秒"计时：模块从下面 KEY_SetTimeSource() 注册的函数取时间。
 * 没注册时退回老行为（每次调用算 KEY_SCAN_PERIOD_MS 毫秒），这样把
 * key_matrix.c 单独拿去编译做单元测试也能跑。
 *
 * 以前是"调用次数 × KEY_SCAN_PERIOD_MS"当时间：主循环一圈 ≈1ms 时
 * 去抖 15ms 实际只等了 3ms（快 5 倍），刷屏把一圈拖到几十毫秒时
 * 又变成慢好几倍 —— 两头都不对。 */
void KEY_Scan(void);

/* 时间源钩子：fn 返回"自由运行的毫秒数"（不需要从 0 开始，允许回绕）。
 * 例：  KEY_SetTimeSource(Game_Millis);      // 主循环里真实毫秒
 *       KEY_SetTimeSource(0);                // 恢复内置近似 */
typedef uint32_t (*KeyMsFn_t)(void);
void KEY_SetTimeSource(KeyMsFn_t fn);

/* 取事件：有新事件返回 true 并填充 ev；没有则返回 false（事件被取走后清除） */
bool KEY_GetEvent(KeyEvent_t *ev);

/* 当前稳定位图 / 按键个数 / 单个键是否按下（n = 1~9） */
uint16_t KEY_GetMap(void);
uint8_t  KEY_GetCount(void);
bool     KEY_IsPressed(uint8_t n);

/*********************************************************************
 * @fn      KEY_GhostDropped
 * @brief   最近一次扫描是否抑制掉了幽灵键（1 = 有）。
 *
 * 【幽灵键抑制规则】无二极管的 3x3 矩阵同时按下构成 L 形的三键时
 *（例：2/3/5），第 4 角（6）会被电路误读为按下。现在驱动按**扫描顺序
 * 先到先得**处理：一个格子只有在"它所在的行、列**都**已经被接受"时
 * 才判为幽灵丢弃 —— 于是 2/3/5 正确上报，不再多出 6。
 *
 * 注意：四个真键正好构成矩形时（如 2/3/7/8 一起按），按顺序最后一个
 * 会被当成幽灵丢掉；松开任意一键后其余键会自动恢复。要彻底解决只能
 * 给每个按键串一个二极管。
 *********************************************************************/
uint8_t  KEY_GhostDropped(void);

/*********************************************************************
 * @fn      KEY_BlockInput
 * @brief   屏蔽按键输入一段时间（毫秒）：屏蔽期内扫描结果恒为"无按键"。
 *
 * 典型用法：进键盘页前调 KEY_BlockInput(500)，把切页瞬间手上还按着的键
 *（和触点抖动）挡在 HID 报文之外；之前就按着的键会在去抖窗后正常上报
 * 松开，不会卡住主机。
 *********************************************************************/
void     KEY_BlockInput(uint16_t ms);

/*==========================================================================
 * 【诊断 2026-10-13】按键间歇性锁死的只读探针
 * 复现时看 debounce / raw_read / blocked 三个值就能判断是
 * 去抖被干扰打断，还是扫描被 KEY_BlockInput 跳过。只读，不改行为。
 *========================================================================*/
typedef struct
{
    uint16_t raw_read;      /* 最近一次 key_read_raw() 的原始位图      */
    uint16_t raw_last;      /* 上一次读到、用于比对的位图              */
    uint16_t stable;        /* 去抖后的位图（9 键按下状态看这个）      */
    uint16_t reported;      /* 已经上报过的位图                        */
    uint16_t debounce;      /* 当前位图已稳定多少毫秒（阈值 15）       */
    uint8_t  blocked;       /* 1 = KEY_BlockInput 屏蔽中（扫描被跳过） */
    uint8_t  ghost;         /* 1 = 最近一次丢了幽灵键                  */
} KeyDbg_t;

void     KEY_DbgGet(KeyDbg_t *out);

/* 当前是否处于屏蔽期 */
bool     KEY_IsBlocked(void);

/* 查组合键编号（直接给位图，0 = 不是组合键） */
uint8_t  KEY_ComboLookup(uint16_t map);

/* 位图转字符串，便于调试打印：如 "1+2"、"-"（无键） */
const char *KEY_MapToString(uint16_t map);

#ifdef __cplusplus
}
#endif


/*==========================================================================
 * 按键映射组（用旋转编码器切换，最多 KEY_MAP_GROUPS 组）
 *
 * 每个物理键（K1~K9）在每一组里对应一个 16 位"映射值"：
 * 当 BLE HID 键码、ASCII、或你自己的动作编号都行，默认填的是 HID 键码。
 *
 * 键盘模式下用编码器切组：
 *      case GKEY_ENC_CW:  KEY_MapGroupNext(); break;
 *      case GKEY_ENC_CCW: KEY_MapGroupPrev(); break;
 * 取映射值：
 *      uint16_t v = KEY_GetMapped(n);      // n = 1..9
 *========================================================================*/
#define KEY_MAP_GROUPS      5       /* 3 套默认 + 2 套自定义（需求二.3）；
                                     * 改这里要同步 key_matrix.c 的默认表、
                                     * kmap_store.h 的记录偏移、工具页的 KEY_MAP_GROUPS */
#define KEY_MAP_KEYS        9       /* 每组几个键（3x3 就是 9） */

uint8_t  KEY_GetMapGroup(void);
void     KEY_SetMapGroup(uint8_t group);
void     KEY_MapGroupNext(void);
void     KEY_MapGroupPrev(void);
void     KEY_MapGroupByEncoder(int8_t step);    /* >0 下一组，<0 上一组，0 不动 */
const char *KEY_GetMapGroupName(uint8_t group); /* 组名（ASCII，屏幕显示用） */

uint16_t KEY_GetMapped(uint8_t key);            /* 当前组下 1..9 键的映射值 */
uint16_t KEY_GetMapValue(uint8_t group, uint8_t key);

void     KEY_SetMapValue(uint8_t group, uint8_t key, uint16_t value);
void     KEY_SetMapRow(uint8_t group, const uint16_t *values);
void     KEY_ResetMap(void);

/*==========================================================================
 * 编码器映射槽（2026-10-13 新增）
 *
 * 需求："编码器的正反转和单击也能设置键值"。原来只有 3x3 九个键能配，
 * 编码器三个动作是写死的。现在每个映射组里另开 3 个槽：
 *
 *      which = KEY_ENC_CW   (1) 正转（顺时针）
 *      which = KEY_ENC_CCW  (2) 反转（逆时针）
 *      which = KEY_ENC_CLICK(3) 单击
 *
 * 值 = 16 位映射值（高字节修饰键位图 + 低字节 HID Usage，和九个键同格式）。
 * 默认：单击 = 0x28（回车，与旧行为一致），正反转 = 0（不发键）。
 *
 * 存取路径：网页协议 kmap_proto 的 0x15/0x16 命令 <-> 这两个函数
 *          <-> kmap_store 的 Flash 记录（改完 0x30 SAVE_FLASH 掉电不丢）。
 *========================================================================*/
#define KEY_ENC_CW      1u      /* 正转 */
#define KEY_ENC_CCW     2u      /* 反转 */
#define KEY_ENC_CLICK   3u      /* 单击 */
#define KEY_ENC_SLOTS   3u

uint16_t KEY_GetEncMapValue(uint8_t group, uint8_t which);  /* 指定组 */
uint16_t KEY_GetEncMapped(uint8_t which);                   /* 当前组 */
void     KEY_SetEncMapValue(uint8_t group, uint8_t which, uint16_t value);

/*********************************************************************
 * @fn      KEY_MapValueHook
 * @brief   【给你留的钩子】想动态决定映射时，在别的 .c 里实现这个函数即可
 * @note    默认实现直接返回 value；它是弱符号(weak)，你在任何地方重写它
 *          就能接管全部映射，不用改 key_matrix.c
 *********************************************************************/
uint16_t KEY_MapValueHook(uint8_t group, uint8_t key, uint16_t value);

/*********************************************************************
 * @fn      KEY_MapGroupNameHook
 * @brief   组名钩子。KEY_GetMapGroupName() 会先问这个函数。
 * @param   group  组号
 * @return  组名；返回 0 表示不接管，用内置的 s_map_name
 * @note    默认弱符号实现返回 0（行为不变）。
 *          网页在线改映射时，由 keyboard/kmap_store.c 提供强符号实现，
 *          返回 RAM 里可写的组名副本。
 *********************************************************************/
const char *KEY_MapGroupNameHook(uint8_t group);
/* 按键(1~9) -> 行列(1~3)，屏上显示坐标用 */
uint8_t KEY_GetRow(uint8_t n);
uint8_t KEY_GetCol(uint8_t n);

#endif /* __KEY_MATRIX_H__ */
