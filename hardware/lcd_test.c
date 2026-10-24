/**************************************************************************
 * lcd_test.c —— CH592X 屏幕测试程序（纯 CH592X 写法）
 *
 * 依赖：tft_port / tft_st7789 / tft_gfx（工程 hardware\ 里那几个文件）
 *       + CH592 EVT 标准外设驱动（CH59x_common.h：SetSysClock / DelayMs / SYS_GetSysTickCnt）
 **************************************************************************/

#include "CH59x_common.h"
#include "tft_port.h"
#include "tft_st7789.h"
#include "tft_gfx.h"
#include "lcd_test.h"

/*==========================================================================
 * 1. 初始化
 *========================================================================*/
void LCD_Test_Init(uint16_t w, uint16_t h)
{
    SetSysClock(CLK_SOURCE_PLL_60MHz);      /* CH592X 跑 60MHz */

    ST7789_Init(w, h);                      /* 硬复位 + 送 ST7789 初始化命令表 */
    TPORT_Bl(1);                            /* 开背光（PB4 输出低） */
}

/*==========================================================================
 * 2. 实测：SysTick 每毫秒多少个计数（不猜频率，用 DelayMs 标定一次）
 *========================================================================*/
static uint32_t lcd_ticks_per_ms(void)
{
    uint32_t t0;
    uint32_t t1;

    t0 = SYS_GetSysTickCnt();
    DelayMs(50);
    t1 = SYS_GetSysTickCnt();

    if (t1 == t0)
    {
        return 30000u;                      /* 兜底：60MHz 下 SysTick 走 Fsys/2 */
    }
    return (t1 - t0) / 50u;
}

/*==========================================================================
 * 3. 运行信息页
 *========================================================================*/
void LCD_Test_Info(void)
{
    uint16_t w = TFT_Width();
    uint16_t h = TFT_Height();

    TFT_FillScreen(ST77XX_BLACK);

    TFT_SetTextSize(2);
    TFT_SetTextColorBg(ST77XX_WHITE, ST77XX_BLACK);
    TFT_SetCursor(2, 4);
    TFT_Print("CH592X");

    TFT_SetTextSize(1);
    TFT_SetTextColorBg(ST77XX_GREEN, ST77XX_BLACK);
    TFT_SetCursor(2, 26);
    TFT_Print("ST7789 LCD test");

    TFT_SetTextColorBg(ST77XX_YELLOW, ST77XX_BLACK);
    TFT_SetCursor(2, 40);
    TFT_Printf("panel : %ux%u", (unsigned)w, (unsigned)h);

    TFT_SetCursor(2, 52);
    TFT_Printf("rot   : %u", (unsigned)TFT_GetRotation());

#if (TFT_SPI_BACKEND == TFT_SPI_HARD)
    TFT_SetCursor(2, 64);
    TFT_Print("spi   : hardware SPI0");
#else
    TFT_SetCursor(2, 64);
    TFT_Print("spi   : software (bit banging)");
#endif

    TFT_SetTextColorBg(ST77XX_CYAN, ST77XX_BLACK);
    TFT_SetCursor(2, 76);
    TFT_Printf("clock : %u MHz", (unsigned)(GetSysClock() / 1000000u));

    TFT_SetTextColorBg(ST77XX_MAGENTA, ST77XX_BLACK);
    TFT_SetCursor(2, 88);
    TFT_Printf("systick/ms : %u", (unsigned)lcd_ticks_per_ms());

    TFT_SetTextColorBg(ST77XX_WHITE, ST77XX_BLACK);
    TFT_SetCursor(2, 104);
    TFT_Print("busy-wait fill screen...");

    {
        uint32_t t0 = SYS_GetSysTickCnt();
        uint32_t t1;
        uint32_t tpm = lcd_ticks_per_ms();

        TFT_FillScreen(ST77XX_BLUE);
        t1 = SYS_GetSysTickCnt();

        TFT_SetCursor(2, 118);
        TFT_Printf("fill %ux%u : %u ms", (unsigned)w, (unsigned)h,
                   (unsigned)((t1 - t0) / tpm));
    }
}

/*==========================================================================
 * 4. 上电自检：红 -> 绿 -> 蓝
 *========================================================================*/
void LCD_Test_BringUp(void)
{
    static const uint16_t seq[3] = { ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE };
    uint8_t i;

    for (i = 0; i < 3u; i++)
    {
        TFT_FillScreen(seq[i]);
        DelayMs(500);
    }
}

/*==========================================================================
 * 5. 颜色条 + 文字 + 图形
 *========================================================================*/
void LCD_Test_Colors(void)
{
    uint16_t w = TFT_Width();
    uint16_t h = TFT_Height();
    uint16_t bar = (uint16_t)(w / 8u);
    static const uint16_t col[8] =
    {
        ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_YELLOW,
        ST77XX_CYAN, ST77XX_MAGENTA, ST77XX_WHITE, ST77XX_ORANGE
    };
    uint8_t i;

    TFT_FillScreen(ST77XX_BLACK);

    /* 顶部 8 个颜色条：顺序应该是 红 绿 蓝 黄 青 品红 白 橙 */
    for (i = 0; i < 8u; i++)
    {
        TFT_FillRect((int16_t)(i * bar), 0, (int16_t)bar, 24, col[i]);
    }

    TFT_SetTextSize(1);
    TFT_SetTextColorBg(ST77XX_WHITE, ST77XX_BLACK);
    TFT_SetCursor(4, 32);
    TFT_Print("CH592X + ST7789 OK");

    TFT_SetTextSize(2);
    TFT_SetTextColor(ST77XX_YELLOW);
    TFT_SetCursor(4, 48);
    TFT_Print("RGB565");

    TFT_SetTextSize(1);
    TFT_SetTextColor(ST77XX_GREEN);
    TFT_SetCursor(4, 72);
    TFT_Printf("screen: %ux%u", (unsigned)w, (unsigned)h);

    /* 图形：边框 / 交叉线 / 圆 / 实心圆 / 圆角矩形 / 三角 */
    TFT_DrawRect(4, 88, (int16_t)(w - 8), (int16_t)(h - 92), ST77XX_WHITE);
    TFT_DrawLine(6, 90, (int16_t)(w - 7), (int16_t)(h - 6), ST77XX_BLUE);
    TFT_DrawLine((int16_t)(w - 7), 90, 6, (int16_t)(h - 6), ST77XX_RED);
    TFT_DrawCircle((int16_t)(w / 2), (int16_t)(h - 30), 14, ST77XX_CYAN);
    TFT_FillCircle((int16_t)(w / 4), (int16_t)(h - 30), 10, ST77XX_MAGENTA);
    TFT_FillRoundRect((int16_t)(w - w / 4 - 12), (int16_t)(h - 40), 24, 20, 6, ST77XX_ORANGE);
    TFT_DrawTriangle((int16_t)(w / 2), (int16_t)(h - 46),
                     (int16_t)(w / 2 - 10), (int16_t)(h - 30),
                     (int16_t)(w / 2 + 10), (int16_t)(h - 30), ST77XX_GREEN);
}

/*==========================================================================
 * 6. 四方向动画：小方块扫描 + 逐行刷屏
 *========================================================================*/
void LCD_Test_Animate(void)
{
    uint16_t w;
    uint16_t h;
    int16_t  x;
    int16_t  y;
    uint8_t  r;

    for (r = 0; r < 4u; r++)
    {
        TFT_SetRotation(r);
        w = TFT_Width();
        h = TFT_Height();

        TFT_FillScreen(ST77XX_BLACK);
        TFT_SetTextSize(2);
        TFT_SetTextColorBg(ST77XX_WHITE, ST77XX_BLACK);
        TFT_SetCursor(4, 4);
        TFT_Printf("ROT %u", (unsigned)r);

        TFT_SetTextSize(1);
        TFT_SetCursor(4, 24);
        TFT_Printf("%ux%u", (unsigned)w, (unsigned)h);

        /* 小方块从左扫到右 */
        y = (int16_t)(h / 2 - 4);
        for (x = 0; x < (int16_t)(w - 8); x += 6)
        {
            TFT_FillRect(x, y, 8, 8, ST77XX_BLUE);
            DelayMs(8);
            TFT_FillRect(x, y, 8, 8, ST77XX_BLACK);
        }

        /* 逐行刷屏：颜色渐变，能看出满屏刷新要多快 */
        for (y = 0; y < (int16_t)h; y += 4)
        {
            TFT_DrawFastHLine(0, y, w, (uint16_t)((y * 4) & 0xFFE0u));
        }
        DelayMs(400);
    }

    TFT_SetRotation(0);
}

/*==========================================================================
 * 7. 满屏填色耗时实测
 *========================================================================*/
uint32_t LCD_Test_Benchmark(void)
{
    uint32_t tpm = lcd_ticks_per_ms();
    uint32_t t0;
    uint32_t t1;
    uint32_t ms;

    t0 = SYS_GetSysTickCnt();
    TFT_FillScreen(ST77XX_RED);
    t1 = SYS_GetSysTickCnt();
    ms = (t1 - t0) / tpm;

    TFT_SetTextSize(1);
    TFT_SetTextColorBg(ST77XX_WHITE, ST77XX_RED);
    TFT_SetCursor(4, 4);
    TFT_Printf("fill screen = %u ms", (unsigned)ms);

    return ms;
}

/*==========================================================================
 * 8. 整套跑一遍
 *========================================================================*/
void LCD_Test_Run(uint16_t w, uint16_t h)
{
    LCD_Test_Init(w, h);

    LCD_Test_Info();            /* 运行信息 + 满屏耗时 */
    DelayMs(2500);

    LCD_Test_BringUp();         /* 红 -> 绿 -> 蓝 */
    LCD_Test_Colors();          /* 颜色条 / 文字 / 图形 */
    DelayMs(2500);

    (void)LCD_Test_Benchmark();
    DelayMs(2000);

    while (1)
    {
        LCD_Test_Animate();
    }
}

/******************* (C) CH592X 屏幕测试 *****************/
