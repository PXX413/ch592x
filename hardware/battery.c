/**************************************************************************
 * battery.c —— CH592X 电池电压 / 电量检测
 *
 * 测量流程（每次调用 BAT_ReadMv() / BAT_ReadPercent() 都会走一遍）：
 *     1) VBAT_EN(PB14) 输出高电平 —— 打开分压器 / 使能 MOS
 *     2) 等 BAT_EN_SETTLE_MS（默认 500ms）
 *     3) 连续采 BAT_ADC_SAMPLES 次取平均（ADC 通道 = BAT_ADC_CHANNEL）
 *     4) 关断 VBAT_EN（默认低，降低静态功耗）
 *     5) 原始值 + 粗调校准值  ->  引脚电压 mV  ->  × 分压比  ->  电池电压 mV
 *     6) 查 OCV 表得到电量 %
 *
 * 注意：函数里有 500ms 的阻塞延时，不要在中断里调用；
 *       建议 1~2 秒调用一次即可（电池电压变化很慢）。
 **************************************************************************/

#include "battery.h"
#include "CH59x_common.h"   /* __risc_v_disable_irq / __risc_v_enable_irq */

/*==========================================================================
 * OCV 表：电压(mV) -> 电量(%)，精度 1%
 *   表按电压从高到低排列（4200mV=100% … 3200mV=0%）；
 *   相邻两点之间做线性插值，所以步长不一定是 10mV 也可以。
 *========================================================================*/
/* 【电压列的量程说明】本表第一列是 **电池电压**（3200~4200mV），
 * 与电池厂给的 OCV 曲线同一量程，跟具体分压电阻无关。
 *
 * 换算链（battery.h 里配置）：
 *     pin_mv = raw * BAT_ADC_VREF_MV / BAT_ADC_FULL_SCALE      （ADC 引脚电压）
 *     mv     = pin_mv * BAT_VBAT_MUL_NUM / BAT_VBAT_MUL_DEN    （还原成电池电压）
 *     mv     = mv * BAT_CAL_GAIN_NUM / BAT_CAL_GAIN_DEN + OFFSET （ADC 刻度标定）
 *     %      = BAT_MvToPercent(mv)   <- 查的就是本表
 *
 * 换分压电阻时**只改 BAT_VBAT_MUL_NUM/DEN 一行**，本表不用动：
 *     100k + 100k（1/2）  -> 2/1
 *     100k + 91k （0.4764）-> 2099/1000（= 191/91）
 * 2026-10-12 注：这张表曾按"引脚电压"量程整体减半过（当时在追 100%
 * 的显示问题），后来确认根因是 ADC 实际刻度偏高 19.6%（已由
 * BAT_CAL_GAIN 标定收掉），所以表已还原成电池电压量程。 */
const uint16_t battery_ocv_table[][2] = {
    {4200, 100},
    {4190,  99},
    {4180,  98},
    {4170,  97},
    {4160,  96},
    {4150,  95},
    {4140,  94},
    {4130,  93},
    {4120,  92},
    {4110,  91},
    {4100,  90},
    {4090,  89},
    {4080,  88},
    {4070,  87},
    {4060,  86},
    {4050,  85},
    {4040,  84},
    {4030,  83},
    {4020,  82},
    {4010,  81},
    {4000,  80},
    {3990,  79},
    {3980,  78},
    {3970,  77},
    {3960,  76},
    {3950,  75},
    {3940,  74},
    {3930,  73},
    {3920,  72},
    {3910,  71},
    {3900,  70},
    {3890,  69},
    {3880,  68},
    {3870,  67},
    {3860,  66},
    {3850,  65},
    {3840,  64},
    {3830,  63},
    {3820,  62},
    {3810,  61},
    {3800,  60},
    {3790,  59},
    {3780,  58},
    {3770,  57},
    {3760,  56},
    {3750,  55},
    {3740,  54},
    {3730,  53},
    {3720,  52},
    {3710,  51},
    {3700,  50},
    {3690,  49},
    {3680,  48},
    {3670,  47},
    {3660,  46},
    {3650,  45},
    {3640,  44},
    {3630,  43},
    {3620,  42},
    {3610,  41},
    {3600,  40},
    {3590,  39},
    {3580,  38},
    {3570,  37},
    {3560,  36},
    {3550,  35},
    {3540,  34},
    {3530,  33},
    {3520,  32},
    {3510,  31},
    {3500,  30},
    {3490,  29},
    {3480,  28},
    {3470,  27},
    {3460,  26},
    {3450,  25},
    {3440,  24},
    {3430,  23},
    {3420,  22},
    {3410,  21},
    {3400,  20},
    {3390,  19},
    {3380,  18},
    {3370,  17},
    {3360,  16},
    {3350,  15},
    {3340,  14},
    {3330,  13},
    {3320,  12},
    {3310,  11},
    {3300,  10},
    {3290,   9},
    {3280,   8},
    {3270,   7},
    {3260,   6},
    {3250,   5},
    {3240,   4},
    {3230,   3},
    {3220,   2},
    {3210,   1},
    {3200,   0}
};

const uint8_t battery_ocv_table_len =
    (uint8_t)(sizeof(battery_ocv_table) / sizeof(battery_ocv_table[0]));

/*==========================================================================
 * 内部状态
 *========================================================================*/
static int16_t  s_calib;            /* 实际参与换算的 ADC 校准值 */
static int16_t  s_calib_raw;        /* SDK 粗调函数返回的原值（仅调试查看） */
static uint16_t s_last_raw;         /* 最近一次 ADC 原始值（校准后参与换算） */
static uint16_t s_last_mv;          /* 最近一次电池电压 mV */
static uint8_t  s_last_percent;     /* 最近一次电量 % */

/*==========================================================================
 * 引脚底层
 *========================================================================*/
static void bat_en_pin_init(void)
{
    if (BAT_EN_PORTB) { GPIOB_ModeCfg(BAT_EN_PIN, GPIO_ModeOut_PP_5mA); }
    else              { GPIOA_ModeCfg(BAT_EN_PIN, GPIO_ModeOut_PP_5mA); }
}

static void bat_adc_pin_init(void)
{
    /* ADC 输入脚要配成"浮空输入"（手册：AINx -> Floating input） */
    if (BAT_ADC_PORTB) { GPIOB_ModeCfg(BAT_ADC_PIN, GPIO_ModeIN_Floating); }
    else               { GPIOA_ModeCfg(BAT_ADC_PIN, GPIO_ModeIN_Floating); }
}

void BAT_Enable(uint8_t on)
{
    if (BAT_EN_PORTB)
    {
        if (on) { GPIOB_SetBits(BAT_EN_PIN); } else { GPIOB_ResetBits(BAT_EN_PIN); }
    }
    else
    {
        if (on) { GPIOA_SetBits(BAT_EN_PIN); } else { GPIOA_ResetBits(BAT_EN_PIN); }
    }
}

/*==========================================================================
 * 初始化
 *========================================================================*/
void BAT_Init(void)
{
    /* VBAT_EN 用的是 PB14（仿真调试口），必须先关掉调试功能才能当普通 IO 用。
     * 关掉后 WCH-Link 可能无法再直接下载，需要用 ISP 方式恢复！ */
    if ((BAT_EN_PORTB == 1) && ((BAT_EN_PIN == GPIO_Pin_14) || (BAT_EN_PIN == GPIO_Pin_15)))
    {
        R16_PIN_ALTERNATE |= RB_DEBUG_EN;
    }

    /* 1) 使能脚：推挽输出，默认低电平（关断分压器） */
    bat_en_pin_init();
    BAT_Enable(0);

#if (BAT_EN_LEAVE_ON != 0)
    /* 标定电压用：开机就拉高 VBAT_EN(PB14) 让分压器常导通，
     * 方便万用表直接量 PA7。正常固件 BAT_EN_LEAVE_ON = 0，不走这里。 */
    BAT_Enable(1);
#endif

    /* 2) ADC 输入脚：浮空输入 */
    bat_adc_pin_init();

    /* 3) ADC 初始化：外部单通道采样 + 3.2M 采样率 + 1 倍增益（0dB） */
    ADC_ExtSingleChSampInit(SampleFreq_3_2, ADC_PGA_0);
    ADC_ChannelCfg(BAT_ADC_CHANNEL);

    /* 4) 取一次粗调校准值（必须在配置完 ADC 之后调用）。
     *    BAT_ADC_USE_ROUGH_CAL = 0 时不参与换算，只留一份在 s_calib_raw
     *    里供 BAT_GetCalibRaw() 查看 —— 本机实测这个返回值不可信，
     *    见 battery.h 的实测记录。 */
    s_calib_raw = ADC_DataCalib_Rough();

#if (BAT_ADC_USE_ROUGH_CAL != 0)
    s_calib = s_calib_raw;
#else
    s_calib = 0;
#endif

    /* 5) 清空缓存，先测一次（可选：想上电就出结果就调用 BAT_ReadMv()） */
    s_last_raw     = 0u;
    s_last_mv      = 0u;
    s_last_percent = 0u;
}

/*==========================================================================
 * 读一次 ADC（平均 BAT_ADC_SAMPLES 次），返回校准后的原始值
 *========================================================================*/
static uint16_t bat_read_adc(void)
{
    uint32_t sum = 0u;
    uint8_t  i;
    int32_t  v;
    uint32_t intlev;

    /* 【2026-10-14 卡死修复】整段转换必须原子。
     *
     * 这个 ADC 是共享的：BLE 协议栈把温感采样回调挂了进来
     *     bluetooth/MCU.c:172   cfg.tsCB = HAL_GetInterTempValue;
     * 而 HAL_GetInterTempValue() 调 ADC_InterTSSampInit() 重配 ADC，
     * 只备份/恢复 R8_TEM_SENSOR / R8_ADC_CHANNEL / R8_ADC_CFG，
     * **不碰 R8_ADC_CONVERT**。只要它插在"置 START"和"等 START 清零"
     * 之间，正在进行的这次转换就被破坏，START 位可能再也不清零 ——
     * 库里那句 while 会把整机卡死（屏幕定格、不出现 HARDFAULT）。
     *
     * BAT_ADC_SAMPLES(8) 次转换总共只有几百微秒，关中断远小于
     * BLE 的连接间隔（7.5ms），不会影响协议栈时序。 */
    intlev = __risc_v_disable_irq();
    for (i = 0u; i < BAT_ADC_SAMPLES; i++)
    {
        sum += ADC_ExcutSingleConver();      /* 阻塞式单次转换 */
    }
    __risc_v_enable_irq(intlev);
    sum = (sum + (BAT_ADC_SAMPLES / 2u)) / BAT_ADC_SAMPLES;

    /* 加上粗调校准值 */
    v = (int32_t)sum + (int32_t)s_calib;
    if (v < 0)
    {
        v = 0;
    }
    if (v > (int32_t)BAT_ADC_FULL_SCALE - 1)
    {
        v = (int32_t)BAT_ADC_FULL_SCALE - 1;
    }

    s_last_raw = (uint16_t)v;
    return s_last_raw;
}

/*==========================================================================
 * 【核心】读一次电池电压（mV）
 *   完整流程：使能 -> 等 500ms -> 采样 -> 关断 -> 换算
 *========================================================================*/
/* 先声明：换算是从 BAT_ReadMv() 里抽出来的，定义在后面 */
static uint16_t bat_finish_from_raw(void);

uint16_t BAT_ReadMv(void)
{
    /* 1) 打开分压器使能 */
    BAT_Enable(1);

    /* 2) 等分压网络和 ADC 输入电容稳定（你要求 0.5 秒） */
    DelayMs(BAT_EN_SETTLE_MS);

    /* 3) 采样（此时分压器是导通的） */
    (void)bat_read_adc();

    /* 4) 关断使能，省电 */
#if (BAT_EN_LEAVE_ON == 0)
    BAT_Enable(0);
#endif

    /* 5~7) 原始值 -> mV -> 电量 */
    return bat_finish_from_raw();
}

/*--------------------------------------------------------------------------
 * 原始值 -> 电池电压 mV / 电量 %（BAT_ReadMv 的第 5~7 步）
 * 单独抽出来是为了让"非阻塞采样"能复用同一套换算，避免两份公式走偏。
 *------------------------------------------------------------------------*/
static uint16_t bat_finish_from_raw(void)
{
    uint16_t pin_mv;
    uint32_t mv;
    int32_t  tmp;

    /* 5) 原始值 -> ADC 引脚电压 mV
     *    四舍五入：三步除法都截断的话整条链会固定偏低 1~2mV，
     *    正好卡在表格行上时就差 1% 档（4.20V 显示 99%、3.80V 显示 59%）。 */
    pin_mv = (uint16_t)((((uint32_t)s_last_raw * BAT_ADC_VREF_MV) +
                         (BAT_ADC_FULL_SCALE / 2u)) / BAT_ADC_FULL_SCALE);

    /* 6) 引脚电压 -> 电池电压 mV（乘分压比，同样四舍五入） */
    mv = ((((uint32_t)pin_mv * BAT_VBAT_MUL_NUM) + (BAT_VBAT_MUL_DEN / 2u))
          / BAT_VBAT_MUL_DEN);

    /* 7) 线性校准（四舍五入） */
    tmp = (int32_t)(((mv * BAT_CAL_GAIN_NUM) + (BAT_CAL_GAIN_DEN / 2)) / BAT_CAL_GAIN_DEN)
          + BAT_CAL_OFFSET_MV;
    if (tmp < 0)
    {
        tmp = 0;
    }
    if (tmp > 65535)
    {
        tmp = 65535;
    }

    s_last_mv      = (uint16_t)tmp;
    s_last_percent = (uint8_t)BAT_MvToPercent(s_last_mv);

    return s_last_mv;
}


/*==========================================================================
 * 【你要求的接口】读一次电量（%）
 *========================================================================*/
uint8_t BAT_ReadPercent(void)
{
    (void)BAT_ReadMv();         /* 内部已经把电量算好并缓存 */
    return s_last_percent;
}

/*==========================================================================
 * 【非阻塞采样】给"运行中定时刷新电量"用，见 battery.h 的说明
 *========================================================================*/
static uint8_t  s_bs_state;         /* 0 = 空闲，1 = 已使能、等稳定 */
static uint8_t  s_bs_first;         /* 1 = 还没拿到起始时刻 */
static uint32_t s_bs_t0;            /* 使能时刻（毫秒，调用方给的） */

void BAT_SampleStart(void)
{
    if (s_bs_state != 0u)
    {
        return;                     /* 已经在采了，别重复开 */
    }

    BAT_Enable(1);                  /* 打开分压器 */
    s_bs_state = 1u;
    s_bs_first = 1u;
}

bool BAT_SamplePoll(uint32_t now_ms)
{
    if (s_bs_state == 0u)
    {
        return false;               /* 没在采 */
    }

    if (s_bs_first != 0u)
    {
        s_bs_first = 0u;            /* 第一次调用只用来记起始时刻 */
        s_bs_t0    = now_ms;
        return false;
    }

    if ((uint32_t)(now_ms - s_bs_t0) < (uint32_t)BAT_EN_SETTLE_MS)
    {
        return false;               /* 还没稳定，下圈再看 */
    }

    (void)bat_read_adc();           /* 采样（分压器还开着） */

#if (BAT_EN_LEAVE_ON == 0)
    BAT_Enable(0);                  /* 关断，省电 */
#endif

    (void)bat_finish_from_raw();    /* 换算并缓存 */
    s_bs_state = 0u;

    return true;                    /* 这一次采完了 */
}

/*==========================================================================
 * OCV 表查电量（带线性插值）
 *   表是从高电压到低电压排列的：{4200,100} … {3200,0}
 *   高于最高点 -> 100%；低于最低点 -> 0%
 *========================================================================*/
uint16_t BAT_MvToPercent(uint16_t mv)
{
    uint8_t  i;
    uint16_t v_hi, v_lo;
    uint16_t p_hi, p_lo;

    /* 高于表头（满电） */
    if (mv >= battery_ocv_table[0][0])
    {
        return battery_ocv_table[0][1];
    }

    for (i = 0u; i + 1u < battery_ocv_table_len; i++)
    {
        v_hi = battery_ocv_table[i][0];
        p_hi = battery_ocv_table[i][1];
        v_lo = battery_ocv_table[i + 1u][0];
        p_lo = battery_ocv_table[i + 1u][1];

        if ((mv >= v_lo) && (mv < v_hi))
        {
            /* 线性插值：mv 落在 [v_lo, v_hi) 之间 */
            return (uint16_t)(p_lo + (uint16_t)(((uint32_t)(mv - v_lo) * (p_hi - p_lo)) /
                                                (v_hi - v_lo)));
        }
    }

    /* 低于表尾（空电） */
    return battery_ocv_table[battery_ocv_table_len - 1u][1];
}

/*==========================================================================
 * 缓存读取（不重新测量）
 *========================================================================*/
uint16_t BAT_GetLastMv(void)
{
    return s_last_mv;
}

uint8_t BAT_GetLastPercent(void)
{
    return s_last_percent;
}

uint16_t BAT_GetLastRaw(void)
{
    return s_last_raw;
}

int16_t BAT_GetCalibRaw(void)
{
    return s_calib_raw;
}

/******************* (C) CH592X 电池检测 *****************/
