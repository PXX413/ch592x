/**************************************************************************
 * tft_gfx.c —— 图形绘制层（Adafruit_GFX 的原生 C 实现）
 *
 *   算法全部移植自 Adafruit_GFX.cpp (Adafruit Industries, BSD License)：
 *       Adafruit_GFX::drawPixel / writeLine / drawFastHLine / drawFastVLine
 *       drawRect / fillRect / fillScreen / drawCircle / fillCircle
 *       drawCircleHelper / fillCircleHelper / drawRoundRect / fillRoundRect
 *       drawTriangle / fillTriangle / drawBitmap / drawChar
 *
 *   替换关系：
 *       Adafruit_GFX 的 startWrite()/endWrite()  -> TFT_BeginWrite()/TFT_EndWrite()
 *       Adafruit_GFX 的 SPI_WRITE16()            -> TFT_PushColor()/TFT_FillWindow()
 *       pgm_read_byte(&font[...])                -> tft_font5x7[...]
 **************************************************************************/

#include "tft_gfx.h"
#include "tft_font5x7.h"

#ifndef TFT_USE_PRINTF
#define TFT_USE_PRINTF  1       /* 1 = 提供 TFT_Printf（自带格式化器，不依赖 stdio） */
#endif

#if (TFT_USE_PRINTF)
#include <stdarg.h>             /* 只需要 va_list；<stdio.h> 已经不再需要 */
#endif

/*==========================================================================
 * 文本状态（对应 Adafruit_GFX 的 cursor_x/cursor_y/textcolor/... 成员）
 *========================================================================*/
static int16_t  s_cursor_x     = 0;
static int16_t  s_cursor_y     = 0;
static uint16_t s_textcolor    = 0xFFFF;
static uint16_t s_textbgcolor  = 0xFFFF;
static uint8_t  s_textsize     = 1;
static bool     s_wrap         = true;

/*==========================================================================
 * 小工具
 *========================================================================*/
static int16_t tft_abs(int16_t v)
{
    return (v < 0) ? (int16_t)(-v) : v;
}

static void tft_swap_i16(int16_t *a, int16_t *b)
{
    int16_t t = *a;
    *a = *b;
    *b = t;
}

/*********************************************************************
 * @brief  在最底层写一个像素：要求在 CS 已拉低的事务中调用
 *         （对应 Adafruit_GFX::writePixel + SPI_WRITE16）
 *
 * 【P2 修复】这里原来没有边界检查，负坐标一经 (uint16_t) 强制转换就变成
 * 6 万多，TFT_SetAddrWindow() 会把 GRAM 地址写到面板可视区之外 ——
 * 表现为局部花屏，或后续整屏错乱（GRAM 指针被带跑）。
 * 本工程的屏幕宽高在 tft._width / tft._height 里，随旋转变化，所以按它判。
 * 对比：TFT_FillRect() / TFT_DrawPixel() 一直是有裁剪的，只有这里漏了。
 *********************************************************************/
static void write_pixel_raw(int16_t x, int16_t y, uint16_t color)
{
    if ((x < 0) || (y < 0) ||
        (x >= (int16_t)tft._width) || (y >= (int16_t)tft._height))
    {
        return;
    }

    TFT_SetAddrWindow((uint16_t)x, (uint16_t)y, 1, 1);
    TFT_PushColor(color);
}

/*==========================================================================
 * 颜色
 *========================================================================*/
uint16_t TFT_Color565(uint8_t r, uint8_t g, uint8_t b)
{
    /* 兼容 Arduino 的 Color565 约定：RGB 顺序 */
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*==========================================================================
 * 像素 / 直线 / 矩形
 *========================================================================*/
void TFT_DrawPixel(int16_t x, int16_t y, uint16_t color)
{
    if ((x < 0) || (y < 0) || (x >= (int16_t)tft._width) || (y >= (int16_t)tft._height))
    {
        return;                         /* 越界直接丢弃 */
    }
    TFT_BeginWrite();
    write_pixel_raw(x, y, color);
    TFT_EndWrite();
}

/*********************************************************************
 * @brief  水平线（比 DrawLine 快很多）
 *********************************************************************/
void TFT_DrawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color)
{
    if ((y < 0) || (y >= (int16_t)tft._height) || (w <= 0))
    {
        return;
    }
    if (x < 0)
    {
        w += x;                         /* 左边裁掉 */
        x = 0;
    }
    if ((x + w) > (int16_t)tft._width)
    {
        w = (int16_t)tft._width - x;    /* 右边裁掉 */
    }
    if (w <= 0)
    {
        return;
    }

    TFT_BeginWrite();
    TFT_SetAddrWindow((uint16_t)x, (uint16_t)y, (uint16_t)w, 1);
    TFT_FillWindow(color, (uint32_t)w);
    TFT_EndWrite();
}

/*********************************************************************
 * @brief  垂直线
 *********************************************************************/
void TFT_DrawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color)
{
    if ((x < 0) || (x >= (int16_t)tft._width) || (h <= 0))
    {
        return;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if ((y + h) > (int16_t)tft._height)
    {
        h = (int16_t)tft._height - y;
    }
    if (h <= 0)
    {
        return;
    }

    TFT_BeginWrite();
    TFT_SetAddrWindow((uint16_t)x, (uint16_t)y, 1, (uint16_t)h);
    TFT_FillWindow(color, (uint32_t)h);
    TFT_EndWrite();
}

void TFT_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    int16_t x2;

    if (w <= 0 || h <= 0)
    {
        return;
    }
    if ((x >= (int16_t)tft._width) || (y >= (int16_t)tft._height))
    {
        return;
    }
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if ((x + w) > (int16_t)tft._width)
    {
        w = (int16_t)tft._width - x;
    }
    if ((y + h) > (int16_t)tft._height)
    {
        h = (int16_t)tft._height - y;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }

    x2 = x + w - 1;
    if ((x2 < 0) || (x2 >= (int16_t)tft._width))
    {
        return;
    }

    TFT_BeginWrite();
    TFT_SetAddrWindow((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    TFT_FillWindow(color, (uint32_t)w * (uint32_t)h);
    TFT_EndWrite();
}

/*********************************************************************
 * @brief  整屏填充（比 FillRect(0,0,w,h) 少一层裁剪判断）
 *********************************************************************/
void TFT_FillScreen(uint16_t color)
{
    TFT_BeginWrite();
    TFT_SetAddrWindow(0, 0, tft._width, tft._height);
    TFT_FillWindow(color, (uint32_t)tft._width * (uint32_t)tft._height);
    TFT_EndWrite();
}

void TFT_DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }
    TFT_DrawFastHLine(x, y, w, color);
    TFT_DrawFastHLine(x, y + h - 1, w, color);
    TFT_DrawFastVLine(x, y, h, color);
    TFT_DrawFastVLine(x + w - 1, y, h, color);
}

/*********************************************************************
 * @brief  任意两点画线（对应 Adafruit_GFX::writeLine 的 Bresenham 算法）
 *********************************************************************/
void TFT_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    bool    steep;
    int16_t dx;
    int16_t dy;
    int16_t err;
    int16_t ystep;
    int16_t t;

    /* 水平/垂直线直接用优化版本 */
    if (y0 == y1)
    {
        if (x1 > x0)
        {
            TFT_DrawFastHLine(x0, y0, (int16_t)(x1 - x0 + 1), color);
        }
        else
        {
            TFT_DrawFastHLine(x1, y0, (int16_t)(x0 - x1 + 1), color);
        }
        return;
    }
    if (x0 == x1)
    {
        if (y1 > y0)
        {
            TFT_DrawFastVLine(x0, y0, (int16_t)(y1 - y0 + 1), color);
        }
        else
        {
            TFT_DrawFastVLine(x0, y1, (int16_t)(y0 - y1 + 1), color);
        }
        return;
    }

    steep = tft_abs((int16_t)(y1 - y0)) > tft_abs((int16_t)(x1 - x0));
    if (steep)
    {
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1)
    {
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    dx    = (int16_t)(x1 - x0);
    dy    = tft_abs((int16_t)(y1 - y0));
    err   = (int16_t)(dx / 2);
    ystep = (y0 < y1) ? 1 : -1;

    /* 整条线共用一次 CS 事务，比逐像素开关片选快得多 */
    TFT_BeginWrite();
    for (; x0 <= x1; x0++)
    {
        if (steep)
        {
            write_pixel_raw(y0, x0, color);
        }
        else
        {
            write_pixel_raw(x0, y0, color);
        }
        err -= dy;
        if (err < 0)
        {
            y0 += ystep;
            err = (int16_t)(err + dx);
        }
    }
    TFT_EndWrite();
}

/*==========================================================================
 * 圆 / 圆角矩形
 *========================================================================*/
/*********************************************************************
 * @brief  画四分之一的圆弧（对应 Adafruit_GFX::drawCircleHelper）
 * @param  cornername - 位 0~3 选择四个象限
 *********************************************************************/
static void draw_circle_helper(int16_t x0, int16_t y0, int16_t r,
                               uint8_t cornername, uint16_t color)
{
    int16_t f     = (int16_t)(1 - r);
    int16_t ddF_x = 1;
    int16_t ddF_y = (int16_t)(-2 * r);
    int16_t x     = 0;
    int16_t y     = r;

    while (x < y)
    {
        if (f >= 0)
        {
            y--;
            ddF_y += 2;
            f = (int16_t)(f + ddF_y);
        }
        x++;
        ddF_x += 2;
        f = (int16_t)(f + ddF_x);

        if (cornername & 0x4)
        {
            write_pixel_raw((int16_t)(x0 + x), (int16_t)(y0 + y), color);
            write_pixel_raw((int16_t)(x0 + y), (int16_t)(y0 + x), color);
        }
        if (cornername & 0x2)
        {
            write_pixel_raw((int16_t)(x0 + x), (int16_t)(y0 - y), color);
            write_pixel_raw((int16_t)(x0 + y), (int16_t)(y0 - x), color);
        }
        if (cornername & 0x8)
        {
            write_pixel_raw((int16_t)(x0 - y), (int16_t)(y0 + x), color);
            write_pixel_raw((int16_t)(x0 - x), (int16_t)(y0 + y), color);
        }
        if (cornername & 0x1)
        {
            write_pixel_raw((int16_t)(x0 - y), (int16_t)(y0 - x), color);
            write_pixel_raw((int16_t)(x0 - x), (int16_t)(y0 - y), color);
        }
    }
}

/*********************************************************************
 * @brief  填充部分圆（对应 Adafruit_GFX::fillCircleHelper）
 *********************************************************************/
static void fill_circle_helper(int16_t x0, int16_t y0, int16_t r,
                               uint8_t cornername, int16_t delta, uint16_t color)
{
    int16_t f     = (int16_t)(1 - r);
    int16_t ddF_x = 1;
    int16_t ddF_y = (int16_t)(-2 * r);
    int16_t x     = 0;
    int16_t y     = r;

    while (x < y)
    {
        if (f >= 0)
        {
            y--;
            ddF_y += 2;
            f = (int16_t)(f + ddF_y);
        }
        x++;
        ddF_x += 2;
        f = (int16_t)(f + ddF_x);

        if (cornername & 0x1)
        {
            TFT_DrawFastVLine((int16_t)(x0 + x), (int16_t)(y0 - y),
                              (int16_t)(2 * y + 1 + delta), color);
            TFT_DrawFastVLine((int16_t)(x0 + y), (int16_t)(y0 - x),
                              (int16_t)(2 * x + 1 + delta), color);
        }
        if (cornername & 0x2)
        {
            TFT_DrawFastVLine((int16_t)(x0 - x), (int16_t)(y0 - y),
                              (int16_t)(2 * y + 1 + delta), color);
            TFT_DrawFastVLine((int16_t)(x0 - y), (int16_t)(y0 - x),
                              (int16_t)(2 * x + 1 + delta), color);
        }
    }
}

void TFT_DrawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    int16_t f     = (int16_t)(1 - r);
    int16_t ddF_x = 1;
    int16_t ddF_y = (int16_t)(-2 * r);
    int16_t x     = 0;
    int16_t y     = r;

    if (r < 0)
    {
        return;
    }

    TFT_BeginWrite();
    write_pixel_raw(x0, (int16_t)(y0 + r), color);
    write_pixel_raw(x0, (int16_t)(y0 - r), color);
    write_pixel_raw((int16_t)(x0 + r), y0, color);
    write_pixel_raw((int16_t)(x0 - r), y0, color);

    while (x < y)
    {
        if (f >= 0)
        {
            y--;
            ddF_y += 2;
            f = (int16_t)(f + ddF_y);
        }
        x++;
        ddF_x += 2;
        f = (int16_t)(f + ddF_x);

        write_pixel_raw((int16_t)(x0 + x), (int16_t)(y0 + y), color);
        write_pixel_raw((int16_t)(x0 - x), (int16_t)(y0 + y), color);
        write_pixel_raw((int16_t)(x0 + x), (int16_t)(y0 - y), color);
        write_pixel_raw((int16_t)(x0 - x), (int16_t)(y0 - y), color);
        write_pixel_raw((int16_t)(x0 + y), (int16_t)(y0 + x), color);
        write_pixel_raw((int16_t)(x0 - y), (int16_t)(y0 + x), color);
        write_pixel_raw((int16_t)(x0 + y), (int16_t)(y0 - x), color);
        write_pixel_raw((int16_t)(x0 - y), (int16_t)(y0 - x), color);
    }
    TFT_EndWrite();
}

void TFT_FillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    if (r < 0)
    {
        return;
    }
    TFT_DrawFastVLine(x0, (int16_t)(y0 - r), (int16_t)(2 * r + 1), color);
    fill_circle_helper(x0, y0, r, 3, 0, color);
}

void TFT_DrawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t color)
{
    int16_t max_radius;

    if (w <= 0 || h <= 0)
    {
        return;
    }

    max_radius = ((w < h) ? w : h) / 2;
    if (r > max_radius)
    {
        r = max_radius;
    }

    TFT_DrawFastHLine((int16_t)(x + r), y, (int16_t)(w - 2 * r), color);
    TFT_DrawFastHLine((int16_t)(x + r), (int16_t)(y + h - 1), (int16_t)(w - 2 * r), color);
    TFT_DrawFastVLine(x, (int16_t)(y + r), (int16_t)(h - 2 * r), color);
    TFT_DrawFastVLine((int16_t)(x + w - 1), (int16_t)(y + r), (int16_t)(h - 2 * r), color);

    TFT_BeginWrite();
    draw_circle_helper((int16_t)(x + r), (int16_t)(y + r), r, 1, color);
    draw_circle_helper((int16_t)(x + w - r - 1), (int16_t)(y + r), r, 2, color);
    draw_circle_helper((int16_t)(x + w - r - 1), (int16_t)(y + h - r - 1), r, 4, color);
    draw_circle_helper((int16_t)(x + r), (int16_t)(y + h - r - 1), r, 8, color);
    TFT_EndWrite();
}

void TFT_FillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                       int16_t r, uint16_t color)
{
    int16_t max_radius;

    if (w <= 0 || h <= 0)
    {
        return;
    }

    max_radius = ((w < h) ? w : h) / 2;
    if (r > max_radius)
    {
        r = max_radius;
    }

    if (r < 1)
    {
        /* 没有圆角，直接一个矩形 */
        TFT_FillRect(x, y, w, h, color);
        return;
    }

    /* 中间十字：竖条贯通整个高度、横条贯通整个宽度。
     * 剩下没被盖到的就是四个 r x r 的角，交给下面的圆弧填充。 */
    TFT_FillRect((int16_t)(x + r), y, (int16_t)(w - 2 * r), h, color);
    TFT_FillRect(x, (int16_t)(y + r), (int16_t)(w), (int16_t)(h - 2 * r), color);

    /* 左右两条圆角边。
     *
     * 用 delta = h - 2r - 1 把竖线从"上圆弧"一直拉到"下圆弧"，
     * 一次调用就同时把 左上+左下（或 右上+右下）两个角填好。
     *
     * 原来这里是按四个角分别调 cornername = 1/2/4/8、delta 全传 0，
     * 但 fill_circle_helper() 只实现了 0x1 / 0x2 两个分支，0x4 / 0x8 是空的
     * —— 结果**四个角的填充全丢了**，现象就是"圆角处没有颜色"。
     * （描边用的 draw_circle_helper() 四个分支都在，所以轮廓一直是好的。）
     *
     * 注意 0x2 = 圆心左侧、0x1 = 圆心右侧（和 Adafruit 一致）。 */
    {
        int16_t delta = (int16_t)(h - 2 * r - 1);

        fill_circle_helper((int16_t)(x + r), (int16_t)(y + r),
                           r, 2, delta, color);                     /* 左边 */
        fill_circle_helper((int16_t)(x + w - r - 1), (int16_t)(y + r),
                           r, 1, delta, color);                     /* 右边 */
    }
}

/*==========================================================================
 * 三角形
 *========================================================================*/
void TFT_DrawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color)
{
    TFT_DrawLine(x0, y0, x1, y1, color);
    TFT_DrawLine(x1, y1, x2, y2, color);
    TFT_DrawLine(x2, y2, x0, y0, color);
}

/*********************************************************************
 * @brief  实心三角形（对应 Adafruit_GFX::fillTriangle）
 *********************************************************************/
void TFT_FillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color)
{
    int16_t  a;
    int16_t  b;
    int16_t  y;
    int16_t  last;
    int16_t  dx01;
    int16_t  dy01;
    int16_t  dx02;
    int16_t  dy02;
    int16_t  dx12;
    int16_t  dy12;
    int32_t  sa;
    int32_t  sb;

    /* 先按 Y 排序：y2 >= y1 >= y0 */
    if (y0 > y1)
    {
        tft_swap_i16(&y0, &y1);
        tft_swap_i16(&x0, &x1);
    }
    if (y1 > y2)
    {
        tft_swap_i16(&y2, &y1);
        tft_swap_i16(&x2, &x1);
    }
    if (y0 > y1)
    {
        tft_swap_i16(&y0, &y1);
        tft_swap_i16(&x0, &x1);
    }

    if (y0 == y2)                       /* 三点共线：退化成一条横线 */
    {
        a = b = x0;
        if (x1 < a) { a = x1; }
        else if (x1 > b) { b = x1; }
        if (x2 < a) { a = x2; }
        else if (x2 > b) { b = x2; }
        TFT_DrawFastHLine(a, y0, (int16_t)(b - a + 1), color);
        return;
    }

    dx01 = (int16_t)(x1 - x0);
    dy01 = (int16_t)(y1 - y0);
    dx02 = (int16_t)(x2 - x0);
    dy02 = (int16_t)(y2 - y0);
    dx12 = (int16_t)(x2 - x1);
    dy12 = (int16_t)(y2 - y1);
    sa   = 0;
    sb   = 0;

    if (y1 == y2)
    {
        last = y1;                      /* 平底三角形：含 y1 */
    }
    else
    {
        last = (int16_t)(y1 - 1);       /* 否则 y1 留给下半部分 */
    }

    for (y = y0; y <= last; y++)
    {
        a = (int16_t)(x0 + sa / dy01);
        b = (int16_t)(x0 + sb / dy02);
        sa += dx01;
        sb += dx02;
        if (a > b)
        {
            tft_swap_i16(&a, &b);
        }
        TFT_DrawFastHLine(a, y, (int16_t)(b - a + 1), color);
    }

    sa = (int32_t)dx12 * (y - y1);
    sb = (int32_t)dx02 * (y - y0);

    for (; y <= y2; y++)
    {
        a = (int16_t)(x1 + sa / dy12);
        b = (int16_t)(x0 + sb / dy02);
        sa += dx12;
        sb += dx02;
        if (a > b)
        {
            tft_swap_i16(&a, &b);
        }
        TFT_DrawFastHLine(a, y, (int16_t)(b - a + 1), color);
    }
}

/*==========================================================================
 * 位图
 *========================================================================*/
/*********************************************************************
 * @brief  单色位图（对应 Adafruit_GFX::drawBitmap）
 * @param  bitmap - 每行 (w+7)/8 字节，MSB 在前；位为 1 的地方画 color
 *********************************************************************/
void TFT_DrawBitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                    int16_t w, int16_t h, uint16_t color)
{
    int16_t byteWidth = (int16_t)((w + 7) / 8);
    uint8_t byte      = 0;
    int16_t i;
    int16_t j;

    if (bitmap == 0)
    {
        return;
    }

    TFT_BeginWrite();
    for (j = 0; j < h; j++, y++)
    {
        for (i = 0; i < w; i++)
        {
            if (i & 7)
            {
                byte <<= 1;
            }
            else
            {
                byte = bitmap[j * byteWidth + i / 8];
            }
            if (byte & 0x80)
            {
                if ((y >= 0) && (y < (int16_t)tft._height) &&
                    ((int16_t)(x + i) >= 0) && ((int16_t)(x + i) < (int16_t)tft._width))
                {
                    write_pixel_raw((int16_t)(x + i), y, color);
                }
            }
        }
    }
    TFT_EndWrite();
}

/*********************************************************************
 * @brief  RGB565 位图（每像素 2 字节，大端），对应 pushImage()
 *********************************************************************/
void TFT_DrawRGBBitmap(int16_t x, int16_t y, const uint16_t *pixels,
                       int16_t w, int16_t h)
{
    int16_t row;

    if ((pixels == 0) || (w <= 0) || (h <= 0))
    {
        return;
    }
    if ((x < 0) || (y < 0) || ((x + w) > (int16_t)tft._width) ||
        ((y + h) > (int16_t)tft._height))
    {
        return;                         /* 简单起见：整体越界不画 */
    }

    TFT_BeginWrite();
    for (row = 0; row < h; row++)
    {
        TFT_SetAddrWindow((uint16_t)x, (uint16_t)(y + row), (uint16_t)w, 1);
        TFT_PushColors(&pixels[(uint32_t)row * (uint32_t)w], (uint32_t)w);
    }
    TFT_EndWrite();
}

/*********************************************************************
 * @brief  屏幕反显（对应 Adafruit_ST77xx::invertDisplay）
 *********************************************************************/
void TFT_InvertDisplay(bool invert)
{
    TFT_SendCommand(invert ? tft.invertOnCommand : tft.invertOffCommand, 0, 0);
}

/*==========================================================================
 * 文本
 *========================================================================*/
void TFT_SetCursor(int16_t x, int16_t y)
{
    s_cursor_x = x;
    s_cursor_y = y;
}

int16_t TFT_GetCursorX(void)
{
    return s_cursor_x;
}

int16_t TFT_GetCursorY(void)
{
    return s_cursor_y;
}

void TFT_SetTextColor(uint16_t color)
{
    s_textcolor   = color;
    s_textbgcolor = color;              /* 背景 = 前景 -> 透明 */
}

void TFT_SetTextColorBg(uint16_t color, uint16_t bg)
{
    s_textcolor   = color;
    s_textbgcolor = bg;
}

void TFT_SetTextSize(uint8_t size)
{
    if (size == 0)
    {
        size = 1;
    }
    s_textsize = size;
}

uint8_t TFT_GetTextSize(void)
{
    return s_textsize;
}

void TFT_SetTextWrap(bool wrap)
{
    s_wrap = wrap;
}

/*********************************************************************
 * @brief  在指定位置画一个字符（对应 Adafruit_GFX::drawChar）
 * @param  ch   - ASCII 字符
 * @param  bg   - 背景色；bg == color 时背景透明
 * @param  size - 放大倍数
 *********************************************************************/
void TFT_DrawChar(int16_t x, int16_t y, char ch,
                  uint16_t color, uint16_t bg, uint8_t size)
{
    uint8_t i;
    uint8_t j;
    uint8_t line;

    if (size == 0)
    {
        return;
    }
    if ((x >= (int16_t)tft._width) || (y >= (int16_t)tft._height))
    {
        return;
    }
    if (((x + 6 * (int16_t)size - 1) < 0) || ((y + 8 * (int16_t)size - 1) < 0))
    {
        return;
    }

    /* 优化路径：1 倍字号 + 不透明背景 + 整格都在屏内
     *          -> 6x8 单元格一次写下去，比逐像素快 20 倍以上 */
    if ((size == 1) && (bg != color) && (x >= 0) && (y >= 0) &&
        ((x + 6) <= (int16_t)tft._width) && ((y + 8) <= (int16_t)tft._height))
    {
        uint16_t cell[6 * 8];

        for (i = 0; i < 5; i++)
        {
            line = tft_font5x7[((uint8_t)ch) * 5 + i];
            for (j = 0; j < 8; j++)
            {
                cell[j * 6 + i] = (line & 0x01) ? color : bg;
                line >>= 1;
            }
        }
        for (j = 0; j < 8; j++)
        {
            cell[j * 6 + 5] = bg;       /* 字符右侧 1 列间隔 */
        }

        TFT_BeginWrite();
        TFT_SetAddrWindow((uint16_t)x, (uint16_t)y, 6, 8);
        TFT_PushColors(cell, 6 * 8);
        TFT_EndWrite();
        return;
    }

    /* 优化路径 2：2~4 倍字号 + 不透明背景 + 整格都在屏内
     *          -> 一个字符只要 8 次窗口设置（每个字模行连纵向放大行一次写入），
     *             原来每个字模点一次 TFT_FillRect，一个字符 40~80 次。
     *             跑酷 HUD / 结束页这些 scale=2 的文本都走这里。
     * 【性能】TFT_FillRect 每次都要发一整套地址窗口命令（5 笔 SPI 事务），
     *         小矩形里命令比像素数据还贵，所以逐点绘制是最大的浪费。 */
    if ((size >= 2) && (size <= 4) && (bg != color) && (x >= 0) && (y >= 0) &&
        ((x + 6 * (int16_t)size) <= (int16_t)tft._width) &&
        ((y + 8 * (int16_t)size) <= (int16_t)tft._height))
    {
        uint16_t blk[6 * 4 * 4];        /* 一行放大的像素（最多 24x4 = 192 B）*/
        uint8_t  u;
        uint8_t  v;
        uint16_t rowpx = (uint16_t)(6 * size);

        /* 【2026-10-18 性能优化】整格只开【1 次】地址窗口。
         *
         * 原来是每个字模行 j 各开一次窗口（8 次），而每次 TFT_SetAddrWindow
         * 要发 11 字节命令、约 5 笔独立 SPI 事务 —— 8 行就是 40 笔命令，
         * 数据本身（size=2 时 8 行 × 48 B）才 384 字节。
         * 小字号文本里命令开销远大于数据开销。
         *
         * 现在窗口一次开到整格 (6*size) x (8*size)，之后 8 次 TFT_PushColors
         * 连续往同一个窗口里灌 —— 命令笔数从 40 降到 5，整体事务数少 3/4。
         * 缓冲区仍是 192 字节（和原来一样，栈占用不变）。 */
        TFT_BeginWrite();
        TFT_SetAddrWindow((uint16_t)x, (uint16_t)y,
                          (uint16_t)(6 * size), (uint16_t)(8 * size));

        for (j = 0; j < 8; j++)
        {
            for (v = 0; v < size; v++)          /* 纵向放大：整行重复 size 次 */
            {
                for (i = 0; i < 6; i++)         /* 第 6 列是字符间隔，填背景色 */
                {
                    line = (i < 5) ? tft_font5x7[((uint8_t)ch) * 5 + i] : 0u;
                    line = (uint8_t)((line >> j) & 0x01);
                    for (u = 0; u < size; u++)  /* 横向放大 */
                    {
                        blk[(uint16_t)v * rowpx + (uint16_t)i * size + u] =
                            (line != 0u) ? color : bg;
                    }
                }
            }
            TFT_PushColors(blk, (uint32_t)rowpx * (uint32_t)size);
        }
        TFT_EndWrite();
        return;
    }

    /* 通用路径：与原 Adafruit_GFX 完全一致的逐点绘制（含负坐标裁剪） */
    for (i = 0; i < 5; i++)
    {
        line = tft_font5x7[((uint8_t)ch) * 5 + i];
        for (j = 0; j < 8; j++, line >>= 1)
        {
            if (line & 0x01)
            {
                if (size == 1)
                {
                    TFT_DrawPixel((int16_t)(x + i), (int16_t)(y + j), color);
                }
                else
                {
                    TFT_FillRect((int16_t)(x + i * size), (int16_t)(y + j * size),
                                 size, size, color);
                }
            }
            else if (bg != color)
            {
                if (size == 1)
                {
                    TFT_DrawPixel((int16_t)(x + i), (int16_t)(y + j), bg);
                }
                else
                {
                    TFT_FillRect((int16_t)(x + i * size), (int16_t)(y + j * size),
                                 size, size, bg);
                }
            }
        }
    }
}

/*********************************************************************
 * @brief  按当前光标输出一个字符（对应 Adafruit_GFX::write 的简化实现）
 *********************************************************************/
void TFT_WriteChar(char ch)
{
    if (ch == '\n')
    {
        s_cursor_x = 0;
        s_cursor_y = (int16_t)(s_cursor_y + s_textsize * 8);
        return;
    }
    if (ch == '\r')
    {
        return;
    }

    if (s_wrap && ((s_cursor_x + s_textsize * 6) > (int16_t)tft._width))
    {
        s_cursor_x = 0;
        s_cursor_y = (int16_t)(s_cursor_y + s_textsize * 8);
    }

    TFT_DrawChar(s_cursor_x, s_cursor_y, ch, s_textcolor, s_textbgcolor, s_textsize);

    s_cursor_x = (int16_t)(s_cursor_x + s_textsize * 6);
    if (s_wrap && (s_cursor_x > ((int16_t)tft._width - s_textsize * 6)))
    {
        s_cursor_x = 0;
        s_cursor_y = (int16_t)(s_cursor_y + s_textsize * 8);
    }
}

void TFT_Print(const char *str)
{
    if (str == 0)
    {
        return;
    }
    while (*str)
    {
        TFT_WriteChar(*str++);
    }
}

/*==========================================================================
 * 【Flash / RAM 优化】自带一个极简格式化器，把 newlib 的 stdio 家族整个踢出镜像
 *
 * 原来 TFT_Printf() 走 vsnprintf()；而 WCH 的 CH592SFR.h 在 -DDEBUG 下把
 * PRINT() 宏定义成 printf()，于是 newlib 被链进来一大串：
 *     printf / _vfprintf_r / _svfprintf_r / __sflush_r / __sfputs_r / _write_r ...
 * 代价：
 *     * Flash  约 5 KB
 *     * RAM    __sf 312 B + _impure_data 76 B = 388 B 常驻 .bss
 *     * 栈     TFT_Printf 还在栈上开 96 字节格式化缓冲
 *
 * 本工程实际只用到 %u %lu %d %x %02x 和 %s，没必要为此背一整套 stdio。
 * 这里自己实现一个够用的格式化器（支持 0/- 标志、宽度、l/h/z 长度修饰，
 * 以及 d i u x X o c s % ），同时供两处使用：
 *     TFT_Printf()  -> 输出到屏幕（TFT_WriteChar）
 *     printf()      -> 输出到调试串口（沿用 CH59x_sys.c 的 _write，只在 DEBUG 下提供）
 * 不用 stdio、不在栈上开大缓冲、不用动态内存。
 *========================================================================*/
#if (TFT_USE_PRINTF) || defined(DEBUG)      /* ---- 自带格式化器：开始 ---- */
typedef void (*tft_putc_fn)(char c);

#if (TFT_USE_PRINTF)
static void tft_putc_tft(char c)
{
    TFT_WriteChar(c);
}
#endif

/* 输出一个（无符号）数值，含符号 / 宽度 / 补零。返回实际输出的字符数。
 * 排布规则和标准 printf 一致：
 *     %5d  的 -42 -> "  -42"      先补空格，再出负号
 *     %05d 的 -42 -> "-0042"      先出负号，再补零
 *     %-5d 的 -42 -> "-42  "      左对齐，后面补空格 */
static int tft_emit_num(tft_putc_fn put, uint32_t uv, uint8_t base, uint8_t ucase,
                        uint8_t neg, uint8_t zero, uint8_t left, int width)
{
    char dig[12];
    int  n   = 0;
    int  pad;
    int  i;
    int  cnt = 0;

    if (uv == 0u)
    {
        dig[n++] = '0';
    }
    while (uv != 0u)
    {
        uint8_t d = (uint8_t)(uv % base);

        dig[n++] = (char)((d < 10u) ? (int)('0' + d)
                                    : (int)((ucase ? 'A' : 'a') + (d - 10u)));
        uv /= base;
    }

    pad = width - n - (neg ? 1 : 0);
    if (pad < 0) { pad = 0; }

    if (!left && !zero)
    {
        for (i = 0; i < pad; i++) { put(' '); cnt++; }
    }
    if (neg) { put('-'); cnt++; }
    if (!left && zero)
    {
        for (i = 0; i < pad; i++) { put('0'); cnt++; }
    }
    while (n != 0) { put(dig[--n]); cnt++; }
    if (left)
    {
        for (i = 0; i < pad; i++) { put(' '); cnt++; }
    }

    return cnt;
}

static int tft_vformat(tft_putc_fn put, const char *fmt, va_list ap)
{
    int         count = 0;
    const char *p     = fmt;

    while (*p != '\0')
    {
        char    c = *p++;
        uint8_t left  = 0u;
        uint8_t zero  = 0u;
        uint8_t lng   = 0u;
        int     width = 0;

        if (c != '%')
        {
            put(c);
            count++;
            continue;
        }

        /* ---- 标志（+ / 空格 / # 忽略即可）---- */
        for (;;)
        {
            if (*p == '-')      { left = 1u; p++; }
            else if (*p == '0') { zero = 1u; p++; }
            else if ((*p == '+') || (*p == ' ') || (*p == '#')) { p++; }
            else { break; }
        }
        /* ---- 宽度 ---- */
        while ((*p >= '0') && (*p <= '9'))
        {
            width = width * 10 + (int)(*p - '0');
            p++;
        }
        /* ---- 精度：跳过（本工程没用）---- */
        if (*p == '.')
        {
            p++;
            while ((*p >= '0') && (*p <= '9')) { p++; }
        }
        /* ---- 长度修饰 ---- */
        while ((*p == 'l') || (*p == 'h') || (*p == 'z') || (*p == 't'))
        {
            if (*p != 'h') { lng = 1u; }
            p++;
        }

        c = *p;
        if (c == '\0')
        {
            put('%');
            count++;
            break;                      /* 格式串以 '%' 结尾：原样吐一个 % */
        }
        p++;

        switch (c)
        {
            case '%':
                put('%');
                count++;
                break;

            case 'd':
            case 'i':
            {
                int32_t  sv = lng ? (int32_t)va_arg(ap, long)
                                  : (int32_t)va_arg(ap, int);
                uint32_t uv = (sv < 0) ? (uint32_t)(0u - (uint32_t)sv) : (uint32_t)sv;

                count += tft_emit_num(put, uv, 10u, 0u,
                                      (uint8_t)(sv < 0), zero, left, width);
                break;
            }

            case 'u':
            case 'o':
            case 'x':
            case 'X':
            {
                uint32_t uv = lng ? (uint32_t)va_arg(ap, unsigned long)
                                  : (uint32_t)va_arg(ap, unsigned int);
                uint8_t  base = (c == 'u') ? 10u : ((c == 'o') ? 8u : 16u);

                count += tft_emit_num(put, uv, base, (uint8_t)(c == 'X'),
                                      0u, zero, left, width);
                break;
            }

            case 'c':
                put((char)va_arg(ap, int));
                count++;
                break;

            case 's':
            {
                const char *s = va_arg(ap, const char *);
                int         n = 0;
                int         i;

                if (s == 0) { s = "(null)"; }
                while (s[n] != '\0') { n++; }

                if (!left && (width > n))
                {
                    for (i = 0; i < (width - n); i++) { put(' '); count++; }
                }
                for (i = 0; i < n; i++) { put(s[i]); count++; }
                if (left && (width > n))
                {
                    for (i = 0; i < (width - n); i++) { put(' '); count++; }
                }
                break;
            }

            default:
                put('%');
                put(c);
                count += 2;
                break;
        }
    }

    return count;
}

#if (TFT_USE_PRINTF)
int TFT_Printf(const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (fmt == 0)
    {
        return 0;
    }

    va_start(ap, fmt);
    n = tft_vformat(tft_putc_tft, fmt, ap);
    va_end(ap);

    return n;
}
#endif

/*==========================================================================
 * 把 DEBUG 下的 printf 也换成同一个格式化器
 *
 * WCH 的 CH592SFR.h 在 -DDEBUG 下写的是 `#define PRINT(X...) printf(X)`，
 * 而本工程编译时带了 -DDEBUG。只要还有一处 printf 没被顶掉，newlib 就会把
 * printf / _vfprintf_r / _svfprintf_r / __sflush_r ... 连同 __sf + _impure_data
 * （388 字节 .bss）一起链进来。这里提供一个同名实现顶上，
 * 输出沿用 CH59x_sys.c 里那份 _write()（写到 DEBUG 选定的 UART）。
 *
 * 注意：DEBUG 关掉时 PRINT 展开成空，本函数也不编 —— 不会有未定义引用。
 *======================================================================*/
#ifdef DEBUG
extern int _write(int fd, char *buf, int size);

static void tft_putc_uart(char c)
{
    (void)_write(1, &c, 1);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (fmt == 0)
    {
        return 0;
    }

    va_start(ap, fmt);
    n = tft_vformat(tft_putc_uart, fmt, ap);
    va_end(ap);

    return n;
}

/*--------------------------------------------------------------------------
 * puts / putchar 也必须自己顶上，否则 stdio 还是会整条被链进来
 *
 * 原因：printf 是编译器内建函数，GCC 会自动做两个等价改写：
 *     printf("head file error...
")  ->  puts("head file error...")
 *     printf("
")                    ->  putchar('
')
 * 本工程 MCU.c 与 hiddev.c 里正好各有一处，objdump 出来引用的是
 * puts / putchar（objdump bluetooth/MCU.o 可见 R_RISCV_CALL_PLT puts）。
 * 只要它们还指向 newlib，就会顺着
 *     puts -> __swbuf_r / __swsetup_r -> _fflush_r -> __sglue / __sf
 * 把 __sf(312B) + findfp + wbuf + fflush + makebuf + stdio 一整串拖进来。
 * 这里把这两个也实现掉，那条链就彻底断了。
 *------------------------------------------------------------------------*/
int putchar(int c)
{
    tft_putc_uart((char)c);
    return c;
}

int puts(const char *s)
{
    const char *p = s;

    if (s == 0)
    {
        return -1;
    }
    while (*p != '\0')
    {
        tft_putc_uart(*p++);
    }
    tft_putc_uart('\n');
    return 0;
}
#endif
#endif

/******************* (C) 移植自 Adafruit GFX 库, BSD License *****************/
