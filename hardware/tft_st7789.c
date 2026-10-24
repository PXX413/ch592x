/**************************************************************************
 * tft_st7789.c —— ST7789 SPI TFT 屏驱动（核心 + 面板层，合并为一个源文件）
 *
 *   移植自 Adafruit_ST77xx.cpp / Adafruit_ST7789.cpp (Adafruit, MIT License)
 *     - Adafruit_SPITFT 的 sendCommand/writeCommand/spiWrite 用
 *       TFT_SendCommand / TFT_WriteCommand / TFT_WriteData32 / TPORT_* 实现
 *     - pgm_read_byte(addr) -> TFT_PGM_READ(addr)（MCU 直接读 Flash）
 *     - delay(ms) -> DelayMs(ms)；boolean -> bool
 *     - 只保留 ST7789 一条路径；原 tft_st77xx.c + tft_st7789.c 合并到本文件
 *
 *   本文件平台无关：GPIO/SPI/延时通过 tft_port.h 的 TPORT_* 接口，
 *   CH592(WCH RISC-V) 与 STM32F103 两个版本共用同一份代码。
 **************************************************************************/

#include "tft_st7789.h"

/*==========================================================================
 * 第一部分：驱动核心（原 tft_st77xx.c）
 *========================================================================*/

/* 全局单实例：对应原来 Arduino 里 global 的 Adafruit_ST77xx 对象 */
TFT_t tft;

/* 一次批量写像素用的中转缓冲（32 像素 = 64 字节，省 RAM） */
/* 每次 SPI 事务写多少个像素。原来是 32（64 字节）——一次全屏 240x135
 * 要发 1013 次 SPI 事务，每次都有固定开销。放到 256 后同样一屏只要
 * 127 次。缓冲 512 字节，RAM 还放得下。 */
#define TFT_CHUNK_PIXELS   128
static uint8_t s_chunk[TFT_CHUNK_PIXELS * 2];

/*==========================================================================
 * SPI 事务控制
 *========================================================================*/
/* 【性能计数】每次都累加，几乎不要钱，配合 LOOP_DIAG 显示在屏幕底部。
 *   g_tft_ops = TFT_SetAddrWindow() 调用次数 = "昂贵绘制操作"次数
 *               （每次 = 11 字节地址窗口命令，约 5 次独立 SPI 事务）
 *   g_tft_px  = 实际写出去的像素数 */
volatile uint32_t g_tft_ops;
volatile uint32_t g_tft_px;

/* FW38 【重入保护】>0 = 正处在一次 TFT_BeginWrite()/TFT_EndWrite() 事务里。
 * TMR0 中断里的卡死红条要先查这个标志：绝不能在主线程正做 SPI0 事务时
 * 抢总线 —— 那一笔嵌套传输会把 R16_SPI0_TOTAL_CNT 冲掉，主线程就卡死在
 * SPI0_MasterTrans() 的等待循环里永远出不来。*/
volatile uint8_t  g_tft_busy = 0u;

void TFT_BeginWrite(void)
{
    g_tft_busy++;
    TPORT_Cs(0);            /* CS = 低，选中屏幕 */
}

void TFT_EndWrite(void)
{
    TPORT_Cs(1);            /* CS = 高 */
    if (g_tft_busy != 0u) { g_tft_busy--; }
}

/*********************************************************************
 * @brief  发一个命令字节，不改动 CS（必须在已经拉低 CS 的事务里使用）
 *         对应 Adafruit_SPITFT 内部直接 spiWrite() + DC 控制的写法
 *********************************************************************/
static void write_command_raw(uint8_t cmd)
{
    TPORT_Dc(0);            /* DC = 0 -> 命令 */
    TPORT_WriteByte(cmd);
}

/*********************************************************************
 * @brief  独立发送一个命令（自带 CS 事务）
 *         适合用户自己写新命令时调用；驱动内部配置窗口时用 raw 版本
 *********************************************************************/
void TFT_WriteCommand(uint8_t cmd)
{
    TFT_BeginWrite();
    write_command_raw(cmd);
    TFT_EndWrite();
}

void TFT_WriteData32(uint32_t dat)
{
    TPORT_Dc(1);
    TPORT_WriteByte((uint8_t)(dat >> 24));
    TPORT_WriteByte((uint8_t)(dat >> 16));
    TPORT_WriteByte((uint8_t)(dat >> 8));
    TPORT_WriteByte((uint8_t)(dat));
}

/*********************************************************************
 * @fn      TFT_SendCommand
 *
 * @brief   对应 Adafruit_SPITFT::sendCommand(cmd, data, len)
 *          一条命令 + len 个参数，整段 CS 保持低电平
 *********************************************************************/
void TFT_SendCommand(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    TFT_BeginWrite();
    write_command_raw(cmd);
    if ((data != 0) && (len > 0))
    {
        TPORT_Dc(1);
        TPORT_WriteBytes(data, len);
    }
    TFT_EndWrite();
}

/*********************************************************************
 * @fn      TFT_DisplayInit
 *
 * @brief   对应 Adafruit_ST77xx::displayInit()
 *          按表发送 LCD 初始化命令：格式与 Adafruit 库完全一致，
 *          因此原库里的命令表（如 ST7789 的 generic_st7789）可以原样搬过来。
 *
 *          表格式： [命令条数]
 *                   [命令, 参数个数|ST_CMD_DELAY, 参数..., (延时ms)]
 *                   ...
 *********************************************************************/
void TFT_DisplayInit(const uint8_t *addr)
{
    uint8_t  numCommands;
    uint8_t  cmd;
    uint8_t  numArgs;
    uint16_t ms;

    numCommands = TFT_PGM_READ(addr++);         /* 后面跟随的命令条数 */

    while (numCommands--)
    {
        cmd     = TFT_PGM_READ(addr++);         /* 读命令           */
        numArgs = TFT_PGM_READ(addr++);         /* 读参数个数       */

        ms       = numArgs & ST_CMD_DELAY;      /* 高位表示后面有延时 */
        numArgs &= ~ST_CMD_DELAY;               /* 屏蔽掉延时标志位   */

        TFT_SendCommand(cmd, addr, numArgs);
        addr += numArgs;

        if (ms)
        {
            ms = TFT_PGM_READ(addr++);          /* 读延时时间(ms)   */
            if (ms == 255)
            {
                ms = 500;                       /* 255 约定为 500ms */
            }
            DelayMs(ms);
        }
    }
}

/*********************************************************************
 * @fn      TFT_SetAddrWindow
 *
 * @brief   设置一个矩形写入窗口（对应 Adafruit_ST77xx::setAddrWindow）
 *
 *          注意：本函数不改变 CS（内部也不会抬 CS），必须自己包在
 *          TFT_BeginWrite() / TFT_EndWrite() 之间调用，
 *          而且随后的像素数据要在同一个 CS 低电平期间发完
 *          （GFX 层的绘图函数内部已经处理好了）。
 *
 *          如果像早先那样在这里调用 TFT_WriteCommand()，命令字节发完
 *          就会抬 CS，4 个参数会被屏幕当成"新命令"丢掉 —— 现象是花屏。
 *********************************************************************/
void TFT_SetAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint32_t xa;
    uint32_t ya;
    uint8_t  xb[4];
    uint8_t  yb[4];

    g_tft_ops++;

    x += (uint16_t)tft._xstart;
    y += (uint16_t)tft._ystart;

    xa = ((uint32_t)x << 16) | (uint32_t)(x + w - 1);
    ya = ((uint32_t)y << 16) | (uint32_t)(y + h - 1);

    xb[0] = (uint8_t)(xa >> 24);
    xb[1] = (uint8_t)(xa >> 16);
    xb[2] = (uint8_t)(xa >> 8);
    xb[3] = (uint8_t)(xa);
    yb[0] = (uint8_t)(ya >> 24);
    yb[1] = (uint8_t)(ya >> 16);
    yb[2] = (uint8_t)(ya >> 8);
    yb[3] = (uint8_t)(ya);

    /* 【性能 2026-10-12】按 DC 电平分段发送，11 次 SPI 事务 -> 5 次。
     *
     *   原来这里是 write_command_raw() + TFT_WriteData32()，而 TFT_WriteData32
     *   把 4 个字节拆成了 4 次 TPORT_WriteByte，每次 TPORT_WriteByte 都是一笔
     *   完整的 SPI 事务（写 R8_SPI0_BUFFER + 自旋等 RB_SPI_IF_FREE）。
     *   合计 1+4+1+4+1 = 11 笔事务，对一次小 FillRect 来说，窗口设置的开销
     *   比它自己的像素数据还贵（60~70%）。
     *
     *   DC 在这几条之间要翻转（CASET/RASET/RAMWR 是命令，中间 4 字节是参数），
     *   而 SPI 收发期间无法改 DC，所以 5 笔是下限 —— 但已经省掉一半以上。
     *
     *   本函数是所有绘制（FillRect / DrawPixel / DrawLine / 位图 / 圆角）的
     *   必经之路，所以这一处的收益会放大到整个 UI。 */
    write_command_raw(ST77XX_CASET);        /* 列地址设置：命令（DC=0） */
    TPORT_Dc(1);
    TPORT_WriteBytes(xb, 4u);               /* 4 字节参数：数据（DC=1） */

    write_command_raw(ST77XX_RASET);        /* 行地址设置：命令 */
    TPORT_Dc(1);
    TPORT_WriteBytes(yb, 4u);               /* 4 字节参数：数据 */

    write_command_raw(ST77XX_RAMWR);        /* 开始写显存：命令 */
}

/*==========================================================================
 * 像素数据输出（需在 SetAddrWindow 之后、同一次 CS 事务内调用）
 *========================================================================*/
void TFT_PushColor(uint16_t color)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(color >> 8);
    buf[1] = (uint8_t)(color & 0xFF);

    TPORT_Dc(1);
    TPORT_WriteBytes(buf, 2);
}

void TFT_PushColors(const uint16_t *colors, uint32_t count)
{
    uint32_t i;
    uint32_t n;

    if ((colors == 0) || (count == 0))
    {
        return;
    }

    TPORT_Dc(1);
    while (count)
    {
        n = (count > TFT_CHUNK_PIXELS) ? TFT_CHUNK_PIXELS : count;
        for (i = 0; i < n; i++)
        {
            s_chunk[2 * i]     = (uint8_t)(colors[i] >> 8);         /* 高字节先发 */
            s_chunk[2 * i + 1] = (uint8_t)(colors[i] & 0xFF);
        }
        TPORT_WriteBytes(s_chunk, (uint16_t)(n * 2));
        colors += n;
        count  -= n;
    }
}

/*********************************************************************
 * @fn      TFT_FillWindow
 *
 * @brief   往当前窗口里连续填 count 个同样的颜色（整屏/矩形填充用）
 *********************************************************************/
void TFT_FillWindow(uint16_t color, uint32_t count)
{
    g_tft_px += count;
    uint32_t i;
    uint32_t n;

    if (count == 0)
    {
        return;
    }

    /* 【2026-10-18 优化】只把这一帧真正要发的像素数填进缓冲。
     *
     * 原来无论 count 多小都先填满 TFT_CHUNK_PIXELS(128) 个像素 ——
     * 而 UI 里大量调用是"擦一小块"（几个像素到几十个像素），
     * 于是每画一个小矩形都要白算 128 次 16 位拆字节。
     * 填 min(count, TFT_CHUNK_PIXELS) 个就够：后面 while 循环里
     * 第一次取的就是这个数，后续分块复用同一段缓冲，语义完全不变。 */
    n = (count > (uint32_t)TFT_CHUNK_PIXELS) ? (uint32_t)TFT_CHUNK_PIXELS : count;

    for (i = 0; i < n; i++)
    {
        s_chunk[2 * i]     = (uint8_t)(color >> 8);
        s_chunk[2 * i + 1] = (uint8_t)(color & 0xFF);
    }

    TPORT_Dc(1);
    while (count)
    {
        n = (count > TFT_CHUNK_PIXELS) ? TFT_CHUNK_PIXELS : count;
        TPORT_WriteBytes(s_chunk, (uint16_t)(n * 2));
        count -= n;
    }
}
/*********************************************************************
 * @fn      TFT_DrawMonoBitmap
 *
 * @brief   1bpp 单色位图 -> RGB565，整块刷到屏上。
 *
 *          整个字形只设【1 次】地址窗口，像素按行展开后分块流式写出。
 *          对比逐像素画法（每像素一次地址窗口 = 11 字节命令 + 约 5 次
 *          SPI 事务），这是画点阵字库唯一正确的做法。
 *
 * @param   x,y    目标左上角
 * @param   data   1bpp 位图，MSB 在前，行优先
 * @param   w,h    位图像素宽高
 * @param   stride 每行字节数 = (w + 7) / 8
 * @param   fg,bg  前景 / 背景色
 *********************************************************************/
void TFT_DrawMonoBitmap(int16_t x, int16_t y, const uint8_t *data,
                        uint16_t w, uint16_t h, uint16_t stride,
                        uint16_t fg, uint16_t bg)
{
    uint16_t row;
    uint16_t col;
    uint16_t n = 0;

    if ((data == 0) || (w == 0) || (h == 0)) { return; }
    if ((x < 0) || (y < 0)) { return; }
    if (((int32_t)x + (int32_t)w > (int32_t)tft._width) ||
        ((int32_t)y + (int32_t)h > (int32_t)tft._height)) { return; }

    TFT_BeginWrite();
    TFT_SetAddrWindow((uint16_t)x, (uint16_t)y, w, h);
    TPORT_Dc(1);

    for (row = 0; row < h; row++)
    {
        const uint8_t *src = data + (uint32_t)row * (uint32_t)stride;

        for (col = 0; col < w; col++)
        {
            uint16_t c = (((src[col >> 3] >> (7u - (col & 7u))) & 1u) != 0u)
                         ? fg : bg;

            s_chunk[2 * n]     = (uint8_t)(c >> 8);
            s_chunk[2 * n + 1] = (uint8_t)(c & 0xFFu);
            n++;

            if (n == TFT_CHUNK_PIXELS)
            {
                TPORT_WriteBytes(s_chunk, (uint16_t)(n * 2u));
                n = 0;
            }
        }
    }

    if (n != 0)
    {
        TPORT_WriteBytes(s_chunk, (uint16_t)(n * 2u));
    }

    TFT_EndWrite();
}


/*==========================================================================
 * 初始化
 *========================================================================*/
/*********************************************************************
 * @fn      TFT_Begin
 *
 * @brief   对应 Adafruit_ST77xx::begin()：设定反显命令 + 初始化 SPI/IO
 *********************************************************************/
void TFT_Begin(void)
{
    tft.invertOnCommand  = ST77XX_INVON;
    tft.invertOffCommand = ST77XX_INVOFF;

    TPORT_Init();           /* 引脚 + SPI（软件模拟 或 硬件 SPI） */
}

void TFT_CommonInit(const uint8_t *cmdList)
{
    TFT_Begin();

    if (cmdList)
    {
        TFT_DisplayInit(cmdList);
    }
}

/*==========================================================================
 * 旋转 / 显示开关
 *   TFT_SetRotation() 的实现见本文件末尾【第二部分：面板层】。
 *========================================================================*/
void TFT_SetColRowStart(int8_t col, int8_t row)
{
    tft._colstart = col;
    tft._rowstart = row;
}

/*********************************************************************
 * @brief  改变显示屏开关状态（对应 Adafruit_ST77xx::enableDisplay）
 *********************************************************************/
void TFT_EnableDisplay(bool enable)
{
    TFT_SendCommand(enable ? ST77XX_DISPON : ST77XX_DISPOFF, 0, 0);
}

/*********************************************************************
 * @brief  TE 撕裂信号输出开关（对应 enableTearing）
 *********************************************************************/
void TFT_EnableTearing(bool enable)
{
    TFT_SendCommand(enable ? ST77XX_TEON : ST77XX_TEOFF, 0, 0);
}

/*********************************************************************
 * @brief  睡眠模式开关（对应 enableSleep）
 *********************************************************************/
void TFT_EnableSleep(bool enable)
{
    TFT_SendCommand(enable ? ST77XX_SLPIN : ST77XX_SLPOUT, 0, 0);
}



/*==========================================================================
 * 第二部分：面板层（原 tft_st7789.c）
 *   初始化命令表 / ST7789_Init / TFT_SetRotation
 *========================================================================*/

/*==========================================================================
 * ST7789 初始化命令表（9 条命令）
 *========================================================================*/
/* clang-format off */
static const uint8_t generic_st7789[] = {
    9,                                    /* 共 9 条命令                   */
    ST77XX_SWRESET,   ST_CMD_DELAY,       /* 1: 软件复位                   */
        150,
    ST77XX_SLPOUT,    ST_CMD_DELAY,       /* 2: 退出睡眠                   */
        10,
    ST77XX_COLMOD,    1 + ST_CMD_DELAY,   /* 3: 颜色模式 = 16bit (0x55)    */
        0x55,
        10,
    ST77XX_MADCTL,    1,                  /* 4: 扫描方向                   */
        0x08,
    ST77XX_CASET,     4,                  /* 5: 列地址 0 ~ 239             */
        0x00,                     0,
        0,                        240,
    ST77XX_RASET,     4,                  /* 6: 行地址 0 ~ 319             */
        0x00,                     0,
        320 >> 8,                 320 & 0xFF,
    ST77XX_INVON,     ST_CMD_DELAY,       /* 7: 反显（这块屏要开）         */
        10,
    ST77XX_NORON,     ST_CMD_DELAY,       /* 8: 正常显示模式               */
        10,
    ST77XX_DISPON,    ST_CMD_DELAY,       /* 9: 开显示                     */
        10
};
/* clang-format on */

/*********************************************************************
 * @fn      ST7789_Init
 *
 * @brief   ST7789 初始化（对应 Adafruit_ST7789::init）
 * @param   width  - 屏宽 240 或 135
 * @param   height - 屏高 320 / 240
 *********************************************************************/
void ST7789_Init(uint16_t width, uint16_t height)
{
    TPORT_InitPins();                   /* 先配好 IO，才能拉 RST */
    TPORT_HardwareReset();              /* 硬复位                */

    /* 对应原库 commonInit(NULL)：初始化 SPI + 反显命令（此处不带命令表） */
    TFT_CommonInit(0);

    /* 不同尺寸的面板，显存里的偏移量不一样（原库逻辑照搬） */
    if ((width == 240) && (height == 240))
    {
        /* 1.3" / 1.54" 方屏（靠右下对齐） */
        tft._rowstart  = (int16_t)(320 - height);
        tft._rowstart2 = 0;
        tft._colstart  = (int16_t)(240 - width);
        tft._colstart2 = (int16_t)(240 - width);
    }
    else if ((width == 135) && (height == 240))
    {
        /* 1.14" 小屏（居中，宽为奇数） */
        tft._rowstart  = (int16_t)((320 - height) / 2);
        tft._rowstart2 = tft._rowstart;
        /* 多出来的 1 个像素必须放在 _colstart，不能放 _colstart2 */
        tft._colstart  = (int16_t)((240 - width + 1) / 2);
        tft._colstart2 = (int16_t)((240 - width) / 2);
    }
    else
    {
        /* 1.47" / 1.69" / 1.9" / 2.0" 等（居中） */
        tft._rowstart  = (int16_t)((320 - height) / 2);
        tft._rowstart2 = tft._rowstart;
        tft._colstart  = (int16_t)((240 - width) / 2);
        tft._colstart2 = tft._colstart;
    }

    tft.windowWidth  = width;
    tft.windowHeight = height;

    TFT_DisplayInit(generic_st7789);
    TFT_SetRotation(0);
}

/*********************************************************************
 * @fn      TFT_SetRotation
 *
 * @brief   设置原点位置与显示方向（对应 Adafruit_ST7789::setRotation）
 *          本工程只有 ST7789，所以直接实现公共接口 TFT_SetRotation()
 * @param   m - 旋转 0~3
 *********************************************************************/
void TFT_SetRotation(uint8_t m)
{
    uint8_t madctl = 0;

    tft.rotation = m & 3;               /* 不能大于 3 */

    switch (tft.rotation)
    {
        case 0:
            madctl      = ST77XX_MADCTL_MX | ST77XX_MADCTL_MY | ST77XX_MADCTL_RGB;
            tft._xstart = tft._colstart;
            tft._ystart = tft._rowstart;
            tft._width  = tft.windowWidth;
            tft._height = tft.windowHeight;
            break;

        case 1:
            madctl      = ST77XX_MADCTL_MY | ST77XX_MADCTL_MV | ST77XX_MADCTL_RGB;
            tft._xstart = tft._rowstart;
            tft._ystart = tft._colstart2;
            tft._height = tft.windowWidth;
            tft._width  = tft.windowHeight;
            break;

        case 2:
            madctl      = ST77XX_MADCTL_RGB;
            tft._xstart = tft._colstart2;
            tft._ystart = tft._rowstart2;
            tft._width  = tft.windowWidth;
            tft._height = tft.windowHeight;
            break;

        case 3:
            madctl      = ST77XX_MADCTL_MX | ST77XX_MADCTL_MV | ST77XX_MADCTL_RGB;
            tft._xstart = tft._rowstart2;
            tft._ystart = tft._colstart;
            tft._height = tft.windowWidth;
            tft._width  = tft.windowHeight;
            break;

        default:
            break;
    }

    TFT_SendCommand(ST77XX_MADCTL, &madctl, 1);
}

/******************* (C) 移植自 Adafruit ST7789 库, MIT License *****************/
