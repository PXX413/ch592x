/**************************************************************************
 * ui_kbd_font.c —— 键盘页左侧大字号「笔画字体」渲染器
 *
 * 数据结构与坐标系说明见 ui_kbd_font_data.h。
 * 绘制策略：
 *   * 横/竖笔画直接用 TFT_FillRect（一次开窗刷一行/一列，最快）
 *   * 斜笔画才退回逐像素 Bresenham，并按主轴方向偏移画 t 根做加粗
 *   键盘页左侧最长的键名是 5 个字母（Enter / Space / Right / LShft…），
 *   按 cap=50 算大约 170px 宽，正好放得进 189px 的区域。
 **************************************************************************/

#include "ui_kbd_font.h"
#include "tft_gfx.h"

#include "ui_kbd_font_data.h"

/* 坐标系里大写高度 = 100 个单位 */
#define KBDF_CAP_UNITS   100
/* 含下延的总高度 = 128 个单位 */
#define KBDF_BOX_UNITS   128

/*==========================================================================
 * 内部
 *========================================================================*/

/* ASCII -> 字形下标 */
static uint8_t kf_lookup(char ch)
{
    uint8_t u = (uint8_t)ch;

    if ((u < KBDF_FIRST) || (u > KBDF_LAST))
    {
        return KBDF_NONE;
    }
    return s_lut[u - KBDF_FIRST];
}

/* 笔画粗细：cap 越大越粗，限定在 2..6 */
static int16_t kf_thick(int16_t cap)
{
    int16_t t = (int16_t)((cap + 12) / 14);

    if (t < 2) { t = 2; }
    if (t > 6) { t = 6; }
    return t;
}

/* 单位 -> 像素（四舍五入） */
static int16_t kf_px(int16_t u, int16_t cap)
{
    return (int16_t)(((int32_t)u * (int32_t)cap + (KBDF_CAP_UNITS / 2)) /
                     (int32_t)KBDF_CAP_UNITS);
}

/* 一条线段（带粗细） */
static void kf_seg(int16_t ax, int16_t ay, int16_t bx, int16_t by,
                   int16_t t, uint16_t col)
{
    int16_t dx = (int16_t)(bx - ax);
    int16_t dy = (int16_t)(by - ay);
    int16_t ox;

    if (dx < 0) { dx = (int16_t)(-dx); }
    if (dy < 0) { dy = (int16_t)(-dy); }

    /* 粗细在两侧均分：水平/垂直/斜线三个分支的矩形都按 ox 往左上偏。
     * 原来还多算了一个 oy = t-1-ox，但三个分支谁都没用它（只有水平线
     * 理论上该用 oy 做纵向居中），属于历史残留，删掉消 -Wunused 告警。 */
    ox = (int16_t)(t / 2);

    if (dy <= 1)
    {
        /* 水平：宽度按 t 在两边各补一点，高度 = t */
        int16_t x0 = (ax < bx) ? ax : bx;
        TFT_FillRect((int16_t)(x0 - ox), (int16_t)(ay - ox),
                     (int16_t)(dx + t), t, col);
    }
    else if (dx <= 1)
    {
        /* 垂直：高度按 t 在两边各补一点，宽度 = t */
        int16_t y0 = (ay < by) ? ay : by;
        TFT_FillRect((int16_t)(ax - ox), (int16_t)(y0 - ox),
                     t, (int16_t)(dy + t), col);
    }
    else
    {
        /* 斜线：沿线段铺 t×t 的小方块（楼梯近似）
         * 【性能修复】原实现是"画 t 根逐像素线"，每个像素都要重设一次
         * 地址窗口（11 字节命令 + 2 字节数据，约 6 次 SPI 事务）。
         * 一条斜笔画就要上千次 SPI 事务；字模里 47% 的笔画是斜的，
         * 于是整页重画要 160ms —— 每按一个键主循环就卡这么久。
         * 改成楼梯方块后，一次 TFT_FillRect 顶一大块，快几十倍。 */
        int16_t steps = (dx > dy) ? dx : dy;
        int16_t sp;

        if (steps < 1) { steps = 1; }

        for (sp = 0; sp <= steps; sp = (int16_t)(sp + t))
        {
            int16_t px = (int16_t)(ax + ((int32_t)(bx - ax) * sp) / steps);
            int16_t py = (int16_t)(ay + ((int32_t)(by - ay) * sp) / steps);

            TFT_FillRect((int16_t)(px - ox), (int16_t)(py - ox), t, t, col);
        }
    }
}

/* 画一个字形（x 是笔画盒左边界） */
static void kf_glyph(int16_t x, int16_t y, uint8_t gi,
                     int16_t cap, int16_t t, uint16_t col)
{
    uint16_t p = s_glyphs[gi].off;

    for (;;)
    {
        uint8_t n = s_strokes[p++];
        uint8_t k;
        int16_t px0 = 0;
        int16_t py0 = 0;

        if (n == 0u)
        {
            break;                      /* 该字形结束 */
        }

        for (k = 0u; k < n; k++)
        {
            int16_t px = (int16_t)(x + kf_px((int16_t)s_strokes[p], cap));
            int16_t py = (int16_t)(y + kf_px((int16_t)s_strokes[p + 1u], cap));
            p += 2u;

            if (k > 0u)
            {
                kf_seg(px0, py0, px, py, t, col);
            }
            px0 = px;
            py0 = py;
        }
    }
}

/*==========================================================================
 * 对外
 *========================================================================*/
int16_t KbdFont_InkMargin(int16_t cap)
{
    /* 笔画粗细是 2..6，两侧最大溢出 = max(t/2, t - t/2) = (t+1)/2 */
    return (int16_t)((kf_thick(cap) + 1) / 2);
}

int16_t KbdFont_BoxH(int16_t cap)
{
    if (cap < 1) { cap = 1; }
    return (int16_t)(((int32_t)cap * KBDF_BOX_UNITS + (KBDF_CAP_UNITS / 2)) /
                     KBDF_CAP_UNITS);
}

int16_t KbdFont_Width(const char *s, int16_t cap)
{
    int32_t units = 0;

    if (s == 0) { return 0; }

    while (*s != '\0')
    {
        uint8_t gi = kf_lookup(*s++);

        if (gi != KBDF_NONE)
        {
            units += (int32_t)s_glyphs[gi].adv;
        }
    }

    return (int16_t)((units * (int32_t)cap + (KBDF_CAP_UNITS / 2)) /
                     KBDF_CAP_UNITS);
}

void KbdFont_Draw(int16_t x, int16_t y, const char *s, int16_t cap,
                  uint16_t fg, uint16_t bg)
{
    int16_t pen = x;
    int16_t t;

    if ((s == 0) || (*s == '\0') || (cap < 5))
    {
        return;
    }

    /* 先把整行框刷成底色：换字时不会残留旧的笔画 */
    TFT_FillRect(x, y, KbdFont_Width(s, cap), KbdFont_BoxH(cap), bg);

    t = kf_thick(cap);

    while (*s != '\0')
    {
        uint8_t gi = kf_lookup(*s++);

        if (gi == KBDF_NONE)
        {
            gi = kf_lookup('?');            /* 没收录的画问号 */
            if (gi == KBDF_NONE) { continue; }
        }

        kf_glyph(pen, y, gi, cap, t, fg);
        pen = (int16_t)(pen + kf_px((int16_t)s_glyphs[gi].adv, cap));
    }
}
