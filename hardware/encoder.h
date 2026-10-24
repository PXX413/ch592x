/**************************************************************************
 * encoder.h —— CH592X 带按键旋转编码器驱动（EC11 类增量式编码器）
 *
 * 目标芯片 : CH592X / CH592F（WCH RISC-V，MounRiver Studio，-std=gnu99）
 * 依赖     : CH592 EVT 标准外设驱动 CH59x_common.h（只用 GPIO + SYS）
 *
 * 硬件连接（默认，可在下面改）：
 *     BTN_O -> PA6   按键（开关）输出
 *     A     -> PA5   编码器 A 相
 *     B     -> PA4   编码器 B 相
 *     编码器公共脚一般接 GND（所以 A/B/按键都配上拉输入，闭合时读到 0）
 *     建议 A/B/按键 各并 100nF 电容到地做硬件滤波（软件里也有消抖）
 *
 * 返回值约定（对应你的要求）：
 *     ENC_ButtonPressed()  -> 按下返回 1，没按返回 0
 *     ENC_GetStep()        -> 正转一格返回 +1，反转一格返回 -1，没转动返回 0
 *
 *     【注意】"+1" 与 "1" 数值相同，所以必须用两个函数分别返回：
 *        "转了多少格" 用 ENC_GetStep()，"按键是否按下" 用 ENC_ButtonPressed()。
 *        想要一个函数同时给出两类事件，用 ENC_ReadEvent()（结构体里分开）。
 *
 * 工作原理（正交状态机，天然抗抖动）：
 *     把 A、B 两根线的电平组成 2 位状态 (A<<1)|B，相邻两个状态之间的
 *     合法跳变按 "顺时针 +1 / 逆时针 -1" 累加（跳过中间状态的非法跳变算 0，
 *     抖动产生的来回跳变会自己抵消）。累加满 ±4 就是一个"格"（一个完整
 *     正交周期），于是输出 ±1 —— 既不会一格报 4 次，也不怕接触抖动。
 *
 * 使用：
 *     ENC_Init();
 *     while (1)
 *     {
 *         ENC_Scan();                      // 每 1ms 调一次（主循环或 1ms 定时器里）
 *         int8_t step = ENC_GetStep();     // 取走累计格数
 *         if (step != 0) { ... 正转/反转 ... }
 *         if (ENC_ButtonPressed()) { ... 按键按着 ... }
 *         DelayMs(1);
 *     }
 **************************************************************************/

#ifndef __ENCODER_H__
#define __ENCODER_H__

#include <stdint.h>
#include <stdbool.h>
#include "CH59x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. 引脚配置（portb: 0 = PA，1 = PB）
 *========================================================================*/
/* 按键：BTN_O */
#ifndef ENC_BTN_PORTB
#define ENC_BTN_PORTB       0               /* 0 = PA */
#endif
#ifndef ENC_BTN_PIN
#define ENC_BTN_PIN         GPIO_Pin_6      /* PA6 */
#endif

/* A 相 */
#ifndef ENC_A_PORTB
#define ENC_A_PORTB         0
#endif
#ifndef ENC_A_PIN
#define ENC_A_PIN           GPIO_Pin_5      /* PA5 */
#endif

/* B 相 */
#ifndef ENC_B_PORTB
#define ENC_B_PORTB         0
#endif
#ifndef ENC_B_PIN
#define ENC_B_PIN           GPIO_Pin_4      /* PA4 */
#endif

/* 按键有效电平：0 = 按下时读到高电平（按键接 VCC，内部下拉）★ 本板实测：PA6 高电平按下，所以是 0
 *               0 = 按下时读到高电平（按键接 VCC，内部下拉） */
#ifndef ENC_BTN_ACTIVE_LOW
#define ENC_BTN_ACTIVE_LOW  0
#endif

/* A/B 相有效电平：一般也是"闭合到地 = 低电平"，所以用上拉输入 */
#ifndef ENC_AB_ACTIVE_LOW
#define ENC_AB_ACTIVE_LOW   1
#endif

/* 方向取反：装上去发现"正转变成 -1"就把这个改成 1（换 A/B 线也行） */
#ifndef ENC_DIR_INVERT
#define ENC_DIR_INVERT      0
#endif

/*==========================================================================
 * 2. 消抖与步进参数
 *========================================================================*/
#ifndef ENC_SCAN_MS
#define ENC_SCAN_MS         1               /* ENC_Scan() 的调用间隔（毫秒） */
#endif
#ifndef ENC_BTN_DEBOUNCE_MS
#define ENC_BTN_DEBOUNCE_MS 5               /* 按键消抖时间 */
#endif
#ifndef ENC_DETENT_COUNT
#define ENC_DETENT_COUNT    4               /* 一个"格"包含几个正交跳变（EC11 通常 4） */
#endif

/*==========================================================================
 * 3. 关于"转太快会不会丢"
 *    默认 1ms 轮询足够应付人手转动（一个格 = 4 个跳变 = 至少 2ms 电平变化，
 *    1ms 采样不会漏）。如果装在电机/旋钮上转得极快，可以改用中断：
 *      1) 自己把 A/B 两根线配成 GPIO 中断（注意：CH592 每个引脚只能选
 *         上升沿 / 下降沿 / 高电平 / 低电平 其中一种，所以 A、B 分别配
 *         上升沿即可，一个周期能拿到 2 次中断）
 *      2) 中断里调用 ENC_HandleIRQ()
 *    中断方式下一次中断只走半个正交周期，所以把 ENC_DETENT_COUNT 改成 2。
 *========================================================================*/

/*==========================================================================
 * 4. 事件结构（一个函数同时给出两类事件时用）
 *========================================================================*/
typedef struct
{
    int8_t  step;       /* 本次新增的格数：正 +1，反 -1，没动 0 */
    uint8_t btn;        /* 当前按键状态：按下 1，松开 0 */
    uint8_t btn_event;  /* 本次边沿：1 = 刚按下，2 = 刚松开，0 = 无 */
} EncEvent_t;

/*==========================================================================
 * 5. 接口
 *========================================================================*/

/* 初始化：A/B/按键配成上拉（或下拉）输入，复位状态 */
void ENC_Init(void);

/* 每 ENC_SCAN_MS（默认 1ms）调用一次：读 A/B 状态机 + 按键消抖 */
void ENC_Scan(void);

/*--------------------------------------------------------------------------
 * 【要求 2】取走累计的"格数"
 *   正转一格 -> +1，反转一格 -> -1，没转动 -> 0
 *   （取走后清零；如果两次调用之间转了好几格，会一次返回累计值）
 *------------------------------------------------------------------------*/
int8_t ENC_GetStep(void);

/*--------------------------------------------------------------------------
 * 【要求 1】按键是否按下：按下返回 1，松开返回 0（已消抖）
 *------------------------------------------------------------------------*/
uint8_t ENC_ButtonPressed(void);

/* 按键边沿事件：1 = 本次刚按下，2 = 本次刚松开，0 = 无（读后清零） */
uint8_t ENC_GetButtonEvent(void);

/* 一次取走两类事件（结构体里分开，避免 "+1 与 1" 混淆） */
void ENC_ReadEvent(EncEvent_t *ev);

/* 累计位置（每 +1/-1 累加，可以当作"菜单光标"用） */
int16_t ENC_GetPosition(void);
void    ENC_SetPosition(int16_t pos);

/* 调试用：原始 A/B/按键电平与正交状态 */
uint8_t ENC_GetRawAB(void);         /* 返回 (A<<1)|B */
uint8_t ENC_GetRawButton(void);     /* 返回引脚原始电平 0/1 */

/* 中断方式下：在 GPIO_A_IRQHandler 里调用它 */
void ENC_HandleIRQ(void);

#ifdef __cplusplus
}
#endif

#endif /* __ENCODER_H__ */
