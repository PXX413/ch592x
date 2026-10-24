/**************************************************************************
 * battery.h —— CH592X 电池电压 / 电量检测（带分压器使能控制）
 *
 * 目标芯片 : CH592X / CH592F（WCH RISC-V，MounRiver Studio，-std=gnu99）
 * 依赖     : CH592 EVT 标准外设驱动 CH59x_common.h（GPIO / SYS / ADC）
 *
 * 测量流程（按你的要求）：
 *     PB14(VBAT_EN) 推挽输出高  ->  等 500ms  ->  读 ADC  ->  换算成 mV
 *     ->  用 OCV 表查电量(%)  ->  关断 VBAT_EN（默认低，省电）
 *
 * 【重要 1】PB13 在 CH592X 上不能做 ADC 输入！
 *     CH592X 的 ADC 引脚只有这 12 个：
 *        AIN0=PA4   AIN1=PA5   AIN2=PA12  AIN3=PA13
 *        AIN4=PA14  AIN5=PA15  AIN8=PB0   AIN9=PB6
 *        AIN10=PA6  AIN11=PA7  AIN12=PA8  AIN13=PA9
 *     （AIN6/AIN7 在本封装上没有引出）
 *     你的 LCD 用 PA11~PA15、键盘用 PA8/PA9/PA10/PB0/PB6/PB15，
 *     所以空闲且能测 ADC 的脚是：PA4 / PA5 / PA6 / PA7  —— 这里按引脚定义用 PA7（AIN11）。
 *
 * 【重要 2】PB14 是芯片的仿真调试口（SWD/TIO）。
 *     用它做普通 IO 必须先关掉调试功能（本驱动自动置 RB_DEBUG_EN），
 *     关掉后 WCH-Link 可能无法再直接下载，需要用 ISP 方式恢复。
 *     想保住调试口就把 VBAT_EN 换到 PB10 / PB11 / PB4 / PB7 / PA5 等空闲脚。
 **************************************************************************/

#ifndef __BATTERY_H__
#define __BATTERY_H__

#include <stdint.h>
#include <stdbool.h>
#include "CH59x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 引脚与 ADC 通道配置（按实际接线改这里）
 *========================================================================*/
/* VBAT_EN：控制分压器的 MOS/三极管，推挽输出，默认低电平（关断） */
#ifndef BAT_EN_PORTB
#define BAT_EN_PORTB        1               /* 0 = PA，1 = PB */
#endif
#ifndef BAT_EN_PIN
#define BAT_EN_PIN          GPIO_Pin_14     /* PB14（调试口，见文件头警告） */
#endif

/* VBAT_ADC：电池分压后的电压输入脚（必须是 ADC 引脚，且通道号要与引脚一致） */
#ifndef BAT_ADC_PORTB
#define BAT_ADC_PORTB       0               /* 0 = PA，1 = PB */
#endif
#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN         GPIO_Pin_7      /* PA7 = AIN11 */
#endif
#ifndef BAT_ADC_CHANNEL
#define BAT_ADC_CHANNEL     CH_EXTIN_11     /* PA7 = AIN11 */
#endif

/*==========================================================================
 * 2. 分压/参考/校准参数
 *========================================================================*/
/* 【电池电压 = ADC 引脚电压 × NUM / DEN】—— 分压比的倒数，换电阻只改这里
 *
 *   分压网络            比值        这里填
 *   100k + 100k          1/2         2 / 1        （当前硬件）
 *   100k + 91k           0.4764      2099 / 1000  （= 191/91，换成这个只需改这两行）
 *   200k + 100k          1/3         3 / 1
 *
 * 之所以让 BAT_ReadMv() 直接给出**电池电压**、而不是引脚电压：这样
 * battery_ocv_table 就能保持电池厂商给的 OCV 量程（3200~4200mV），
 * 换分压电阻时只动这两行，不必重新生成那张 101 行的表。
 *
 * 注意 100k+91k 时要把电池侧接 100k、地侧接 91k；接反了分压比变成 0.524，
 * 4.2V 会被压到 2.2V 超出 ADC 量程（削顶）。 */
#ifndef BAT_VBAT_MUL_NUM
#define BAT_VBAT_MUL_NUM    2
#endif
#ifndef BAT_VBAT_MUL_DEN
#define BAT_VBAT_MUL_DEN    1
#endif

/* CH592 外部通道的 ADC 参考电压 = 模拟电源 VDD33。
 * **本机 VDD33 = 2.5V，所以这里必须是 2500。**
 * 它决定 raw -> mV 的比例，填错会整体等比例偏：
 * 实际 2.5V 却填 3300 时读数虚高 3300/2500 = 32%（4.2V 电池会被算成 5.5V，
 * 电量直接顶到 100%）。
 * 它不是精密基准：VDD33 波动会直接影响读数，要精确就用下面两个校准宏，
 * 或者改用内部电池通道（见 README）。 */
#ifndef BAT_ADC_VREF_MV
#define BAT_ADC_VREF_MV     2500u
#endif
#ifndef BAT_ADC_FULL_SCALE
#define BAT_ADC_FULL_SCALE  4096u           /* 12 位 ADC */
#endif

/* 线性校准：结果 mV = mV × GAIN_NUM / GAIN_DEN + OFFSET_MV
 *
 * 【2026-10-12 本机实测标定（结论）】
 *     电池万用表 4040mV ；PA7 万用表 2020mV（分压比正好 0.5）；ADC 原始值 3959
 *     按标称算应是 2020 × 4096 / 2500 = 3311  ->  实测/理论 = 1.196
 *   已逐项排除：
 *     * ADC 粗调校准 ADC_DataCalib_Rough()：旁路前后读数完全一样，
 *       说明它在本机返回 ≈0（芯片健康），不是偏移来源；
 *     * 通道配置：CH_EXTIN_11 = AIN11 = PA7（与硬件一致）；
 *     * PGA = ADC_PGA_0（0dB 增益）、12 位满量程 4096、单端模式；
 *     * VDD33 实测为标准 2.50V。
 *   结论：本颗 ADC 在"外部通道 + 0dB + 3.2M 采样率"下的**实际刻度比标称高
 *   约 19.6%**（等效满量程 2090mV），属器件实际特性 —— 用增益校准收掉：
 *       2090 / 2500 = 0.836  ->  836 / 1000
 *   +1mV 偏移用于抵消整数截断：2416 × 836/1000 = 2019.8 截成 2019，
 *   会让 4.04V 显示 83%，加 1mV 正好落回表里的 84%。
 *
 *   注意：这是**逐芯片**参数。换芯片/换批次建议按同样方法重标一次：
 *     电池万用表电压 + PA7 万用表电压 + 屏上（UIW_BAT_SHOW_RAW=1）原始值
 *     -> 等效满量程 = PA7mV × 4096 / 原始值 -> 增益 = 等效满量程 / 2500。 */
#ifndef BAT_CAL_GAIN_NUM
#define BAT_CAL_GAIN_NUM    836
#endif
#ifndef BAT_CAL_GAIN_DEN
#define BAT_CAL_GAIN_DEN    1000
#endif
#ifndef BAT_CAL_OFFSET_MV
/* +1mV：三步除法改成四舍五入后仍会残留 ±1mV，加 1mV 让"正好落在表格行上"
 * 的点（如 4.10V、3.80V）落到正确的百分比档，而不是差 1%。 */
#define BAT_CAL_OFFSET_MV   1
#endif

/* 是否把 SDK 的 ADC 粗调校准值（ADC_DataCalib_Rough()）计入换算。
 *   1 = 官方 EVT 的做法：raw = 采样值 + 粗调值（本机实测该值≈0，用 1 即可）
 *   0 = 不使用（值仍会取出到 BAT_GetCalibRaw()）
 * 标定时把它设 0 验证过：读数一字未变，故确认它不是偏差来源。 */
#ifndef BAT_ADC_USE_ROUGH_CAL
#define BAT_ADC_USE_ROUGH_CAL   1
#endif

/*==========================================================================
 * 3. 时间与采样参数
 *========================================================================*/
#ifndef BAT_EN_SETTLE_MS
#define BAT_EN_SETTLE_MS    500u            /* 使能分压器后等待时间（你要求 0.5 秒） */
#endif
#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES     8u              /* ADC 平均次数（去抖） */
#endif
#ifndef BAT_EN_LEAVE_ON
#define BAT_EN_LEAVE_ON     0               /* 0 = 读完关断 VBAT_EN 省电；1 = 保持导通
                                             * （标定电压时才需要设 1，见 BAT_CAL_GAIN_NUM） */
#endif

/*==========================================================================
 * 4. 编译期检查：ADC 通道号必须与 ADC 引脚一一对应
 *    （CH592X 的 AIN 编号不是连续的，填错会读到别的通道）
 *========================================================================*/
#if (BAT_ADC_PORTB == 0)
#  if (BAT_ADC_PIN == GPIO_Pin_4)  && (BAT_ADC_CHANNEL != CH_EXTIN_0)
#    error "PA4 对应 AIN0，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_0"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_5)  && (BAT_ADC_CHANNEL != CH_EXTIN_1)
#    error "PA5 对应 AIN1，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_1"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_6)  && (BAT_ADC_CHANNEL != CH_EXTIN_10)
#    error "PA6 对应 AIN10，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_10"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_7)  && (BAT_ADC_CHANNEL != CH_EXTIN_11)
#    error "PA7 对应 AIN11，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_11"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_8)  && (BAT_ADC_CHANNEL != CH_EXTIN_12)
#    error "PA8 对应 AIN12，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_12"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_9)  && (BAT_ADC_CHANNEL != CH_EXTIN_13)
#    error "PA9 对应 AIN13，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_13"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_12) && (BAT_ADC_CHANNEL != CH_EXTIN_2)
#    error "PA12 对应 AIN2，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_2"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_13) && (BAT_ADC_CHANNEL != CH_EXTIN_3)
#    error "PA13 对应 AIN3，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_3"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_14) && (BAT_ADC_CHANNEL != CH_EXTIN_4)
#    error "PA14 对应 AIN4，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_4"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_15) && (BAT_ADC_CHANNEL != CH_EXTIN_5)
#    error "PA15 对应 AIN5，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_5"
#  endif
#else
#  if (BAT_ADC_PIN == GPIO_Pin_0)  && (BAT_ADC_CHANNEL != CH_EXTIN_8)
#    error "PB0 对应 AIN8，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_8"
#  endif
#  if (BAT_ADC_PIN == GPIO_Pin_6)  && (BAT_ADC_CHANNEL != CH_EXTIN_9)
#    error "PB6 对应 AIN9，请把 BAT_ADC_CHANNEL 设为 CH_EXTIN_9"
#  endif
#endif

/* 这两个脚在 CH592X 上根本没有 ADC 功能，直接挡掉 */
#if ((BAT_ADC_PORTB == 1) && ((BAT_ADC_PIN == GPIO_Pin_13) || (BAT_ADC_PIN == GPIO_Pin_15) || \
                              (BAT_ADC_PIN == GPIO_Pin_14) || (BAT_ADC_PIN == GPIO_Pin_12) || \
                              (BAT_ADC_PIN == GPIO_Pin_4)  || (BAT_ADC_PIN == GPIO_Pin_7)  || \
                              (BAT_ADC_PIN == GPIO_Pin_10) || (BAT_ADC_PIN == GPIO_Pin_11)))
#error "该 PB 引脚在 CH592X 上没有 ADC 功能；可用 ADC 脚只有 PA4/PA5/PA6/PA7/PA8/PA9/PA12/PA13/PA14/PA15/PB0/PB6"
#endif

/*==========================================================================
 * 5. OCV 表（电压 mV -> 电量 %），由 battery.c 提供，用户可自行增删
 *========================================================================*/
extern const uint16_t battery_ocv_table[][2];
extern const uint8_t  battery_ocv_table_len;

/*==========================================================================
 * 6. 接口
 *========================================================================*/

/* 上电初始化：VBAT_EN 推挽输出（默认低）；ADC 脚浮空输入；初始化 ADC 并取校准值 */
void BAT_Init(void);

/* 手动控制分压器使能（1 = 导通，0 = 关断） */
void BAT_Enable(uint8_t on);

/* -------- 非阻塞采样（主循环里用；**不要在中断里调**） --------
 *
 * 分压器上电后要等 BAT_EN_SETTLE_MS 才稳定，所以 BAT_ReadMv() 里有一句
 * DelayMs(500) —— 运行中每 5 分钟调一次就会把主循环卡住半秒。
 * 要"定时刷新电量"就用下面这一对，自己拿状态机等：
 *
 *      BAT_SampleStart();                     // 打开使能，开始计时
 *      ... 主循环里每圈 ...
 *      if (BAT_SamplePoll(Game_Millis()))     // 到点才采样，不阻塞
 *      { mv = BAT_GetLastMv(); pct = BAT_GetLastPercent(); }
 *
 * 没到点、不在采样中、已经采过，Poll 都返回 false。
 * 时间由调用方传进来，battery.c 不依赖任何时基。 */
void BAT_SampleStart(void);
bool BAT_SamplePoll(uint32_t now_ms);

/* -------- 核心接口（每次调用都会走完整流程，耗时约 BAT_EN_SETTLE_MS） -------- */

/* 【你要求的函数】读一次电池电量，返回 0~100（%） */
uint8_t BAT_ReadPercent(void);

/* 读一次电压，返回 mV。
 * 注意量程：返回的是 **ADC 引脚电压**（0~2500mV，电池电压 = 该值 ×2），
 * 因为 1/2 分压那个系数按 battery.h 的说明交给了 OCV 表承担。 */
uint16_t BAT_ReadMv(void);

/* -------- 不碰硬件的纯换算与管理接口 -------- */

/* 用 OCV 表把 mV 换算成 %（带线性插值，1% 精度；超出范围按端点值）
 * 入参量程与表一致 = **引脚电压**（1600~2100mV）。
 * 例：2100mV -> 100%，1600mV -> 0%，1850mV -> 50%。 */
uint16_t BAT_MvToPercent(uint16_t mv);

/* 最近一次测量的结果（不重新测） */
uint16_t BAT_GetLastMv(void);
uint8_t  BAT_GetLastPercent(void);

/* 最近一次 ADC 原始值（调试用） */
uint16_t BAT_GetLastRaw(void);

/* SDK 粗调校准函数返回的原始值（调试用；正常应在 0 附近） */
int16_t  BAT_GetCalibRaw(void);

#ifdef __cplusplus
}
#endif

#endif /* __BATTERY_H__ */
