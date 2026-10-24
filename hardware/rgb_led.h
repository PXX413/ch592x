/**************************************************************************
 * rgb_led.h —— CH592X 键盘 RGB 灯效驱动（3x3 蛇形排列的 WS2812 类灯珠）
 *
 * 目标芯片 : CH592X / CH592F（WCH RISC-V，MounRiver Studio，-std=gnu99）
 * 依赖     : CH592 EVT 标准外设驱动 CH59x_common.h（GPIO / SYS / PWM）
 *
 * 硬件连接（默认）：
 *     RGB_EN  -> PB13   推挽输出，高电平有效（打开灯珠供电/电平转换）
 *     DIN     -> PB7   WS2812 数据线（PB7 = PWM9，用硬件 PWM 产生波形）
 *     VCC/GND -> 5V(或 3.3V，看灯珠) / GND
 *
 * 灯的排布：3x3 = 9 颗，**蛇形（S 形）排列，从左上角开始**：
 *
 *        逻辑行列            物理序号（DIN 串联顺序）
 *      (0,0) (0,1) (0,2)        0 -> 1 -> 2
 *                                        |
 *      (1,0) (1,1) (1,2)        5 <- 4 <- 3
 *                               |
 *      (2,0) (2,1) (2,2)        6 -> 7 -> 8
 *
 *     RGB_PhysIndex(row, col) 就是做这个换算的（第 2、4、6… 行方向相反）
 *
 * 三个主要功能（对应你的要求）：
 *     1) RGB_Switch()         —— 灯效总开关（同时控制 RGB_EN 与灯效运行）
 *     2) RGB_ShowEffect()     —— 传入不同数值 = 输出不同灯效（见 RGB_EFF_xxx）
 *        RGB_EffectTask()     —— 每 20ms 调一次，推进动画并刷新到 PB7
 *     3) RGB_KeyWhite() / RGB_UpdateByKeyMap()
 *                             —— 按下某个键，对应位置的灯变白
 *                                （键位：从上到下、每排从左到右）
 **************************************************************************/

#ifndef __RGB_LED_H__
#define __RGB_LED_H__

#include <stdint.h>
#include <stdbool.h>
#include "CH59x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 引脚配置
 *========================================================================*/
#ifndef RGB_EN_PORTB
#define RGB_EN_PORTB        1               /* 0 = PA，1 = PB */
#endif
/* 【注意】不要改回 PA7：PA7 是电池采样 ADC（BAT_ADC_PIN）。
 *   RGB_Init() 会把本脚配成推挽输出，配在 PA7 上会把 ADC 输入拉死，
 *   电池电压恒读 0。引脚以《CH592X引脚定义》为准：键盘灯控制 = PB13。 */
#ifndef RGB_EN_PIN
#define RGB_EN_PIN          GPIO_Pin_13      /* PB13，高电平有效 */
#endif

/* DIN 固定在 PB7 = PWM9，改脚要同时改 PWM 通道（CH_PWM9 与对应寄存器一起改） */
#define RGB_DIN_PORTB       1
#define RGB_DIN_PIN         GPIO_Pin_7      /* PB7 = PWM9 */

/*==========================================================================
 * 2. 灯珠与亮度
 *========================================================================*/
#ifndef RGB_LED_ROWS
#define RGB_LED_ROWS        3
#endif
#ifndef RGB_LED_COLS
#define RGB_LED_COLS        3
#endif
#define RGB_LED_COUNT       (RGB_LED_ROWS * RGB_LED_COLS)

/* 全局亮度上限（0~255）。9 颗灯全白满亮约 9*60mA = 540mA，
 * 5V 供电/线径/电池都可能扛不住，所以默认限制在 40（约 1/6 亮度）。 */
#ifndef RGB_MAX_BRIGHT
#define RGB_MAX_BRIGHT      40
#endif

/*==========================================================================
 * 3. WS2812 时序（单位 = PWM 计数，1 计数 = Tsys = 1/60MHz ≈ 16.67ns）
 *    WS2812B: 位周期 1.25us，'0' 高电平 0.4us，'1' 高电平 0.8us
 *    如果你换成 SK6812/其它灯珠，按数据手册改这三个数即可。
 *========================================================================*/
/* 【2026-10-13 按《CH592X_WS2812B_Software_Driver_Development_Guide.md》对齐】
 *   §12：ARR = 74（周期 75 计数 @60MHz = 1.25us），CCR 取 0 / 26 / 52
 *   所以 T0H=26、T1H=52、位周期 75，和文档完全一致。
 *
 * 【为什么不照 §2 表里的 0.295/0.595】
 *   那张表是 WS2812C / 新版灯珠的规格；经典 WS2812B 的 T1H 窗口是 0.65~0.95us，
 *   发 0.595us 会被判成 0 码 —— 所有 1 码全丢，反而更不亮。
 *   这里按 §12 的 CCR 数值走（0.433/0.867us），经典和新版都能落进窗口。
 *========================================================================*/
#ifndef RGB_BIT_TICKS

#define RGB_BIT_TICKS       75              /* 1.25us @60MHz */
#endif
#ifndef RGB_T0H_TICKS
#define RGB_T0H_TICKS       26              /* 0.433us @60MHz —— 文档 §12 的 CCR=26 */
#endif
#ifndef RGB_T1H_TICKS
#define RGB_T1H_TICKS       52              /* 0.867us @60MHz —— 文档 §12 的 CCR=52 */
#endif
/* rgb_wait_until() 轮询循环自身的开销（周期数），发之前先扣掉。
 * 现场有示波器时按实测 ±2 微调（文档 §11.1 的验收项）。 */
/* rgb_wait_until() 的兜底圈数：正常只需等 75 个计数，
 * 给 4000 圈余量足够；时基异常时也不会死循环。 */
#ifndef RGB_WAIT_GUARD
#define RGB_WAIT_GUARD      4000u
#endif

#ifndef RGB_WAIT_CYCLES
#define RGB_WAIT_CYCLES     5
#endif

#ifndef RGB_RESET_US
#define RGB_RESET_US        300             /* 帧间复位低电平（>50us） */
#endif

/* 发送时是否关中断（保证时序不被中断打断；一帧约 0.3ms） */
#ifndef RGB_TX_IRQ_LOCK
#define RGB_TX_IRQ_LOCK     1
#endif

/* 位周期等待方式：1 = 轮询 PWM 周期结束标志（推荐）、0 = 空循环延时 */
#ifndef RGB_TX_POLL_FLAG
#define RGB_TX_POLL_FLAG    1
#endif
#ifndef RGB_BIT_WAIT_LOOPS
#define RGB_BIT_WAIT_LOOPS  20              /* 仅当 RGB_TX_POLL_FLAG=0 时使用 */
#endif

/* 可选调试钩子：每写一次 PWM 占空比都会调用（默认空宏，编译后无开销）
 * host 端仿真时把它接到"解码器"上，就能把发给灯珠的字节流还原出来 */
#ifndef RGB_DUTY_HOOK
#define RGB_DUTY_HOOK(duty)     ((void)0)
#endif

/*==========================================================================
 * 4. 灯效编号（传给 RGB_ShowEffect()）
 *========================================================================*/
#define RGB_EFF_OFF             0   /* 全灭 */
#define RGB_EFF_WHITE           1   /* 全部常亮白色 */
#define RGB_EFF_RED_BREATH      2   /* 红色呼吸（整体渐变） */
#define RGB_EFF_RAINBOW         3   /* 彩虹流动 */
#define RGB_EFF_RUNNING         4   /* 跑马灯（单灯沿蛇形顺序跑） */
#define RGB_EFF_BLINK           5   /* 整屏闪烁（约 2Hz） */
#define RGB_EFF_KEY_WHITE       6   /* 【按键跟随】只有按下的键亮白，其余灭 */
#define RGB_EFF_KEY_WHITE_RAIN  7   /* 按键亮白 + 底色彩虹（好看一点） */

/* ---- 下面是给 3x3 键盘专门做的 5 种预置灯效（设置页里选）---- */
#define RGB_EFF_FX_BREATH       8   /* 呼吸：整盘青蓝渐变          */
#define RGB_EFF_FX_CHASE        9   /* 跑马：3 颗白色拖尾沿蛇形跑  */
#define RGB_EFF_FX_RIPPLE      10   /* 涟漪：从中心一圈圈往外扩    */
#define RGB_EFF_FX_WAVE        11   /* 波浪：蓝色波峰沿灯带扫过    */
#define RGB_EFF_FX_RAINBOW     12   /* 彩虹：整盘流动              */
#define RGB_EFF_MAX            12

/* 预置灯效数量（设置页用它做下标，和 ui_store.h 的 UI_LEDFX_COUNT 一致） */
#define RGB_FX_PRESET_COUNT     5

/* 预置下标 0..4 -> RGB_EFF_FX_xxx */
uint8_t RGB_EffectOfPreset(uint8_t idx);

/*==========================================================================
 * 5. 接口
 *========================================================================*/

/* 上电初始化：RGB_EN 拉低（灯先关）、配置 PWM9(PB7)、清空缓冲区 */
void RGB_Init(void);

/*--------------------------------------------------------------------------
 * 功能 1：灯效总开关
 *   on = 1：RGB_EN 输出高（灯珠上电），恢复当前灯效并立即刷新一帧
 *   on = 0：先把所有灯刷成灭，再拉低 RGB_EN（避免残留）
 *------------------------------------------------------------------------*/
void RGB_Switch(uint8_t on);
uint8_t RGB_IsOn(void);

/*--------------------------------------------------------------------------
 * 功能 2：灯效驱动输出 —— 传入不同数值输出不同灯效
 *   参数 effect：RGB_EFF_xxx
 *   调用后立即刷新一帧；之后由 RGB_EffectTask() 持续推进动画
 *------------------------------------------------------------------------*/
void RGB_ShowEffect(uint8_t effect);
uint8_t RGB_GetEffect(void);

/* 动画推进：每 20ms 调用一次（内部按 20ms 累加计时，不占用定时器） */
void RGB_EffectTask(void);

/* 把当前缓冲区（9 颗灯的 GRB 数据）通过 PB7 发出去；一帧约 0.3ms（阻塞） */
void RGB_Show(void);

/*--------------------------------------------------------------------------
 * 功能 3：按键 → 对应灯变白（键位：从上到下、每排从左到右）
 *   row/col 从 0 开始：row0 = 最上面一排，col0 = 最左边一个
 *------------------------------------------------------------------------*/
void RGB_KeyWhite(uint8_t row, uint8_t col, uint8_t on);

/* 批量：直接把 3x3 键盘的位图铺到灯上
 *   keymap 的 bit0~bit8 依次是 K1~K9（与 key_matrix 模块的位图一致：
 *   K1=左上、K2=中上、K3=右上、K4=第二排左 … K9=右下） */
void RGB_UpdateByKeyMap(uint16_t keymap);

/*--------------------------------------------------------------------------
 * 底层小工具（调试/自定义灯效用）
 *------------------------------------------------------------------------*/
void     RGB_SetPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void     RGB_SetAll(uint8_t r, uint8_t g, uint8_t b);
void     RGB_Clear(void);
uint8_t  RGB_PhysIndex(uint8_t row, uint8_t col);   /* 逻辑(行,列) -> 物理序号 */

/* 缓冲区指针（r,g,b 顺序，共 27 字节），可以直接改后调用 RGB_Show() */
uint8_t *RGB_GetBuffer(void);

#ifdef __cplusplus
}
#endif

#endif /* __RGB_LED_H__ */

/*==========================================================================
 * 亮度调节（0 ~ RGB_BRIGHT_MAX，等比缩放 RGB 值）
 *   100 = 原样，0 = 全灭；调亮度不会改变颜色比例（不会偏色）
 *========================================================================*/
#ifndef RGB_BRIGHT_MAX
#define RGB_BRIGHT_MAX      100u
#endif

void    RGB_SetBrightness(uint8_t level);   /* 0~100，超范围自动夹到 100；立即刷新 */
uint8_t RGB_GetBrightness(void);
void    RGB_BrightnessUp(uint8_t step);     /* 调亮，step=0 时按 10 走 */
void    RGB_BrightnessDown(uint8_t step);   /* 调暗 */
