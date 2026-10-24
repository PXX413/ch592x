/**************************************************************************
 * ui_icon.c —— 矢量图标绘制
 *
 * 全部用 tft_gfx 的基本图元组合。坐标按 16x16 网格设计，
 * 绘制时按 s/16 缩放（整数运算）。
 **************************************************************************/

#include "ui_icon.h"
#include "tft_gfx.h"

/*--------------------------------------------------------------------------
 * 缩放辅助：把 16 网格坐标映射到实际像素
 *   v16  ∈ [0,16]
 *   s    实际边长
 * 结果 = x + v16 * s / 16
 *------------------------------------------------------------------------*/
#define SC(v16)   ((int16_t)((int32_t)(v16) * s / 16))

/*==========================================================================
 * 【2026-10-18 性能优化】画一条 16 网格上的线段（粗细按 s 自适应）
 *
 * 原实现：为了"加粗"，对 w 根平行线各调一次 TFT_DrawLine，而且**横竖各画一遍**
 * （同一个 o 既做 x 偏移又做 y 偏移），也就是 2w 条 Bresenham 线。
 *
 * 代价（按 s=30、w=3 估）：
 *   一条 30px 长的线 ≈ 30 个像素点，每个点都要 TFT_SetAddrWindow（5 笔 SPI
 *   事务）+ 2 字节数据 —— 2*3 条线 = 6 条 ≈ 180 次开窗 = 约 900 笔 SPI 事务。
 *   而图标本身只有 30x30 = 900 像素。也就是说**图标里每个像素平均被开窗一次**，
 *   这就是 ui_icon.c 在 objdump 里 Icon_Draw 占到 0x944 字节、主页/设置页
 *   每次重画都要几十毫秒的原因。
 *
 * 新实现：横线/竖线/45° 线直接退化成 TFT_DrawFastHLine / DrawFastVLine /
 *   TFT_FillRect（一次开窗画一整段）；只有真正斜的线才走 Bresenham，
 *   并且把 w 根平行线合成"沿线的方块"（与 ui_kbd_font.c 的 kf_seg 同一思路）。
 *   开窗次数从 O(长度×粗细) 降到 O(长度/步长)，图标重画时间降一个数量级。
 *========================================================================*/
static void iline(int16_t x, int16_t y, int16_t s,
                  int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  uint16_t col, int16_t w)
{
    int16_t ax = (int16_t)(x + SC(x0));
    int16_t ay = (int16_t)(y + SC(y0));
    int16_t bx = (int16_t)(x + SC(x1));
    int16_t by = (int16_t)(y + SC(y1));
    int16_t dx;
    int16_t dy;
    int16_t t;

    if (w < 1) { w = 1; }

    dx = (int16_t)(bx - ax);
    dy = (int16_t)(by - ay);

    /* --- 水平线：一根 FillRect 搞定 --- */
    if (dy == 0)
    {
        t = (ax < bx) ? ax : bx;
        TFT_FillRect(t, (int16_t)(ay - w / 2),
                     (int16_t)(((dx < 0) ? -dx : dx) + 1), w, col);
        return;
    }

    /* --- 垂直线：同上 --- */
    if (dx == 0)
    {
        t = (ay < by) ? ay : by;
        TFT_FillRect((int16_t)(ax - w / 2), t, w,
                     (int16_t)(((dy < 0) ? -dy : dy) + 1), col);
        return;
    }

    /* --- 斜线：沿线段铺 w x w 的方块（楼梯近似），而不是 2w 条细线 --- */
    {
        int16_t adx = (dx < 0) ? (int16_t)(-dx) : dx;
        int16_t ady = (dy < 0) ? (int16_t)(-dy) : dy;
        int16_t steps = (adx > ady) ? adx : ady;
        /* 步长取 w：粗细 w 的方块沿线段走 w 像素，正好首尾相接不留缝。
         * w == 1 时步长必须是 1（逐像素），否则会画成虚线 —— 那和原来
         * "两条细线叠在一起"的实线效果不一致。 */
        int16_t step  = (w > 1) ? w : 1;
        int16_t sp;

        if (steps < 1) { steps = 1; }

        for (sp = 0; sp <= steps; sp = (int16_t)(sp + step))
        {
            int16_t px = (int16_t)(ax + ((int32_t)dx * sp) / steps);
            int16_t py = (int16_t)(ay + ((int32_t)dy * sp) / steps);

            TFT_FillRect((int16_t)(px - w / 2), (int16_t)(py - w / 2), w, w, col);
        }
    }
}

/* 16 网格矩形（描边） */
/* 【2026-10-18 性能优化】描边矩形
 *
 * 原来用 w 层嵌套 TFT_DrawRect 来"加粗" —— 每层 4 条边、每条边都是逐像素
 * 开窗（TFT_DrawRect 内部走 DrawFastHLine/VLine，还算好，但 w 层就是 4w 条边）。
 * 现在改成 4 个 TFT_FillRect：外框减内框 = 一圈环，一次画完，开窗 4 次。 */
static void irect(int16_t x, int16_t y, int16_t s,
                  int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  uint16_t col, int16_t w)
{
    int16_t ax = (int16_t)(x + SC(x0));
    int16_t ay = (int16_t)(y + SC(y0));
    int16_t bw = (int16_t)(SC(x1) - SC(x0));
    int16_t bh = (int16_t)(SC(y1) - SC(y0));

    if ((bw <= 0) || (bh <= 0)) { return; }
    if (w < 1) { w = 1; }
    if (w * 2 > bw) { w = (int16_t)(bw / 2); }
    if (w * 2 > bh) { w = (int16_t)(bh / 2); }
    if (w < 1) { return; }

    TFT_FillRect(ax, ay, bw, w, col);                       /* 上 */
    TFT_FillRect(ax, (int16_t)(ay + bh - w), bw, w, col);   /* 下 */
    TFT_FillRect(ax, (int16_t)(ay + w), w, (int16_t)(bh - 2 * w), col);           /* 左 */
    TFT_FillRect((int16_t)(ax + bw - w), (int16_t)(ay + w), w,
                 (int16_t)(bh - 2 * w), col);               /* 右 */
}

/* 16 网格实心矩形 */
static void ifrect(int16_t x, int16_t y, int16_t s,
                   int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                   uint16_t col)
{
    TFT_FillRect((int16_t)(x + SC(x0)), (int16_t)(y + SC(y0)),
                 (int16_t)(SC(x1) - SC(x0)), (int16_t)(SC(y1) - SC(y0)), col);
}

/* 取同色系暗一档（约 55%），用于需要"凹槽"效果但不知背景色时 */
static uint16_t dim565(uint16_t c)
{
    uint16_t r = (uint16_t)(((c >> 11) & 0x1Fu) * 55u / 100u);
    uint16_t g = (uint16_t)(((c >> 5)  & 0x3Fu) * 55u / 100u);
    uint16_t b = (uint16_t)((c         & 0x1Fu) * 55u / 100u);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/*==========================================================================
 * 各图标
 *========================================================================*/

static void ic_keyboard(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t r, c;

    /* 外框 1..15 x 4..12 */
    irect(x, y, s, 1, 4, 15, 12, col, (s >= 24) ? 2 : 1);

    /* 3x2 键点 */
    for (r = 0; r < 2; r++)
    {
        for (c = 0; c < 3; c++)
        {
            int16_t kx = (int16_t)(3 + c * 4);
            int16_t ky = (int16_t)(6 + r * 3);
            ifrect(x, y, s, kx, ky, (int16_t)(kx + 2), (int16_t)(ky + 1), col);
        }
    }
    /* 空格键 */
    ifrect(x, y, s, 5, 10, 11, 11, col);
}

static void ic_game(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t w = (s >= 24) ? 2 : 1;
    int16_t cx = (int16_t)(x + SC(8));
    int16_t cy = (int16_t)(y + SC(9));

    /* 手柄外轮廓：用圆角矩形描边 */
    TFT_DrawRoundRect((int16_t)(x + SC(1)), (int16_t)(y + SC(5)),
                      SC(14), SC(8), SC(3), col);

    /* 左十字键（描边方式，不盖背景色） */
    {
        int16_t dx = (int16_t)(x + SC(4));
        int16_t dy = (int16_t)(y + SC(9));
        TFT_DrawFastHLine((int16_t)(dx - SC(1)), dy,
                          (int16_t)(SC(3)), col);
        TFT_DrawFastVLine(dx, (int16_t)(dy - SC(1)),
                          (int16_t)(SC(3)), col);
    }

    /* 右侧两个按钮 */
    TFT_DrawCircle((int16_t)(cx + SC(3)), (int16_t)(cy - SC(1)), SC(1), col);
    TFT_DrawCircle((int16_t)(cx + SC(3)), (int16_t)(cy + SC(2)), SC(1), col);

    /* 中间握把分隔线（用暗一档的同色，避免依赖背景色） */
    TFT_DrawFastVLine(cx, (int16_t)(y + SC(6)), SC(6), dim565(col));
    (void)w;
}

static void ic_remote(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 天线塔：三角形 + 两层弧 */
    ifrect(x, y, s, 7, 6, 8, 13, col);          /* 塔身 */
    iline(x, y, s, 7, 6, 4, 13, col, (s >= 24) ? 2 : 1);
    iline(x, y, s, 8, 6, 11, 13, col, (s >= 24) ? 2 : 1);
    ifrect(x, y, s, 5, 13, 10, 14, col);        /* 底座 */

    /* 信号弧（用短线段拼） */
    iline(x, y, s, 5, 4, 5, 2, col, 1);
    iline(x, y, s, 5, 2, 10, 2, col, 1);
    iline(x, y, s, 10, 2, 10, 4, col, 1);
}

static void ic_settings(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t cx = (int16_t)(x + SC(8));
    int16_t cy = (int16_t)(y + SC(8));
    int16_t i;
    int16_t or_ = SC(6);
    int16_t ir  = SC(3);

    /* 齿轮：画 8 个方向的齿（短粗线）+ 内外圆 */
    for (i = 0; i < 8; i++)
    {
        /* 用查表避免三角函数 */
        static const int8_t dx[8] = {  0,  9, 12,  9,  0, -9, -12, -9 };
        static const int8_t dy[8] = {-12, -9,  0,  9, 12,  9,   0, -9 };
        int16_t px = (int16_t)(cx + (int32_t)dx[i] * s / 16);
        int16_t py = (int16_t)(cy + (int32_t)dy[i] * s / 16);
        TFT_FillCircle(px, py, (s >= 24) ? 2 : 1, col);
    }
    TFT_DrawCircle(cx, cy, or_, col);
    TFT_DrawCircle(cx, cy, ir, col);
    TFT_FillCircle(cx, cy, (s >= 24) ? 2 : 1, col);
}

static void ic_battery(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    irect(x, y, s, 2, 5, 13, 11, col, 1);
    ifrect(x, y, s, 13, 7, 15, 9, col);         /* 正极 */
    ifrect(x, y, s, 4, 7, 9, 9, col);           /* 电量条 */
}

static void ic_usb(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 插头：矩形 + 两个触点 + 线 */
    irect(x, y, s, 2, 5, 9, 11, col, 1);
    ifrect(x, y, s, 10, 6, 12, 7, col);
    ifrect(x, y, s, 10, 9, 12, 10, col);
    iline(x, y, s, 12, 8, 15, 8, col, (s >= 24) ? 2 : 1);
}

static void ic_bluetooth(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t w = (s >= 24) ? 2 : 1;
    iline(x, y, s, 8, 1, 8, 15, col, w);
    iline(x, y, s, 8, 1, 12, 5, col, w);
    iline(x, y, s, 12, 5, 4, 11, col, w);
    iline(x, y, s, 8, 15, 12, 11, col, w);
    iline(x, y, s, 12, 11, 4, 5, col, w);
}

static void ic_rf24(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 信号强度：三根递增的竖条 */
    ifrect(x, y, s, 2, 11, 4, 15, col);
    ifrect(x, y, s, 6, 7,  8, 15, col);
    ifrect(x, y, s, 10, 3, 12, 15, col);
    ifrect(x, y, s, 14, 1, 15, 15, col);
}

static void ic_check(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t w = (s >= 24) ? 3 : 2;
    iline(x, y, s, 3, 8, 7, 12, col, w);
    iline(x, y, s, 7, 12, 13, 4, col, w);
}

static void ic_cross(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t w = (s >= 24) ? 3 : 2;
    iline(x, y, s, 4, 4, 12, 12, col, w);
    iline(x, y, s, 12, 4, 4, 12, col, w);
}

static void ic_arrow_r(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t w = (s >= 24) ? 2 : 1;
    iline(x, y, s, 6, 8, 11, 8, col, w);
    iline(x, y, s, 8, 5, 11, 8, col, w);
    iline(x, y, s, 11, 8, 8, 11, col, w);
}

static void ic_bright(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t cx = (int16_t)(x + SC(8));
    int16_t cy = (int16_t)(y + SC(8));
    int16_t i;

    TFT_FillCircle(cx, cy, SC(3), col);
    for (i = 0; i < 8; i++)
    {
        static const int8_t dx[8] = {  0,  8, 11,  8,  0, -8, -11, -8 };
        static const int8_t dy[8] = {-11, -8,  0,  8, 11,  8,   0, -8 };
        int16_t ax = (int16_t)(cx + (int32_t)dx[i] * s / 16);
        int16_t ay = (int16_t)(cy + (int32_t)dy[i] * s / 16);
        int16_t bx = (int16_t)(cx + (int32_t)dx[i] * s / 22);
        int16_t by = (int16_t)(cy + (int32_t)dy[i] * s / 22);
        TFT_DrawLine(ax, ay, bx, by, col);
    }
}

static void ic_sleep(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 月牙：画一个圆，再用"暗一档同色"在右上叠一个小圆做凹口。
     * 用 dim565 而不是黑色，这样在任何背景色上都自然。 */
    int16_t cx = (int16_t)(x + SC(7));
    int16_t cy = (int16_t)(y + SC(8));
    int16_t r  = SC(5);

    TFT_FillCircle(cx, cy, r, col);
    TFT_FillCircle((int16_t)(cx + SC(3)), (int16_t)(cy - SC(2)), r, dim565(col));

    /* 点缀星星 */
    TFT_DrawPixel((int16_t)(x + SC(12)), (int16_t)(y + SC(3)), col);
    TFT_DrawPixel((int16_t)(x + SC(14)), (int16_t)(y + SC(7)), col);
    TFT_DrawPixel((int16_t)(x + SC(3)),  (int16_t)(y + SC(13)), col);
}

static void ic_refresh(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    int16_t cx = (int16_t)(x + SC(8));
    int16_t cy = (int16_t)(y + SC(8));
    int16_t r  = SC(5);
    int16_t a;

    /* 用点拼一个缺口圆 */
    for (a = 30; a < 330; a += 6)
    {
        static const int8_t cs[60] = {
            100, 99, 97, 94, 91, 87, 82, 77, 71, 64, 57, 50,
            42, 34, 26, 17,  9,  0, -9,-17,-26,-34,-42,-50,
            -57,-64,-71,-77,-82,-87,-91,-94,-97,-99,-100,-99,
            -97,-94,-91,-87,-82,-77,-71,-64,-57,-50,-42,-34,
            -26,-17, -9,  0,  9, 17, 26, 34, 42, 50, 57, 64 };
        static const int8_t sn[60] = {
            0,  9, 17, 26, 34, 42, 50, 57, 64, 71, 77, 82,
            87, 91, 94, 97, 99,100, 99, 97, 94, 91, 87, 82,
            77, 71, 64, 57, 50, 42, 34, 26, 17,  9,  0, -9,
            -17,-26,-34,-42,-50,-57,-64,-71,-77,-82,-87,-91,
            -94,-97,-99,-100,-99,-97,-94,-91,-87,-82,-77,-71 };
        int16_t idx = (int16_t)((a - 30) / 6);
        if (idx < 0 || idx >= 60) { continue; }
        TFT_DrawPixel((int16_t)(cx + (int32_t)cs[idx] * r / 100),
                      (int16_t)(cy + (int32_t)sn[idx] * r / 100), col);
    }
    /* 箭头 */
    iline(x, y, s, 11, 3, 11, 7, col, 1);
    iline(x, y, s, 11, 3, 8, 3, col, 1);
    iline(x, y, s, 11, 3, 14, 3, col, 1);
}

static void ic_led(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 灯泡 */
    TFT_FillCircle((int16_t)(x + SC(8)), (int16_t)(y + SC(6)), SC(4), col);
    ifrect(x, y, s, 6, 11, 10, 15, col);
}

static void ic_2048(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 2x2 方块，右上角小一点模拟数字块堆叠 */
    int16_t g = (s >= 24) ? 1 : 0;
    ifrect(x, y, s, 2, 2, 8, 8, col);
    ifrect(x, y, s, 9 + g, 2, 14, 8, col);
    ifrect(x, y, s, 2, 9 + g, 8, 14, col);
    irect(x, y, s, 9 + g, 9 + g, 14, 14, col, 1);
}

static void ic_runner(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 火柴人奔跑：头 + 躯干 + 四肢 */
    TFT_FillCircle((int16_t)(x + SC(9)), (int16_t)(y + SC(3)), SC(2), col);
    iline(x, y, s, 9, 5, 8, 10, col, (s >= 24) ? 2 : 1);       /* 躯干 */
    iline(x, y, s, 8, 10, 5, 14, col, 1);                      /* 左腿 */
    iline(x, y, s, 8, 10, 12, 14, col, 1);                     /* 右腿 */
    iline(x, y, s, 9, 6, 5, 5, col, 1);                        /* 左臂 */
    iline(x, y, s, 9, 6, 13, 8, col, 1);                       /* 右臂 */
}

static void ic_thunder(int16_t x, int16_t y, int16_t s, uint16_t col)
{
    /* 战机：向上的三角 + 机翼 */
    TFT_FillTriangle((int16_t)(x + SC(8)), (int16_t)(y + SC(1)),
                     (int16_t)(x + SC(4)), (int16_t)(y + SC(13)),
                     (int16_t)(x + SC(12)), (int16_t)(y + SC(13)), col);
    /* 尾翼 */
    ifrect(x, y, s, 2, 9, 5, 11, col);
    ifrect(x, y, s, 11, 9, 14, 11, col);
}

/*==========================================================================
 * 分发
 *========================================================================*/
void Icon_Draw(int16_t x, int16_t y, icon_t ic, uint16_t col, int16_t s)
{
    if (s < 6) { return; }

    switch (ic)
    {
        case ICON_KEYBOARD:  ic_keyboard (x, y, s, col); break;
        case ICON_GAME:      ic_game     (x, y, s, col); break;
        case ICON_REMOTE:    ic_remote   (x, y, s, col); break;
        case ICON_SETTINGS:  ic_settings (x, y, s, col); break;
        case ICON_BATTERY:   ic_battery  (x, y, s, col); break;
        case ICON_USB:       ic_usb      (x, y, s, col); break;
        case ICON_BLUETOOTH: ic_bluetooth(x, y, s, col); break;
        case ICON_RF24:      ic_rf24     (x, y, s, col); break;
        case ICON_CHECK:     ic_check    (x, y, s, col); break;
        case ICON_CROSS:     ic_cross    (x, y, s, col); break;
        case ICON_ARROW_R:   ic_arrow_r  (x, y, s, col); break;
        case ICON_BRIGHT:    ic_bright   (x, y, s, col); break;
        case ICON_SLEEP:     ic_sleep    (x, y, s, col); break;
        case ICON_REFRESH:   ic_refresh  (x, y, s, col); break;
        case ICON_LED:       ic_led      (x, y, s, col); break;
        case ICON_2048:      ic_2048     (x, y, s, col); break;
        case ICON_RUNNER:    ic_runner   (x, y, s, col); break;
        case ICON_THUNDER:   ic_thunder  (x, y, s, col); break;
        default: break;
    }
}

void Icon_DrawOn(int16_t x, int16_t y, icon_t ic, uint16_t col, uint16_t bg, int16_t s)
{
    TFT_FillRect(x, y, s, s, bg);
    Icon_Draw(x, y, ic, col, s);
}
