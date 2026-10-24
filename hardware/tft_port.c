/**************************************************************************
 * tft_port.c —— ST77xx 驱动在 CH592 上的硬件抽象层实现
 *
 * 把原 Adafruit_SPITFT 里的 digitalWrite()/pinMode()/SPI 调用，
 * 全部换成 CH592 EVT 原生 API：
 *
 *   原 Arduino 写法                     本文件 CH592 写法
 *   ------------------------------------------------------------------
 *   pinMode(p, OUTPUT)                  GPIOA_ModeCfg(mask, GPIO_ModeOut_PP_5mA)
 *   digitalWrite(p, HIGH)               GPIOA_SetBits(mask)
 *   digitalWrite(p, LOW)                GPIOA_ResetBits(mask)
 *   SPI.transfer(b) (软件 SPI)          直接翻转 SCLK/MOSI 寄存器 (本文件)
 *   SPI.transfer(b) (硬件 SPI)          SPI0_MasterSendByte(b)
 *   SPI.writeBytes(buf,n)               SPI0_MasterTrans(buf, n)
 *   delay(ms) / delayMicroseconds(us)   DelayMs() / DelayUs()
 **************************************************************************/

#include "tft_port.h"

#ifndef __nop
#define __nop()   asm volatile("nop")
#endif

#if (TFT_SPI_BACKEND == TFT_SPI_SOFT)

/* 软件 SPI 半周期延时 */
#if (TFT_SW_SPI_NOP > 0)
#define SW_DLY()                                                           \
    do {                                                                   \
        uint8_t _n = (TFT_SW_SPI_NOP);                                     \
        while (_n--) { __nop(); }                                          \
    } while (0)
#else
#define SW_DLY()   ((void)0)
#endif

/*==========================================================================
 * 软件模拟 SPI：模式 0（CPOL=0, CPHA=0），MSB 先行
 *   数据在 SCLK 低电平期间变化 -> 上升沿被从机采样 -> 拉低
 *========================================================================*/
static void sw_write_byte(uint8_t dat)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        if (dat & 0x80)
        {
            TFT_GPIO_SET(TFT_PIN_MOSI_PORT, TFT_PIN_MOSI);
        }
        else
        {
            TFT_GPIO_CLR(TFT_PIN_MOSI_PORT, TFT_PIN_MOSI);
        }
        SW_DLY();

        TFT_GPIO_SET(TFT_PIN_SCLK_PORT, TFT_PIN_SCLK);   /* 上升沿：LCD 采样 */
        SW_DLY();
        TFT_GPIO_CLR(TFT_PIN_SCLK_PORT, TFT_PIN_SCLK);   /* 下降沿：换数据   */

        dat <<= 1;
    }
}

#endif /* TFT_SPI_BACKEND == TFT_SPI_SOFT */

/*==========================================================================
 * 初始化
 *========================================================================*/
/*********************************************************************
 * @fn      TPORT_InitPins
 *
 * @brief   配置 CS/DC/RST/BL/SCLK/MOSI 的 IO 方向与空闲电平。
 *          必须先于 TPORT_HardwareReset() 调用（否则 RST 无法被拉低）。
 *********************************************************************/
void TPORT_InitPins(void)
{
#if (TFT_USES_DEBUG_PINS)
    /* 用到 PB14/PB15 时必须关掉仿真调试口，否则这两个脚不受 GPIO 控制 */
    R16_PIN_ALTERNATE |= RB_DEBUG_EN;
#endif

    /* CS / DC / RST / BL 一律推挽输出，并置成空闲电平 */
    TFT_GPIO_OUT_CFG(TFT_PIN_CS_PORT, TFT_PIN_CS);
    TFT_GPIO_OUT_CFG(TFT_PIN_DC_PORT, TFT_PIN_DC);

    TFT_GPIO_SET(TFT_PIN_CS_PORT, TFT_PIN_CS);       /* CS   = 1 不选中 */
    TFT_GPIO_CLR(TFT_PIN_DC_PORT, TFT_PIN_DC);       /* DC   = 0 命令   */

#if (TFT_USE_RST)
    TFT_GPIO_OUT_CFG(TFT_PIN_RST_PORT, TFT_PIN_RST);
    TFT_GPIO_SET(TFT_PIN_RST_PORT, TFT_PIN_RST);     /* RST  = 1 释放   */
#endif

#if (TFT_USE_BL)
    /* BLK 走 PWM7（5kHz 可调亮度），配置在 TPORT_BlPwmInit() 里，
     * 由 TFT_Init 末尾调用。这里不再把它配成普通 GPIO 开关。 */
    TPORT_BlPwmInit();
    /* 先给"熄灭"电平，等 TPORT_Bl(1) 再点亮，避免上电瞬间背光闪一下。 */
#if (TFT_BL_ACTIVE_HIGH)
    TFT_GPIO_CLR(TFT_PIN_BL_PORT, TFT_PIN_BL);       /* 高=亮 -> 先给低(灭) */
#else
    TFT_GPIO_SET(TFT_PIN_BL_PORT, TFT_PIN_BL);       /* 低=亮 -> 先给高(灭) */
#endif
#endif

    /* SCLK / MOSI 也是推挽输出 */
    TFT_GPIO_OUT_CFG(TFT_PIN_SCLK_PORT, TFT_PIN_SCLK);
    TFT_GPIO_OUT_CFG(TFT_PIN_MOSI_PORT, TFT_PIN_MOSI);
}

/*********************************************************************
 * @fn      TPORT_Init
 *
 * @brief   引脚配置 + SPI 初始化（每次屏幕初始化调用一次）
 *********************************************************************/
void TPORT_Init(void)
{
    TPORT_InitPins();

#if (TFT_SPI_BACKEND == TFT_SPI_HARD)
    /* --- 硬件 SPI0 --- */
    /* 若使用 PB13/PB14 重映射脚，要先打开 SPI0 引脚重映射
     * （PB14/PB15 是调试口的处理已经在 TPORT_InitPins 里做了） */
    if ((TFT_PIN_SCLK_PORT == 1) || (TFT_PIN_MOSI_PORT == 1))
    {
        GPIOPinRemap(ENABLE, RB_PIN_SPI0);   /* SPI0 移到 PB12~PB15 */
    }

    SPI0_MasterDefInit();                    /* 模式0 + 3线全双工 + Fsys/4 */
    SPI0_CLKCfg(TFT_HW_SPI_DIV);             /* 重新设定 SCK 分频         */
    SPI0_DataMode(Mode0_HighBitINFront);     /* 模式0 + 高位(MSB)先行     */
#else
    /* --- 软件 SPI：SCLK / MOSI 空闲电平 --- */
    TFT_GPIO_CLR(TFT_PIN_SCLK_PORT, TFT_PIN_SCLK);   /* 空闲：SCLK = 0 */
    TFT_GPIO_CLR(TFT_PIN_MOSI_PORT, TFT_PIN_MOSI);   /* 空闲：MOSI = 0 */
#endif
}

/*==========================================================================
 * 控制线
 *========================================================================*/
void TPORT_Cs(uint8_t level)
{
    if (level) { TFT_GPIO_SET(TFT_PIN_CS_PORT, TFT_PIN_CS); }
    else       { TFT_GPIO_CLR(TFT_PIN_CS_PORT, TFT_PIN_CS); }
}

void TPORT_Dc(uint8_t level)
{
    if (level) { TFT_GPIO_SET(TFT_PIN_DC_PORT, TFT_PIN_DC); }
    else       { TFT_GPIO_CLR(TFT_PIN_DC_PORT, TFT_PIN_DC); }
}

void TPORT_Rst(uint8_t level)
{
#if (TFT_USE_RST)
    if (level) { TFT_GPIO_SET(TFT_PIN_RST_PORT, TFT_PIN_RST); }
    else       { TFT_GPIO_CLR(TFT_PIN_RST_PORT, TFT_PIN_RST); }
#else
    (void)level;
#endif
}

static uint8_t s_bl_pct = 100u;         /* 当前屏幕亮度，TPORT_Bl(1) 用它恢复 */
static uint8_t s_bl_on  = 0u;           /* 背光当前是否点亮；上电默认灭 */

/* 5kHz 的 PWM 周期：12000 计数 @60MHz = 200us */
#define TPORT_BL_CYCLE      12000u

/* 真正写占空比：把 pct(0~100) 换算成 PWM7 的计数值 */
static void tport_bl_duty(uint8_t pct)
{
    uint32_t duty;

    if (pct > 100u) { pct = 100u; }

    duty = ((uint32_t)TPORT_BL_CYCLE * (uint32_t)pct) / 100u;

#if (TFT_BL_ACTIVE_HIGH)
    R16_PWM7_DATA = (uint16_t)duty;
#else
    R16_PWM7_DATA = (uint16_t)((uint32_t)TPORT_BL_CYCLE - duty);
#endif
}

void TPORT_BlPwmInit(void)
{
#if (TFT_USE_BL)
    GPIOB_ModeCfg(TFT_PIN_BL, GPIO_ModeOut_PP_5mA);

    PWMX_CLKCfg(1);                                        /* 1 计数 = 1/Fsys = 16.67ns */
    PWMX_16bit_CycleCfg((uint16_t)(TPORT_BL_CYCLE - 1u));  /* 200us -> 5kHz */

    s_bl_on = 0u;
    tport_bl_duty(0u);                                     /* 先灭，避免上电闪一下 */
    PWMX_16bit_ACTOUT(CH_PWM7, 0u, High_Level, ENABLE);
#endif
}

void TPORT_BlSetBright(uint8_t pct)
{
    if (pct < 10u) { pct = 10u; }      /* 下限 10：别把自己调成全黑、调不回来（与设置里的范围一致） */
    s_bl_pct = pct;

    /* 只在背光已经点亮时立刻改占空比。开机阶段会先从 Flash 读亮度，
     * 如果这里无条件写 PWM，背光会在第一帧还没画完时就亮起来，正是
     * "上电花屏一帧"的来源。灭着的时候只记下亮度，等 TPORT_Bl(1) 用。 */
    if (s_bl_on != 0u)
    {
        tport_bl_duty(pct);
    }
}

uint8_t TPORT_BlGetBright(void)
{
    return s_bl_pct;
}

void TPORT_Bl(uint8_t level)
{
#if (TFT_USE_BL)
    /* 背光现在由 PWM 驱动（见 TPORT_BlPwmInit），所以
     * "开" = 恢复设置里的亮度、"关" = 占空比 0。
     * 休眠/唤醒只走这两个状态，具体亮度由设置页决定。 */
    s_bl_on = (level != 0u) ? 1u : 0u;
    tport_bl_duty((s_bl_on != 0u) ? s_bl_pct : 0u);
#else
    (void)level;
#endif
}


/*********************************************************************
 * @fn      TPORT_HardwareReset
 *
 * @brief   ST77xx 硬件复位时序：RST 低 20us 以上，再拉高等 120ms。
 *          （对应 Adafruit_SPITFT 里对 _rst 引脚的处理）
 *********************************************************************/
/* 【2026-10-14 诊断】TFT 硬件复位计数器。
 *
 * 现场 W=141ms 是个【固定时长】的阻塞（SPI 从 3 分频提到 2 分频后 W 没变），
 * 而本函数里 DelayMs(10) + DelayMs(120) = 130ms（非 RST 分支是 DelayMs(150)），
 * 是全工程唯一能产生这种固定 140ms 停顿的地方。
 *
 * 屏幕上 res 那个数只要在涨，就说明有人在反复触发 TFT 初始化 ——
 * 那 141ms 就是它，而且它也是蓝牙被堵死的原因。 */
volatile uint32_t g_tft_reset_cnt = 0u;

void TPORT_HardwareReset(void)
{
    g_tft_reset_cnt++;
#if (TFT_USE_RST)
    TPORT_Rst(1);
    DelayMs(10);
    TPORT_Rst(0);            /* 拉低复位，数据手册要求 >10us */
    DelayUs(50);
    TPORT_Rst(1);            /* 释放复位 */
    DelayMs(120);            /* 等内部稳压/振荡器稳定 */
#else
    DelayMs(150);
#endif
}

/*==========================================================================
 * SPI 数据输出
 *========================================================================*/
void TPORT_WriteByte(uint8_t dat)
{
#if (TFT_SPI_BACKEND == TFT_SPI_HARD)
    SPI0_MasterSendByte(dat);
#else
    sw_write_byte(dat);
#endif
}

void TPORT_WriteBytes(const uint8_t *buf, uint16_t len)
{
    if ((buf == 0) || (len == 0))
    {
        return;
    }

#if (TFT_SPI_BACKEND == TFT_SPI_HARD)
    /* SPI0_MasterTrans 单次最多 4095 字节，这里分段发送。
     * EVT 原型没有 const，这里强制转换（该函数只读缓冲区）。 */
    while (len)
    {
        uint16_t n = (len > 4095) ? 4095 : len;
        SPI0_MasterTrans((uint8_t *)buf, n);
        buf += n;
        len -= n;
    }
#else
    while (len--)
    {
        sw_write_byte(*buf++);
    }
#endif
}

/******************* (C) 移植自 Adafruit ST77xx 库, MIT License *****************/
