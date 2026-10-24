/**************************************************************************
 * ui_widget.c —— 通用小控件实现
 **************************************************************************/

#include "ui_widget.h"
#include "ui_theme.h"
#include "ui_list.h"
#include "tft_gfx.h"
#include "battery.h"

/*==========================================================================
 * 电量条
 *========================================================================*/
/* 需求 (一).2：顶部固定 20px 显示当前电量，开机读一次、运行中定时刷新。
 * 刷新周期见 UIW_BAT_REFRESH_MS —— 需求原文写的是 5 分钟，
 * 按用户要求改成 **1 分钟**。
 *
 * 【修复 §8-7】原来是"只在开机采一次"，运行中电量永远不动。
 * 现在 UiW_BatTask() 定时起一次采样，走的是 battery.c 的**非阻塞**分段采样
 * （先使能、等稳定、再读），不会像 BAT_ReadMv() 那样用 DelayMs(500) 把
 * 主循环卡住半秒 —— 每分钟卡半秒是能看出来的。
 *
 * 【调用方注意】刷新完成后要用 UI_DirtyFull()，别用 UI_Dirty()：
 * 电量条画在 carousel 的状态栏里，只有"整页重画"才会把它重绘一遍。 */
#define UIW_BAT_REFRESH_MS   60000u     /* 1 分钟（需求原文 5 分钟，按用户要求改） */

static uint8_t  s_bat_pct;
static uint16_t s_bat_mv;
static uint32_t s_bat_next_ms;          /* 下一次该采的时刻 */
static uint8_t  s_bat_running;          /* 1 = 已经起了采样，在等稳定 */

void UiW_BatInit(void)
{
    /* 【开机必须同步读一次】不能只把"下一次该采"设成现在 ——
     * 那样开机第一帧屏上会写一个 0%，而电量条只在整页重画时才被画，
     * 看上去就是"开机电量显示 0"（踩过这个坑）。
     * 这里仍然同步读（代价是开机多等 BAT_EN_SETTLE_MS，和以前一样）；
     * 运行中每 5 分钟的刷新走下面的非阻塞路径，不卡主循环。 */
    (void)BAT_ReadMv();

    s_bat_mv      = BAT_GetLastMv();
    s_bat_pct     = BAT_GetLastPercent();
    s_bat_running = 0u;
    s_bat_next_ms = Game_Millis() + (uint32_t)UIW_BAT_REFRESH_MS;
}

uint8_t UiW_BatTask(void)
{
    uint32_t now = Game_Millis();

    if (s_bat_running == 0u)
    {
        if ((int32_t)(now - s_bat_next_ms) < 0)
        {
            return 0u;                          /* 还没到点 */
        }

        BAT_SampleStart();
        s_bat_running = 1u;
        return 0u;                              /* 下一圈开始等稳定 */
    }

    if (!BAT_SamplePoll(now))
    {
        return 0u;                              /* 还在等稳定 */
    }

    s_bat_mv      = BAT_GetLastMv();
    s_bat_pct     = BAT_GetLastPercent();
    s_bat_running = 0u;
    s_bat_next_ms = now + (uint32_t)UIW_BAT_REFRESH_MS;

    return 1u;                                  /* 有新读数 */
}

uint8_t UiW_BatPercent(void) { return s_bat_pct; }
/* 注意：s_bat_mv 是 ADC 引脚电压（见 battery.h 的量程说明），
 * 电池实际电压 = 这个值 ×2。 */
uint8_t UiW_BatMv100(void)   { return (uint8_t)(s_bat_mv / 100u); }

/*==========================================================================
 * 【临时】电压标定显示开关
 *
 * 1 = 电量百分比的位置改画 **ADC 原始值**（0~4095，4 位数），用来标定
 *     BAT_ADC_VREF_MV / BAT_CAL_*。正常固件请保持 0。
 *
 * 怎么用：电池用万用表量出真实电压（比如 4.04V），同时记下屏上这个数，
 * 则 真实参考电压 = (电池mV × 分压比) × 4096 / 原始值。
 * 例：4040mV × 0.5 = 2020mV，若屏上是 3450 -> 参考 = 2020 × 4096 / 3450
 *     = 2398mV，把 BAT_ADC_VREF_MV 改成 2398 即可（现在填的是 2500）。
 *========================================================================*/
#ifndef UIW_BAT_SHOW_RAW
#define UIW_BAT_SHOW_RAW    0       /* 标定完成：正常显示电量百分比（设 1 可再看原始值） */
#endif

void UiW_DrawBatBar(int16_t x_left, int16_t y, int16_t h, uint16_t bg, uint16_t fg)
{
    uint16_t col;
    int16_t  ix = 170;                  /* 电池图标左边界（靠右放，左边留给标题） */
    int16_t  iy = (int16_t)(y + 5);

    /* 只清"电池 + 百分比"这一小块。
     * 原来是从 x=0 整条清成黑色 —— 调用方刚画好的标题会被擦掉。 */
    if (x_left < 0)
    {
        x_left = 0;
    }
    if ((x_left < (int16_t)UI_SCREEN_W) && (h > 0))
    {
        TFT_FillRect(x_left, y, (int16_t)((int16_t)UI_SCREEN_W - x_left), h, bg);
    }

    /* 电池轮廓 + 正极（颜色由调用方给，白底时传深色） */
    TFT_DrawRect(ix, iy, 24, 10, fg);
    TFT_FillRect((int16_t)(ix + 24), (int16_t)(iy + 3), 2, 4, fg);

    /* 电量填充：>=50% 绿，>=20% 黄，其余红 */
    /* 用主题色（原来是 ST77XX_*，白底下黄色档看不清） */
    if (s_bat_pct >= 50u)      { col = C_GREEN; }
    else if (s_bat_pct >= 20u) { col = C_AMBER; }
    else                       { col = C_RED; }

    TFT_FillRect((int16_t)(ix + 2), (int16_t)(iy + 2),
                 (int16_t)((uint16_t)20u * s_bat_pct / 100u), 6, col);

#if (UIW_BAT_SHOW_RAW)
    /* 标定模式：画 ADC 原始值（4 位 = 24px，右对齐不会碰到电池图标）。
     * 下面那条彩色填充条反映的仍是**未标定**的百分比，别拿它当依据。 */
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 6), (int16_t)(y + 6),
                         (uint32_t)BAT_GetLastRaw(), 1u, fg, bg);
#else
    /* 百分比右对齐（最多 3 位=18px，不会碰到电池图标） */
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 6), (int16_t)(y + 6),
                         (uint32_t)s_bat_pct, 1u, fg, bg);
#endif
}

/*==========================================================================
 * 分区线
 *========================================================================*/
void UiW_HLine(int16_t x, int16_t y, int16_t w, uint16_t col)
{
    TFT_FillRect(x, y, w, 1, col);
}

void UiW_VLine(int16_t x, int16_t y, int16_t h, uint16_t col)
{
    TFT_FillRect(x, y, 1, h, col);
}

/*==========================================================================
 * 3x3 井字格
 *========================================================================*/
int16_t UiW_GridCellW(int16_t size)
{
    /* 3 格 + 2 条 1px 分隔线正好塞满 size */
    return (int16_t)((size - 2) / 3);
}

int16_t UiW_GridCellX(int16_t x, int16_t size, uint8_t col)
{
    int16_t cw = UiW_GridCellW(size);
    return (int16_t)(x + (int16_t)col * (cw + 1));
}

int16_t UiW_GridCellY(int16_t y, int16_t size, uint8_t row)
{
    int16_t cw = UiW_GridCellW(size);
    return (int16_t)(y + (int16_t)row * (cw + 1));
}

void UiW_DrawKeyGrid(int16_t x, int16_t y, int16_t size, uint16_t keymap)
{
    uint8_t r, c;
    int16_t cw = UiW_GridCellW(size);

    for (r = 0u; r < 3u; r++)
    {
        for (c = 0u; c < 3u; c++)
        {
            uint8_t  k   = (uint8_t)(r * 3u + c);
            int16_t  cx  = UiW_GridCellX(x, size, c);
            int16_t  cy  = UiW_GridCellY(y, size, r);
            uint8_t  on  = (uint8_t)((keymap & (uint16_t)(1u << k)) != 0u);

            /* 按下 = 绿色，没按 = 黑底白框（支持多键同时按） */
            TFT_FillRect(cx, cy, cw, cw,
                         (on != 0u) ? (uint16_t)UI_COL_GREEN : (uint16_t)UI_COL_BG);
            TFT_DrawRect(cx, cy, cw, cw,
                         (on != 0u) ? (uint16_t)UI_COL_GREEN : (uint16_t)UI_COL_TEXT);
        }
    }
}

/*==========================================================================
 * 数值页
 *========================================================================*/
void UiW_DrawValuePage(int16_t cy, uint16_t value, uint8_t scale)
{
    int16_t w = Game_NumberWidth((uint32_t)value, scale);
    int16_t x = (int16_t)((UI_SCREEN_W - w) / 2);
    int16_t y = (int16_t)(cy - (int16_t)(8u * scale) / 2);

    TFT_FillRect(0, (int16_t)(cy - 30), (int16_t)UI_SCREEN_W, 60, (uint16_t)UI_COL_BG);

    Game_DrawNumber(x, y, (uint32_t)value, scale,
                    (uint16_t)UI_COL_FRAME, (uint16_t)UI_COL_BG);
}
