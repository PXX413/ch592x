/**************************************************************************
 * ui_theme.c —— 主题工具实现
 **************************************************************************/

#include "ui_theme.h"

/* 取 RGB565 的分量并重组成 0..31 / 0..63 / 0..31 */
#define R5(c)   ((uint8_t)(((c) >> 11) & 0x1Fu))
#define G6(c)   ((uint8_t)(((c) >> 5)  & 0x3Fu))
#define B5(c)   ((uint8_t)((c)        & 0x1Fu))

#define MK(r, g, b)  ((uint16_t)((((uint16_t)(r) & 0x1Fu) << 11) | \
                                 (((uint16_t)(g) & 0x3Fu) << 5)  | \
                                  ((uint16_t)(b) & 0x1Fu)))

/* 定点乘：v * t / 256 */
static int16_t s_mul256(int16_t v, int16_t t)
{
    int32_t r = (int32_t)v * (int32_t)t;
    if (r >= 0) { r += 128; } else { r -= 128; }
    return (int16_t)(r / 256);
}

uint16_t Theme_Blend(uint16_t a, uint16_t b, int16_t t256)
{
    int16_t  ra, ga, ba, rb, gb, bb;

    if (t256 <= 0)   { return a; }
    if (t256 >= 256) { return b; }

    ra = (int16_t)R5(a);
    ga = (int16_t)G6(a);
    ba = (int16_t)B5(a);
    rb = (int16_t)R5(b);
    gb = (int16_t)G6(b);
    bb = (int16_t)B5(b);

    ra = (int16_t)(ra + s_mul256((int16_t)(rb - ra), t256));
    ga = (int16_t)(ga + s_mul256((int16_t)(gb - ga), t256));
    ba = (int16_t)(ba + s_mul256((int16_t)(bb - ba), t256));

    return MK((uint8_t)ra, (uint8_t)ga, (uint8_t)ba);
}

uint16_t Theme_Dim(uint16_t c, uint8_t pct)
{
    if (pct >= 100u) { return c; }
    return MK((uint8_t)((R5(c) * pct) / 100u),
              (uint8_t)((G6(c) * pct) / 100u),
              (uint8_t)((B5(c) * pct) / 100u));
}

uint16_t Theme_Light(uint16_t c, uint8_t pct)
{
    if (pct == 0u) { return c; }
    if (pct > 100u) { pct = 100u; }

    return MK((uint8_t)(R5(c) + ((31u - R5(c)) * pct) / 100u),
              (uint8_t)(G6(c) + ((63u - G6(c)) * pct) / 100u),
              (uint8_t)(B5(c) + ((31u - B5(c)) * pct) / 100u));
}
