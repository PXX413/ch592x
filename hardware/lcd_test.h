/**************************************************************************
 * lcd_test.h —— CH592X 屏幕测试程序
 *
 * 接线（默认，可在 tft_port.h 里改）：
 *     LCD SCL/SCK  -> PA13
 *     LCD SDA/MOSI -> PA14
 *     LCD CS       -> PA12
 *     LCD DC/A0    -> PA11
 *     LCD RES/RST  -> PA15
 *     LCD BLK/LED  -> PB4（高电平关闭背光；TFT_USE_BL=1 时由驱动控制）
 *     LCD VCC      -> 3.3V      LCD GND -> GND
 *
 * 测试内容：
 *     LCD_Test_Info()      屏上打出运行信息（分辨率 / 旋转 / SPI 方式 / 实测满屏速度）
 *     LCD_Test_BringUp()   整屏 红 -> 绿 -> 蓝（接线自检）
 *     LCD_Test_Colors()    8 色条 + 文字 + 图形（看颜色顺序、偏移、字形）
 *     LCD_Test_Animate()   四个方向依次转动 + 小方块扫描 + 逐行刷屏
 *     LCD_Test_Benchmark() 实测满屏填色耗时（用 SysTick 实测，不猜频率）
 *     LCD_Test_Run()       整套跑一遍并循环（不返回）
 *
 * 用法（在 User/Main.c 里）：
 *     #include "lcd_test.h"
 *     ...
 *     LCD_Test_Run(135, 240);          // 135x240 / 240x240 / 240x320 按你的屏填
 *
 * 注意：
 *     * 默认走**软件 SPI**（tft_port.h 的 TFT_SPI_BACKEND），任意 IO 都能用；
 *       想换硬件 SPI0（TFT_SPI_HARD）要确认 SCLK/MOSI 落在 SPI0 那组脚上，
 *       并把 StdPeriphDriver 里的 CH59x_spi0.c 取消排除再编译；
 *     * 背光/复位脚如果落在 PB14/PB15 这类调试脚上，下载调试会受影响（见 tft_port.h 第 6 节）。
 **************************************************************************/

#ifndef __LCD_TEST_H__
#define __LCD_TEST_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 系统时钟(60MHz) + 屏幕初始化 + 开背光 */
void LCD_Test_Init(uint16_t w, uint16_t h);

/* 屏上显示运行信息（分辨率 / 旋转 / SPI 方式 / 实测满屏速度） */
void LCD_Test_Info(void);

/* 上电自检：整屏 红 -> 绿 -> 蓝，每色 500ms */
void LCD_Test_BringUp(void);

/* 8 色条 + 文字 + 图形 */
void LCD_Test_Colors(void);

/* 四个方向依次转动 + 小方块扫描 + 逐行刷屏 */
void LCD_Test_Animate(void);

/* 实测满屏填色耗时（毫秒），并把结果打到屏上 */
uint32_t LCD_Test_Benchmark(void);

/* 整套跑一遍并循环（不返回） */
void LCD_Test_Run(uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_TEST_H__ */
