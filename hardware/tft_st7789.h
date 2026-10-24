/**************************************************************************
 * tft_st7789.h —— ST7789 SPI TFT 屏驱动（核心 + 面板层，合并为一个头文件）
 *
 *   由 Adafruit_ST77xx.h / Adafruit_ST7789.h (Adafruit, MIT License) 移植：
 *     - 去掉 Arduino.h / Print.h / Adafruit_GFX.h / Adafruit_SPITFT.h 依赖
 *     - 去掉 C++ class，改为 C 结构体 + 全局单实例 tft
 *     - boolean -> bool, byte -> uint8_t, word -> uint16_t
 *     - PROGMEM / pgm_read_byte() -> 直接读取 const 数组
 *     - ST7735 / ST7796 等其它面板分支已删除，只保留 ST7789
 *     - 原 tft_st77xx.h(核心) 与 tft_st7789.h(面板) 已合并到本文件
 *
 *   本文件平台无关：GPIO/SPI/延时通过 tft_port.h 的 TPORT_* 接口，
 *   CH592(WCH RISC-V) 与 STM32F103 两个版本共用同一份代码。
 *========================================================================*/

#ifndef __TFT_ST7789_H__
#define __TFT_ST7789_H__

#include <stdint.h>
#include <stdbool.h>
#include "tft_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_CMD_DELAY            0x80    /* 命令表里表示“参数后带延时”的标志位 */

/*--------------------------------------------------------------------------
 * ST77xx 命令
 *------------------------------------------------------------------------*/
#define ST77XX_NOP              0x00
#define ST77XX_SWRESET          0x01
#define ST77XX_RDDID            0x04
#define ST77XX_RDDST            0x09

#define ST77XX_SLPIN            0x10
#define ST77XX_SLPOUT           0x11
#define ST77XX_PTLON            0x12
#define ST77XX_NORON            0x13

#define ST77XX_INVOFF           0x20
#define ST77XX_INVON            0x21
#define ST77XX_DISPOFF          0x28
#define ST77XX_DISPON           0x29
#define ST77XX_CASET            0x2A
#define ST77XX_RASET            0x2B
#define ST77XX_RAMWR            0x2C
#define ST77XX_RAMRD            0x2E

#define ST77XX_PTLAR            0x30
#define ST77XX_TEOFF            0x34
#define ST77XX_TEON             0x35
#define ST77XX_MADCTL           0x36
#define ST77XX_COLMOD           0x3A

#define ST77XX_MADCTL_MY        0x80
#define ST77XX_MADCTL_MX        0x40
#define ST77XX_MADCTL_MV        0x20
#define ST77XX_MADCTL_ML        0x10
#define ST77XX_MADCTL_RGB       0x00

#define ST77XX_RDID1            0xDA
#define ST77XX_RDID2            0xDB
#define ST77XX_RDID3            0xDC
#define ST77XX_RDID4            0xDD

/*--------------------------------------------------------------------------
 * 现成的 16 位 (RGB565) 颜色
 *------------------------------------------------------------------------*/
#define ST77XX_BLACK            0x0000
#define ST77XX_WHITE            0xFFFF
#define ST77XX_RED              0xF800
#define ST77XX_GREEN            0x07E0
#define ST77XX_BLUE             0x001F
#define ST77XX_CYAN             0x07FF
#define ST77XX_MAGENTA          0xF81F
#define ST77XX_YELLOW           0xFFE0
#define ST77XX_ORANGE           0xFC00

/*--------------------------------------------------------------------------
 * 读取 Flash 中的命令表/字库（CH592 无 PROGMEM 概念，直接寻址即可）
 *------------------------------------------------------------------------*/
#define TFT_PGM_READ(p)         (*(const uint8_t *)(p))

/*--------------------------------------------------------------------------
 * 驱动实例（原 C++ class 成员变量）
 *------------------------------------------------------------------------*/
typedef struct
{
    uint8_t  rotation;          /* 当前旋转 0~3                            */

    uint16_t windowWidth;       /* 逻辑宽高（未旋转，如 240x320）           */
    uint16_t windowHeight;
    uint16_t _width;            /* 当前旋转后的宽                          */
    uint16_t _height;           /* 当前旋转后的高                          */

    int16_t  _xstart;           /* (0,0) 原点在显存里的列偏移               */
    int16_t  _ystart;           /* (0,0) 原点在显存里的行偏移               */
    int16_t  _colstart;         /* 面板列方向物理偏移                       */
    int16_t  _rowstart;         /* 面板行方向物理偏移                       */
    int16_t  _colstart2;        /* 旋转 1/2/3 时的右/下侧偏移               */
    int16_t  _rowstart2;

    uint8_t  invertOnCommand;   /* 反显开命令（ST77XX_INVON）               */
    uint8_t  invertOffCommand;  /* 反显关命令（ST77XX_INVOFF）              */
} TFT_t;

extern TFT_t tft;               /* 全局单实例（对应原来 Arduino 的 tft 对象） */

#define TFT_Width()             (tft._width)
#define TFT_Height()            (tft._height)
#define TFT_GetRotation()       (tft.rotation)

/*--------------------------------------------------------------------------
 * 一次 SPI 事务（CS 拉低 -> ... -> CS 拉高）
 *------------------------------------------------------------------------*/
void TFT_BeginWrite(void);

/* 【性能计数】定义在 tft_st7789.c，配合 LOOP_DIAG 观察 */
extern volatile uint32_t g_tft_ops;
extern volatile uint32_t g_tft_px;
/* FW38: >0 = 正在一次 CS 事务里（TMR0 中断靠它决定能不能动屏幕）*/
extern volatile uint8_t  g_tft_busy;
void TFT_EndWrite(void);

/*--------------------------------------------------------------------------
 * 底层字节/命令发送（对应 Adafruit_SPITFT 的 writeCommand）
 *------------------------------------------------------------------------*/
void TFT_WriteCommand(uint8_t cmd);
void TFT_WriteData32(uint32_t dat);     /* 大端：共 4 字节（CASET/RASET 用） */

/* 对应 SPITFT::sendCommand(cmd, data, len)：一条命令 + 若干参数 */
void TFT_SendCommand(uint8_t cmd, const uint8_t *data, uint8_t len);

/* 对应 Adafruit_ST77xx::displayInit()：按命令表初始化 */
void TFT_DisplayInit(const uint8_t *cmdList);

/* 设置像素写入窗口（内部会自动发 CASET/RASET/RAMWR） */
void TFT_SetAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* 像素数据输出（必须在 SetAddrWindow 之后调用，内部不切 CS） */
void TFT_PushColor(uint16_t color);
void TFT_PushColors(const uint16_t *colors, uint32_t count);
void TFT_FillWindow(uint16_t color, uint32_t count);

/* 1bpp 单色位图 -> RGB565 整块刷出（点阵字库用，1 次地址窗口） */
void TFT_DrawMonoBitmap(int16_t x, int16_t y, const uint8_t *data,
                        uint16_t w, uint16_t h, uint16_t stride,
                        uint16_t fg, uint16_t bg);

/* 旋转 / 开关显示 / 睡眠 / TE 信号（TFT_SetRotation 实现在 tft_st7789.c） */
void TFT_SetRotation(uint8_t r);
void TFT_EnableDisplay(bool enable);
void TFT_EnableTearing(bool enable);
void TFT_EnableSleep(bool enable);
void TFT_SetColRowStart(int8_t col, int8_t row);

/* 公共初始化（begin + 可选命令表） */
void TFT_CommonInit(const uint8_t *cmdList);
void TFT_Begin(void);


/*==========================================================================
 * 第二部分：面板层 —— ST7789 专属定义（原 tft_st7789.h）
 *========================================================================*/

/* 现成颜色 */
#define ST7789_BLACK            ST77XX_BLACK
#define ST7789_WHITE            ST77XX_WHITE
#define ST7789_RED              ST77XX_RED
#define ST7789_GREEN            ST77XX_GREEN
#define ST7789_BLUE             ST77XX_BLUE
#define ST7789_CYAN             ST77XX_CYAN
#define ST7789_MAGENTA          ST77XX_MAGENTA
#define ST7789_YELLOW           ST77XX_YELLOW
#define ST7789_ORANGE           ST77XX_ORANGE

/*--------------------------------------------------------------------------
 * 接口
 *------------------------------------------------------------------------*/

/* 对应 Adafruit_ST7789::init(width, height)：初始化并设旋转 0
 *   width/height 只能填屏幕实际像素（支持 240x320 / 240x240 / 135x240）
 *   （旋转用公共接口 TFT_SetRotation()，声明见本文件） */
void ST7789_Init(uint16_t width, uint16_t height);


#ifdef __cplusplus
}
#endif

#endif /* __TFT_ST7789_H__ */