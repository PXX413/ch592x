/**************************************************************************
 * tft_gfx.h —— 图形绘制层（Adafruit_GFX 的原生 C 替代）
 *
 *   原 Adafruit ST77xx 库依赖 Adafruit_GFX 提供绘图/字符功能，
 *   CH592 上没有 Arduino 环境，因此这里把 Adafruit_GFX 里用得到的
 *   核心算法全部用原生 C 重写：
 *       drawPixel / drawFastHLine / drawFastVLine / writeLine / drawRect
 *       fillRect / fillScreen / drawCircle / fillCircle / drawCircleHelper
 *       fillCircleHelper / drawRoundRect / fillRoundRect / drawTriangle
 *       fillTriangle / drawBitmap / drawChar / 字符输出
 *   算法（Bresenham 等）与原库保持一致，保证图形形状、像素位置完全相同。
 *
 *   所有函数操作全局实例 tft（见 tft_st77xx.h）。
 **************************************************************************/

#ifndef __TFT_GFX_H__
#define __TFT_GFX_H__

#include <stdint.h>
#include <stdbool.h>
#include "tft_st7789.h"

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * 颜色
 *------------------------------------------------------------------------*/
#define TFT_RGB565(r, g, b)                 \
    ((uint16_t)((((uint16_t)(r) & 0xF8) << 8) | \
                (((uint16_t)(g) & 0xFC) << 3) | \
                (((uint16_t)(b) >> 3)))

/* 函数版（参数是变量时用这个） */
uint16_t TFT_Color565(uint8_t r, uint8_t g, uint8_t b);

/*--------------------------------------------------------------------------
 * 基本图形
 *------------------------------------------------------------------------*/
void TFT_FillScreen(uint16_t color);
void TFT_DrawPixel(int16_t x, int16_t y, uint16_t color);
void TFT_DrawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
void TFT_DrawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
void TFT_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void TFT_DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void TFT_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

void TFT_DrawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
void TFT_FillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
void TFT_DrawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
void TFT_FillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);

void TFT_DrawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color);
void TFT_FillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color);

/* 单色位图：每行按字节对齐，MSB 在前（与 Adafruit GFX 的取模软件一致）
 * 例：取模方式选“横向取模、字节倒序不勾、C51 格式”即为此格式 */
void TFT_DrawBitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                    int16_t w, int16_t h, uint16_t color);

/* RGB565 位图（每像素 2 字节，大端） */
void TFT_DrawRGBBitmap(int16_t x, int16_t y, const uint16_t *pixels,
                       int16_t w, int16_t h);

/* 反显整个屏幕 */
void TFT_InvertDisplay(bool invert);

/*--------------------------------------------------------------------------
 * 文本（5x8 点阵，支持 1~n 倍放大、自动换行、透明/不透明背景）
 *------------------------------------------------------------------------*/
void TFT_SetCursor(int16_t x, int16_t y);
int16_t TFT_GetCursorX(void);
int16_t TFT_GetCursorY(void);

/* 透明背景（背景色 = 前景色，和 Adafruit_GFX 约定一致） */
void TFT_SetTextColor(uint16_t color);
/* 不透明背景 */
void TFT_SetTextColorBg(uint16_t color, uint16_t bg);

void TFT_SetTextSize(uint8_t size);
uint8_t TFT_GetTextSize(void);
void TFT_SetTextWrap(bool wrap);

void TFT_DrawChar(int16_t x, int16_t y, char ch,
                  uint16_t color, uint16_t bg, uint8_t size);

/* 按当前光标/颜色/字号输出一个字符（自动换行、处理 \n \r） */
void TFT_WriteChar(char ch);
/* 输出字符串 / 格式化输出（TFT_Printf 需要 <stdio.h>，见 README 编译注意事项） */
void TFT_Print(const char *str);
int  TFT_Printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __TFT_GFX_H__ */
