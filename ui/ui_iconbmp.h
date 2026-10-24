/**************************************************************************
 * ui_iconbmp.h —— 60x60 RGB565 位图图标贴图（**原始尺寸，不缩放**）
 *
 * 直接按 60x60 原样贴，不做任何缩放：像素一一对应，最清晰也最快。
 *
 * 硬件 SPI：底层 TFT_PushColors() -> TPORT_WriteBytes() -> SPI0_MasterTrans()，
 *   也就是 CH592 的**硬件 SPI0**（tft_port.h 里 TFT_SPI_BACKEND = TFT_SPI_HARD，
 *   时钟 = 60MHz / TFT_HW_SPI_DIV，当前 DIV=3 即 20MHz（见 tft_port.h 说明）。
 *   本函数整块只开一次地址窗口，之后连续把像素推出去，
 *   尽量让 SPI 一次发满，而不是一个像素一次事务。
 *
 * 裁剪：左右相邻的图标有一半在屏外（x 为负），必须自己裁 ——
 *   TFT_SetAddrWindow 不会裁剪，传负坐标会把数据写进 GRAM 的乱地址。
 **************************************************************************/

#ifndef __UI_ICONBMP_H__
#define __UI_ICONBMP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 源图标固定 60x60 */
#define ICONBMP_W   60
#define ICONBMP_H   60

/*********************************************************************
 * @fn      IconBmp_Draw
 * @brief   把 60x60 的 RGB565 图标按原始尺寸贴到 (x, y)
 * @param   src  指向 60*60 个 uint16_t 的图标数据
 * @param   x, y 目标左上角（允许为负/超屏，内部裁剪）
 * @note    不缩放，像素一一对应
 *********************************************************************/
void IconBmp_Draw(const uint16_t *src, int16_t x, int16_t y);

#ifdef __cplusplus
}
#endif

#endif /* __UI_ICONBMP_H__ */
