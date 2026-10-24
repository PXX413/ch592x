/**************************************************************************
 * ui_iconbmp.c —— 60x60 RGB565 位图图标贴图（原始尺寸，走硬件 SPI）
 *
 * 关键点：
 *   1) **不缩放**：源 60x60 直接 1:1 贴，没有最近邻取样的开销
 *   2) **整块只开一次地址窗口**：原实现是每行开一次窗口（一张图 60 次
 *      CASET/RASET/RAMWR），现在一次开完，之后全是连续的数据突发
 *   3) **硬件 SPI**：像素经 TFT_PushColors -> TPORT_WriteBytes ->
 *      SPI0_MasterTrans 发出去（TFT_SPI_BACKEND = TFT_SPI_HARD）
 *   4) **自己裁剪**：SetAddrWindow 不裁剪，左右相邻图标一半在屏外，
 *      负坐标会把数据写进 GRAM 乱地址
 **************************************************************************/

#include "ui_iconbmp.h"
#include "tft_st7789.h"     /* TFT_t tft / TFT_SetAddrWindow / TFT_PushColors */
#include "tft_gfx.h"

void IconBmp_Draw(const uint16_t *src, int16_t x, int16_t y)
{
    int16_t scr_w;
    int16_t scr_h;
    int16_t dx0;
    int16_t dx1;
    int16_t dy0;
    int16_t dy1;
    int16_t dy;

    if (src == 0)
    {
        return;
    }

    scr_w = (int16_t)tft._width;
    scr_h = (int16_t)tft._height;

    if ((x >= scr_w) || (y >= scr_h) ||
        ((x + ICONBMP_W) <= 0) || ((y + ICONBMP_H) <= 0))
    {
        return;                         /* 完全在屏外 */
    }

    dx0 = 0;
    dx1 = ICONBMP_W;
    dy0 = 0;
    dy1 = ICONBMP_H;

    if (x < 0)                        { dx0 = (int16_t)(-x); }
    if ((x + ICONBMP_W) > scr_w)      { dx1 = (int16_t)(scr_w - x); }
    if (y < 0)                        { dy0 = (int16_t)(-y); }
    if ((y + ICONBMP_H) > scr_h)      { dy1 = (int16_t)(scr_h - y); }

    if ((dx1 <= dx0) || (dy1 <= dy0))
    {
        return;
    }

    /* ---- 整块只开一次窗口，之后全是连续数据 ---- */
    TFT_BeginWrite();
    TFT_SetAddrWindow((uint16_t)(x + dx0), (uint16_t)(y + dy0),
                      (uint16_t)(dx1 - dx0), (uint16_t)(dy1 - dy0));

    if ((dx0 == 0) && (dx1 == (int16_t)ICONBMP_W) &&
        (dy0 == 0) && (dy1 == (int16_t)ICONBMP_H))
    {
        /* 完整可见（最常见）：一次把 3600 个像素全推出去 */
        TFT_PushColors(src, (uint32_t)(ICONBMP_W * ICONBMP_H));
    }
    else
    {
        /* 被裁掉一部分：窗口已按可见矩形开好，逐行只推可见的那一段。
         * 源数据每行本来是连续的，所以每行也是一次连续突发。 */
        for (dy = dy0; dy < dy1; dy++)
        {
            TFT_PushColors(src + ((int32_t)dy * ICONBMP_W) + dx0,
                           (uint32_t)(dx1 - dx0));
        }
    }

    TFT_EndWrite();
}
