/**************************************************************************
 * tft_port.h —— ST7789 驱动在 CH592 上的硬件抽象层
 *
 * 目标芯片 : CH592X / CH592F / CH591 (WCH RISC-V, QingKe V4C)
 * 开发环境 : MounRiver Studio (GNU RISC-V 工具链, -std=gnu99)
 * 依赖     : CH592 EVT 的 StdPeriphDriver —— CH59x_common.h (GPIO / SPI / SYS)
 * 替代对象 : 原 Adafruit_SPITFT.h / Adafruit_SPITFT_Macros.h 中
 *            digitalWrite() / pinMode() / SPI 相关部分
 *
 * 【本文件是唯一需要按你的实际接线修改的文件】
 *   1) 选择 SPI 后端：软件模拟 or 硬件 SPI0
 *   2) 修改 6 个引脚定义
 *
 * 说明：
 *   - CH592 的 SPI0 主机模式下 SCS 不是硬件片选（手册：主机模式片选可用
 *     任意 IO），因此 CS/DC/RST 全部用普通 GPIO 控制。
 *   - 所有 GPIO 操作使用 CH592 EVT 原生 API：
 *     GPIOA_ModeCfg() / GPIOA_SetBits() / GPIOA_ResetBits()
 *     （注意：CH59x 驱动里没有 GPIOA_WritePort()，见 README 的“踩坑”章节）
 **************************************************************************/

#ifndef __TFT_PORT_H__
#define __TFT_PORT_H__

#include <stdint.h>

/* CH592 EVT 标准外设驱动总头文件：包含 CH592SFR.h / core_riscv.h /
 * CH59x_gpio.h / CH59x_spi.h / CH59x_sys.h，并定义 DelayMs()/DelayUs() 宏 */
#include "CH59x_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 1. SPI 后端选择
 *========================================================================*/
#define TFT_SPI_SOFT    0   /* 软件模拟 SPI（任意 IO，默认，工程无需额外配置） */
#define TFT_SPI_HARD    1   /* 硬件 SPI0（快 5~10 倍，需使能 CH59x_spi0.c）      */

#ifndef TFT_SPI_BACKEND
#define TFT_SPI_BACKEND   TFT_SPI_HARD
#endif

/* 硬件 SPI 时钟分频：SCK = Fsys / TFT_HW_SPI_DIV
 *   Fsys = 60MHz 时： 2 -> 30MHz   3 -> 20MHz   4 -> 15MHz   6 -> 10MHz   8 -> 7.5MHz
 *
 * 【2026-10-12】默认由 4(15MHz) 提到 3(20MHz)，用来压缩 UI 整页重画时间：
 *   240x135 全屏 = 64800 字节，15MHz 要 34.6ms，20MHz 只要 25.9ms（省 8.6ms）。
 *
 * !! 注意 ST7789 串口写周期 t_SCYCW 典型 66ns，约合 15MHz —— 20MHz 属于【超规格使用】。
 *    多数模组实测能跑，但一旦出现花屏 / 错行 / 颜色错位 / 偶发白屏，
 *    第一个该回退的就是这里：把下面的 3 改成 4（或 6），其它代码一行都不用动。
 *    排线较长、接触不良（含 FPC 虚接）时更容易出问题。
 *
 *   DIV = 2 时驱动内部会自动打开 RB_SPI_MST_DLY_EN 高速补偿位；
 *   3 / 4 / 6 / 8 都走普通路径。                                     */
#ifndef TFT_HW_SPI_DIV
/* 【2026-10-14 提速】3 -> 2。
 *
 * 本工程用软件逐字节写 SPI，实测有效速率约 487KB/s，一次整屏重画
 * （240x135x2 = 64800 字节）要 【133ms】 —— 而 BLE 连接间隔只有
 * 7.5~11.25ms。这 133ms 里协议栈完全得不到服务，主机会因为 ATT
 * 服务发现超时把链路拆掉（现场 6<->23 循环，且关掉绑定后循环更快，
 * 正说明是连上之后立刻被堵死）。
 *
 * 分频 2 => SCK = Fsys/2 = 30MHz（SPI0 硬件上限），整屏约 67ms。
 * 若 ST7789 模块走线扛不住（花屏/闪点），退回 3。 */
#define TFT_HW_SPI_DIV    2
#endif

/* 软件 SPI 每个半周期插入的 NOP 个数（0 = 全速，约 3~6MHz @60MHz 主频）
 * 杜邦线较长 / 屏幕花屏时改成 1 或 2 降速即可。                      */
#ifndef TFT_SW_SPI_NOP
#define TFT_SW_SPI_NOP    0
#endif

/*==========================================================================
 * 2. 引脚映射（默认全部在 PA 口，与 SPI0 复用功能脚对齐）
 *
 *    LCD 模块丝印   ST7789 信号              CH592X 引脚
 *    ----------------------------------------------------------
 *    VCC            +3.3V                   3V3
 *    GND            GND                     GND
 *    SCL/SCK/CLK    SCLK (时钟)             PA13  (SPI0 SCK0)
 *    SDA/MOSI/DIN   MOSI (数据)             PA14  (SPI0 MOSI)
 *    CS/CE          CS   (片选, 低有效)     PA12  (SPI0 SCS, 当普通 IO 用)
 *    DC/A0/RS       DC   (命令/数据选择)    PA11
 *    RES/RST        RST  (复位, 低有效)     PA15  (SPI0 MISO, 本屏不用)
 *    BLK/LED        BL   (背光)             PB4, 高电平点亮（实测标定）
 *
 *   注意：PA10/PA11 通常是 32.768kHz 晶振脚（X32KI/X32KO）。
 *         如果你的板子上焊了 32K 晶振，就不要把 DC 放在 PA11（换成 PA4/PA5 等空闲脚）。
 *
 *   端口编号： 0 = GPIOA(PA)   1 = GPIOB(PB)
 *   掩码使用 EVT 的 GPIO_Pin_x 宏
 *   （这些宏都带 #ifndef 保护，也可以在 MounRiver 的工程预处理宏里覆盖）
 *========================================================================*/
#ifndef TFT_PIN_CS_PORT
#define TFT_PIN_CS_PORT     0
#endif
#ifndef TFT_PIN_CS
#define TFT_PIN_CS          GPIO_Pin_12     /* 片选      */
#endif
#ifndef TFT_PIN_SCLK_PORT
#define TFT_PIN_SCLK_PORT   0
#endif
#ifndef TFT_PIN_SCLK
#define TFT_PIN_SCLK        GPIO_Pin_13     /* 时钟      */
#endif
#ifndef TFT_PIN_MOSI_PORT
#define TFT_PIN_MOSI_PORT   0
#endif
#ifndef TFT_PIN_MOSI
#define TFT_PIN_MOSI        GPIO_Pin_14     /* 数据 MOSI */
#endif
#ifndef TFT_PIN_DC_PORT
#define TFT_PIN_DC_PORT     0
#endif
#ifndef TFT_PIN_DC
#define TFT_PIN_DC          GPIO_Pin_11     /* 命令/数据 (PA11) */
#endif
#ifndef TFT_PIN_RST_PORT
#define TFT_PIN_RST_PORT    0
#endif
#ifndef TFT_PIN_RST
#define TFT_PIN_RST         GPIO_Pin_15     /* 复位 (PA15) */
#endif
#ifndef TFT_PIN_BL_PORT
#define TFT_PIN_BL_PORT     1
#endif
#ifndef TFT_PIN_BL
#define TFT_PIN_BL          GPIO_Pin_4      /* 背光 PB4（极性见 TFT_BL_ACTIVE_HIGH） */
#endif

/* 没有接 RST 脚（模块内部已上拉）时置 0；没有接背光控制时置 0 */
#ifndef TFT_USE_RST
#define TFT_USE_RST         1
#endif
/* 背光：本板 PB4 接了背光控制，必须为 1。
 * 之前是 0，导致 TPORT_Bl() 退化成空函数、PB4 悬空
 * （见《CH592X_Bug修复与优化方案.md》P2-7）。 */
#ifndef TFT_USE_BL
#define TFT_USE_BL          1
#endif

/* 背光电平极性（**实测标定**，改这一行就能翻转）：
 *     1 = 高电平点亮背光   （本板实测：PB4 给高 -> 背光亮）
 *     0 = 低电平点亮背光
 *
 * 注意：CH592X引脚定义.txt / lcd_test.h 里写的是"BLK 高电平关闭"，
 * 与实测**相反**。以实测为准 —— 曾经按文档改成"低电平点亮"，
 * 结果上电背光直接灭了。以后换屏或改背光驱动电路，
 * 若行为反过来，只改这里取反即可，不用动驱动逻辑。 */
#ifndef TFT_BL_ACTIVE_HIGH
#define TFT_BL_ACTIVE_HIGH  1
#endif

/* 软件 SPI 的 IO 翻转速度：0 -> 5mA，1 -> 20mA（更快，EMI 略大） */
#ifndef TFT_GPIO_DRIVE_20MA
#define TFT_GPIO_DRIVE_20MA 1
#endif

/*==========================================================================
 * 3. 可选调试钩子：每次 GPIO 翻转都会调用（默认空宏，编译后无任何开销）
 *    主机端仿真/逻辑分析仪替代：-include xx.h 里定义 TFT_PIN_HOOK 即可。
 *    例： #define TFT_PIN_HOOK(pin,level)  my_hook((pin),(level))
 *========================================================================*/
#ifndef TFT_PIN_HOOK
#define TFT_PIN_HOOK(pin, level)   ((void)0)
#endif

/* GPIO 操作宏：portb 为编译期常量，条件会被编译器优化掉 */
#if (TFT_GPIO_DRIVE_20MA)
#define TFT_GPIO_OUT_MODE   GPIO_ModeOut_PP_20mA
#else
#define TFT_GPIO_OUT_MODE   GPIO_ModeOut_PP_5mA
#endif

#define TFT_GPIO_SET(portb, mask)                                          \
    do {                                                                   \
        if (portb) { GPIOB_SetBits(mask); } else { GPIOA_SetBits(mask); }   \
        TFT_PIN_HOOK((mask), 1);                                           \
    } while (0)

#define TFT_GPIO_CLR(portb, mask)                                          \
    do {                                                                   \
        if (portb) { GPIOB_ResetBits(mask); } else { GPIOA_ResetBits(mask); } \
        TFT_PIN_HOOK((mask), 0);                                           \
    } while (0)

#define TFT_GPIO_OUT_CFG(portb, mask)                                      \
    do {                                                                   \
        if (portb) { GPIOB_ModeCfg((mask), TFT_GPIO_OUT_MODE); }            \
        else       { GPIOA_ModeCfg((mask), TFT_GPIO_OUT_MODE); }            \
    } while (0)

/*==========================================================================
 * 4. 编译期检查：硬件 SPI 模式下 SCK/MOSI 必须是 SPI0 的功能脚
 *========================================================================*/
#if (TFT_SPI_BACKEND == TFT_SPI_HARD)
#  if !((TFT_PIN_SCLK_PORT == 0 && TFT_PIN_SCLK == GPIO_Pin_13) || \
        (TFT_PIN_SCLK_PORT == 1 && TFT_PIN_SCLK == GPIO_Pin_13))
#    error "硬件 SPI0: SCLK 必须接 PA13 (默认) 或 PB13 (需重映射), 请修改 tft_port.h"
#  endif
#  if !((TFT_PIN_MOSI_PORT == 0 && TFT_PIN_MOSI == GPIO_Pin_14) || \
        (TFT_PIN_MOSI_PORT == 1 && TFT_PIN_MOSI == GPIO_Pin_14))
#    error "硬件 SPI0: MOSI 必须接 PA14 (默认) 或 PB14 (需重映射), 请修改 tft_port.h"
#  endif
#endif

/*==========================================================================
 * 5. 延时兼容层
 *    CH592 EVT 的 CH59x_common.h 里已经有:
 *        #define DelayMs(x)  mDelaymS(x)
 *        #define DelayUs(x)  mDelayuS(x)
 *    这里再做一次兜底，缺头文件时也能编译。
 *========================================================================*/
#ifndef DelayMs
#define DelayMs(x)   mDelaymS(x)
#endif
#ifndef DelayUs
#define DelayUs(x)   mDelayuS(x)
#endif

/*==========================================================================
 * 6. 仿真调试口冲突检查
 *    PB14 / PB15 默认是芯片的仿真调试口（SWD），要用它们做普通 IO，
 *    必须关闭调试功能：本文件会自动置 R16_PIN_ALTERNATE.RB_DEBUG_EN；
 *    如果仍然无效，请在 MounRiver 下载配置里取消勾选 "Debug Interface"。
 *    （用 SPI0 重映射组 PB12~PB15 时一定要注意这一点！）
 *========================================================================*/
#define TFT_PIN_IS_DEBUG_PIN(portb, mask)                                  \
    (((portb) == 1) && (((mask) == GPIO_Pin_14) || ((mask) == GPIO_Pin_15)))

#define TFT_USES_DEBUG_PINS                                                \
    (TFT_PIN_IS_DEBUG_PIN(TFT_PIN_SCLK_PORT, TFT_PIN_SCLK) ||              \
     TFT_PIN_IS_DEBUG_PIN(TFT_PIN_MOSI_PORT, TFT_PIN_MOSI) ||              \
     TFT_PIN_IS_DEBUG_PIN(TFT_PIN_CS_PORT, TFT_PIN_CS) ||                  \
     TFT_PIN_IS_DEBUG_PIN(TFT_PIN_DC_PORT, TFT_PIN_DC))

/*==========================================================================
 * 7. 供驱动层调用的接口（实现在 tft_port.c）
 *========================================================================*/

/* 只配置 IO 方向/电平（不发 SPI 数据），复位脚要用它先把 RST 拉低 */
void TPORT_InitPins(void);

/* 初始化引脚 + SPI（软件或硬件）。每次 ST7789_Init() 调用一次 */
void TPORT_Init(void);

/* 片选 / DC / 复位 / 背光 电平控制，level: 1 = 高，0 = 低 */
void TPORT_Cs(uint8_t level);
void TPORT_Dc(uint8_t level);
void TPORT_Rst(uint8_t level);
void TPORT_Bl(uint8_t level);

/*********************************************************************
 * @fn      TPORT_BlPwmInit
 * @brief   把 BLK(PB4) 从"普通 GPIO 开关"切成 PWM7 输出，5kHz
 *
 * PB4 在 CH592 上固定是 PWM7（见 EVT EXAM\PWMX\src\Main.c 的引脚注释）。
 * 周期 = 12000 计数 @60MHz = 200us = 5kHz。
 * 注意 PWM7 属于 PWM4-9 组，和 PWM9 共用 R32_PWM_REG_CYCLE —— 所以
 * keyboard/rgb_led.c 那边已经把 WS2812 的 DIN 改回纯 GPIO，不再占 PWM9。
 *********************************************************************/
void TPORT_BlPwmInit(void);

/*********************************************************************
 * @fn      TPORT_BlSetBright
 * @brief   设屏幕亮度 10~100（步长 5，与设置页 "屏幕亮度" 一致；小于 10 一律按 10 处理）
 *********************************************************************/
void TPORT_BlSetBright(uint8_t pct);

/*********************************************************************
 * @fn      TPORT_BlGetBright
 *********************************************************************/
uint8_t TPORT_BlGetBright(void);

/* SPI 收发（只写，不读；ST7789 写屏不需要 MISO） */
void TPORT_WriteByte(uint8_t dat);
void TPORT_WriteBytes(const uint8_t *buf, uint16_t len);

/* 复位时序 + 背光开（驱动初始化里会调用） */
void TPORT_HardwareReset(void);

#ifdef __cplusplus
}
#endif

#endif /* __TFT_PORT_H__ */
