/**************************************************************************
 * Main.c —— CH592X 键盘整机主程序（UI 驱动 + 惰性无线）
 *
 * 架构：这是一个"页面机"。UI 层管页面栈和路由，游戏只是"游戏模式"
 *       底下的一个子页面。
 *
 * 主循环节奏（整个程序里只有 Game_Task() 里那一处延时）：
 *     TMR0 1ms 中断 -> ENC_Scan()   编码器必须在定时器里扫，
 *                                   否则快速转动会丢格
 *     主循环      -> Game_Task()    1ms 节拍：扫矩阵键盘 + 延时
 *                 -> UI_Task()     喂按键给 UI、按帧重画
 *
 *==========================================================================
 * 【无线惰性策略】—— 本文件最重要的设计
 *
 * 需求：USB / BLE / 2.4G 只在"键盘模式"和"遥控模式"下启用，
 *       其他模式（主页 / 游戏 / 设置 / 亮度 / 休眠 / 重置）一律不启用。
 *
 * 做法：不按"模式"写死，而是每帧从"当前页面栈顶的名字"算出一个
 *       radio_demand（见 radio_demand_of()），再和上一帧对比，
 *       只在**跳变沿**上做上下电。这样：
 *         * 从主页进键盘页 -> 上电；键盘页退回主页 -> 断电
 *         * 从键盘页进遥控页 -> 键盘无线让位、2.4G 换角色给遥控
 *         * 进游戏/设置页 -> 全部断电，射频 PowerDown
 *
 * 收益（CH592X 只有 26KB RAM / 448KB Flash）：
 *         * BLE 协议栈的 TMOS 不再常驻跑，省电
 *         * 没插线也不空转等 POLL，主循环更快
 *         * 射频只在真正需要时上电，降低静态电流
 *
 * 惰性实现层次：
 *         [L1] 射频时钟：HALRF_PowerDown()/PowerUp() 控制 2.4G 模拟前端
 *         [L2] USB 设备：UsbKbd_DeviceInit() 上电，DeviceShutdown() 断电
 *         [L3] BLE 广播：BT_WCH_StartAdvertising() 开，Stop 关
 *         [L4] 主循环：不满足条件的分支整块跳过（不空转、不白等）
 *========================================================================*/

#include <string.h>

#include "CH59x_common.h"

#include "tft_port.h"
#include "tft_st7789.h"
#include "tft_gfx.h"
#include "key_matrix.h"
#include "encoder.h"
#include "battery.h"
#include "rgb_led.h"

#include "game_port.h"
#include "ui_page.h"
#include "ui_kbd_ch.h"
#include "ui_kbd.h"          /* UiKbd_Transport()：诊断用 */
#include "macro.h"          /* 键盘宏（一个键触发一串按键） */
#include "usb_kbd.h"
#include "bt_wch.h"
#include "bt_gatt.h"
#include "hal_rf.h"
#include "ui_pages2.h"
#include "rf_cfg.h"
#include "proto_rc.h"
#include "rc_table.h"
#include "game_mem.h"
#include "game_state.h"      /* Game_PoolYield()：与遥控主机共用同一块堆 */
#include "ui_widget.h"
#include "ui_store.h"

/* ---- 在线改按键映射（网页直连用）---- */
#include "kmap_store.h"
#include "usb_cdc.h"

/*==========================================================================
 * 参数
 *========================================================================*/
/* 屏的物理尺寸（竖屏原生）。ST7789_Init() 必须吃这一对，
 * 它决定显存偏移 _colstart/_rowstart，传错画面会跑到屏外。 */
#define LCD_PANEL_W         135
#define LCD_PANEL_H         240
/* 横屏后的逻辑尺寸（由 Game_SetOrientation(GAME_LANDSCAPE) 生效） */
#define LCD_W               240
#define LCD_H               135

#define COL_BOOT_BG         0x0000u

/* 开机屏自检：红->绿->蓝->白 各 250ms（在 Game_Init() 之后执行）。
 * 用来一眼判断显示链路是否正常：
 *   能看到四色 = 屏 + SPI + 背光都 OK，问题在 UI 代码；
 *   看不到     = SPI / 面板 / 接线问题。
 * 确认屏幕正常后改成 0 即可关掉（这段是原来被删掉的自检）。 */
#define LCD_BOOT_SELFTEST   0

/* 开机打点：在屏底打一行 ASCII 小字，记下最后跑到哪一步。
 * 会被 UI 的第一帧盖掉；如果这行字一直留在屏上，说明后面卡住了，
 * 字里写的就是最后成功执行的那一步。定位完改成 0 即可。 */
#define LCD_BOOT_TRACE      0

/*==========================================================================
 * 系统 1ms 时基 + 编码器采样
 *========================================================================*/
volatile uint32_t g_ms_tick;

/* ===== 【2026-10-14 卡死定位】主循环阶段码 + 中断看门狗 =====================
 * 主循环每圈把正在做的阶段写进 g_loop_stage；TMR0 1ms 中断发现 g_loop_beat
 * 超过 500ms 没翻圈，就判定卡死，把当时的阶段码/蓝牙步号冻存到 g_hang_*，
 * 并在屏底画一条红底 HANG 横条。
 *
 * 这样现场症状可以三选一，直接指方向：
 *   红底 HARDFAULT 事故屏     -> 硬故障（中断已关，看门狗不会执行）
 *   屏底红条 HANG s=.. b=..   -> 普通死循环（中断还活着，看阶段码）
 *   屏底还是普通诊断行不动     -> ROM 里挂起（连 1ms 中断都停了）
 *
 * 阶段码表（g_loop_stage）：
 *   0  空闲/循环收尾        1  Game_Task（扫键盘）     2  栈高水位采样
 *   3  app_sleep_task       4  led_task               5  radio_task
 *   6  UsbKbd_DeviceTask    7  BT_WCH_Process         8  UiKbdCh_Task
 *   9  射频归属切换        10  UiKbdCh_PollRf24      11  Rc_HostTickMs
 *  12  Rc_HostSuperframe   13  UiW_BatTask          14  BtGatt_SetBatteryLevel
 *  15  UI_Task（画屏）     16  BT_WCH_DrawDiag       17  loop_diag_end
 *  20  SNV 读回           21  SNV 擦除(4KB)         22  SNV 写回
 * ======================================================================== */
volatile uint8_t  g_loop_stage = 0u;
volatile uint32_t g_loop_beat  = 0u;
volatile uint8_t  g_hang_stage = 0xFFu;
volatile uint8_t  g_hang_bt    = 0xFFu;
volatile uint8_t  g_hang_flag  = 0u;
volatile uint16_t g_temp_calls = 0u;   /* 【FW35】BLE 温感回调 (cfg.tsCB) 被调用次数 */
/* ===== FW36 卡死定位仪（前向声明；实现见文件末尾）=====================
 * 已确认：Startup/startup_CH592.S 把所有未实现的中断向量都指向同一条
 *   1:  j 1b                 (无限自跳)
 * 任何"没有真处理函数"的中断一旦被投递，CPU 就永久卡在那里：
 * 主循环不再推进、但 TMR0 1ms 中断仍能进 -> 屏底照样会画出红条。
 *
 * FW36 在 1ms 看门狗命中时量出四件事，直接指出卡在哪：
 *   g_stuck_pc   = mepc          主循环真正停住的那条指令地址(F=Flash / R=RAM)
 *   g_stuck_gisr = PFIC->GISR    当前正在服务的中断号
 *   g_stuck_pend = PFIC->IPR[0]>>16   外部中断 IRQ16..31 的挂起位
 *   g_stuck_enab = PFIC->ISR[0]>>16   外部中断 IRQ16..31 的使能位
 * 另外给所有原本没有处理函数的中断补上计数用的真处理函数（见文件末尾）。 */
#define HANG_GISR   (*(volatile uint32_t *)0xE000E04Cu)   /* PFIC->GISR    */
#define HANG_IPR0   (*(volatile uint32_t *)0xE000E020u)   /* PFIC->IPR[0]  */
#define HANG_ISR0   (*(volatile uint32_t *)0xE000E000u)   /* PFIC->ISR[0]  */

extern volatile uint32_t g_stuck_pc;
extern volatile uint32_t g_stuck_pc2;
extern volatile uint32_t g_stuck_gisr;
extern volatile uint32_t g_stuck_pend;
extern volatile uint32_t g_stuck_enab;
extern volatile uint32_t g_irq_total;
extern volatile uint32_t g_irq_cnt[20];
/* FW38：UI 耗时计（红条第三行）+ 红条防重入标志 */
extern volatile uint32_t g_ui_ms_max;
extern volatile uint32_t g_draw_ms_max;
extern volatile uint8_t  g_hang_bar;
extern volatile uint8_t  g_tft_busy;   /* 定义在 hardware/tft_st7789.c */

static uint32_t hang_mepc(void);
static void     hang_show2(uint8_t stg, uint8_t bt);


static char *hang_u16(char *p, uint16_t v)
{
    char    t[5];
    uint8_t n = 0u;

    if (v == 0u) { *p = 0x30; return p + 1; }
    while ((v != 0u) && (n < 5u)) { t[n] = (char)(0x30 + (v % 10u)); n++; v = (uint16_t)(v / 10u); }
    while (n != 0u) { n--; *p = t[n]; p++; }
    return p;
}

/* 【2026-10-18 清理】原 hang_show() 已被 hang_show2() 完全取代：
 * 它画的是 FW35 的单行 "H35 s=.. b=.. t=.."，而 TMR0 看门狗现在
 * 调用的是三行的 hang_show2()。保留未调用的版本只会白占 Flash，
 * 还会触发 -Wunused-function 告警，所以整段删掉。 */

/* 注意：中断处理函数**必须**带 __INTERRUPT（也就是
 * __attribute__((interrupt("WCH-Interrupt-fast")))），
 * 否则编译器用 ret 而不是 mret 返回 —— RISC-V 上中断要从 mepc 恢复，
 * 用 ret 会跳回 ra，直接把控制流打飞（现象：跑一会儿就死机/画面冻住）。
 *
 * 之前这里漏了这个属性，导致 1ms 时基 g_ms_tick 实际上没能正常工作，
 * UI 的帧率门（每 20ms 才画一帧）永远不通过 -> 画面一直不出来。
 *
 * __HIGH_CODE 放 RAM 里跑，和工程里 RTC/USB 的中断保持一致的写法。 */
__INTERRUPT
__HIGH_CODE
void TMR0_IRQHandler(void)
{
    if (TMR0_GetITFlag(TMR0_3_IT_CYC_END) != 0u)
    {
        TMR0_ClearITFlag(TMR0_3_IT_CYC_END);
        ENC_Scan();                     /* 编码器只在 1ms 中断里扫 */
        g_ms_tick++;

        /* 【卡死看门狗】主循环 500ms 没翻圈就画一条红底 HANG 横条。
         * 只在第一次越过阈值时画一次，之后不再重复。 */
        {
            static uint32_t s_beat_last = 0u;
            static uint16_t s_stall     = 0u;

            if (g_loop_beat != s_beat_last)
            {
                s_beat_last = g_loop_beat;
                s_stall     = 0u;
                g_hang_flag = 0u;
            }
            else if (s_stall < 60000u)
            {
                s_stall++;
                if ((s_stall > 500u) && ((s_stall % 1000u) == 0u))
                {
                    g_stuck_pc2 = hang_mepc();      /* 再采一次，确认是同一个死循环 */
                }
                if (s_stall == 500u)
                {
                    g_stuck_pc   = hang_mepc();
                    g_stuck_gisr = HANG_GISR;
                    g_stuck_pend = (HANG_IPR0 >> 16) & 0xFFFFu;
                    g_stuck_enab = (HANG_ISR0 >> 16) & 0xFFFFu;
                    g_hang_stage = g_loop_stage;
                    g_hang_bt    = g_bt_diag_step;
                    g_hang_flag  = 1u;
                }
                /* FW38：红条必须等 SPI 总线空闲再画。
                 * 原代码在中断里直接抢总线，而主线程被打断时很可能正卡在
                 * SPI0_MasterTrans() 里（那时 mepc 就是 0x143DA 附近）——
                 * 中断里那一笔嵌套传输会让主线程永远出不来：本来是"一次慢重画"，
                 * 被诊断代码变成了"真死机"。
                 * 3 秒且总线空闲才画；5 秒兜底（真的死在 SPI 事务里、总线永不空闲
                 * 时也该把 PC 显示出来 —— 那时反正已经死了）。*/
                if ((g_hang_flag != 0u) && (g_hang_bar == 0u) &&
                    (((s_stall >= 3000u) && (g_tft_busy == 0u)) || (s_stall >= 5000u)))
                {
                    g_hang_bar = 1u;
                    hang_show2(g_hang_stage, g_hang_bt);
                }
            }
        }
    }
}

#if (LCD_BOOT_TRACE)
/* 开机打点：横屏坐标，画在屏底一行。
 * 注意别照抄 bt_wch.c 的 bt_mark —— 它用竖屏 y=200，
 * 横屏下 _height 只有 135，那个坐标会被裁剪掉，什么也看不到。 */
static void boot_trace(const char *s)
{
    TFT_FillRect(0, (int16_t)(LCD_H - 13), (int16_t)LCD_W, 13, 0x0000u);
    TFT_SetTextSize(1);
    TFT_SetTextColorBg(0x07E0u, 0x0000u);
    TFT_SetCursor(2, (int16_t)(LCD_H - 12));
    TFT_Print(s);
}
#define BOOT_TRACE(s)   boot_trace(s)
#else
#define BOOT_TRACE(s)   ((void)0)
#endif

/*==========================================================================
 * USB_DIAG_NO_RADIO —— 【临时诊断开关】
 *   = 1：只跑 USB，完全不碰 BLE / 2.4G / 射频，也不跑灯效和休眠。
 *        用来二分"USB 本身"和"射频 + 负载环境"。做完记得设回 0。
 *   = 0：正常固件。
 *========================================================================*/
#ifndef USB_DIAG_NO_RADIO
#define USB_DIAG_NO_RADIO   0
#endif

/*==========================================================================
 * USB_DIAG_BARE —— 【诊断开关】把变量压到只剩"USB 实现代码"一个
 *   = 1：main() 里只做 SetSysClock + UsbKbd_DeviceInit()，
 *        然后 while(1) 只跑 UsbKbd_DeviceTask()。
 *        不点屏、不扫矩阵、不读电量、不起任何其它中断、不碰射频。
 *        配合 USBKBD_ENABLE_CDC=0（只有一个 HID 接口、两个端点），
 *        就和 WCH 那个在你板子上验证过能跑的 HID_CompliantDev 同形。
 *        判读：能枚举 -> 病在我们的 App 初始化/运行环境；
 *              不能枚举 -> 病在 usb_kbd.c / usb_kbd_port_ch592.c 本身。
 *   = 0：正常固件。用完必须设回 0。
 *========================================================================*/
/*==========================================================================
 * USB_DIAG_NO_RF24 —— 【临时诊断】不扫 2.4G，射频始终留给 BLE
 *   = 1 打开（正式固件必须为 0）
 *   用途：2.4G 收包会去重配射频，可能把 BLE 广播搅掉。打开这个开关后
 *   主循环完全不动 2.4G，如果蓝牙就能搜到/连上，说明问题出在射频切换；
 *   如果照样搜不到，那 BLE 这条链路本身还有别的问题。
 *========================================================================*/
#ifndef USB_DIAG_NO_RF24
/*==========================================================================
 * 【2026-10-14 隔离测试档】把 2.4G 从方程里彻底拿掉
 *
 * 置 1 时：主循环里强制 HALRF_BtOwnsPhy(true)，射频【全程归 BLE】，
 *          2.4G 键盘从机一次都不碰射频（见主循环里 USB_DIAG_NO_RF24 分支）。
 *
 * 用途：现场现象是"连上(23) 立刻断(6) 来回跳"。既然 2.4G 那边的
 *       HALRF_Tx/RxStart/Shut/PowerDown 都有 s_bt_owns_phy 守卫，
 *       理论上动不了射频；但"理论"要验证 —— 置 1 烧一次就知道：
 *         连上就稳了   -> 凶手在 2.4G 仲裁/交接路径上
 *         还是来回跳   -> 与 2.4G 无关，往 BLE 协议栈/hiddev 那边查
 *
 * 测完改回 0。 */
#define USB_DIAG_NO_RF24   0
#endif

#ifndef USB_DIAG_BARE
#define USB_DIAG_BARE   0
#endif

/*==========================================================================
 * LOOP_DIAG —— 【临时诊断】把"主循环频率 + 最坏一圈耗时"画在屏幕底部
 *   = 1 打开（正式固件要设回 0）
 *   用途：UI 卡、按键采样率低时，先量出这两个数：
 *     Hz  = 主循环每秒转多少圈（矩阵键盘就是每秒扫多少次）
 *     W   = 最近一秒里最慢那一圈花了多少毫秒（刷屏/写Flash/RF 的停顿）
 *   正常参考：W 应该就是 UI 刷屏那一圈（十几毫秒），Hz 应该在几十以上。
 *   如果 Hz 掉到个位数 -> 有东西在长时间阻塞主循环，再按 W 定位。
 *========================================================================*/
#ifndef LOOP_DIAG
/* 【2026-10-15】屏底那两行诊断（含黑色底带）默认关掉了：日常使用不需要，
 * 而且它占着屏幕最下面 21 像素。要排查时把下面的 LOOP_DIAG 改回 1 重新编译
 * 即可 —— 代码一行都没删。
 *
 * 【2026-10-14 诊断档】打开主循环 Hz / 最坏圈耗时显示。
 *
 * 为什么要量这个：BLE 连接间隔只有 7.5~11.25ms，协议栈靠主循环里的
 * BT_WCH_Process() 喂。**只要主循环有一圈超过约 10ms，就会错过一次
 * 连接事件**；连续错过到监督超时（3 秒）就断链 —— 现场正是
 * "连上(23) 立刻断(6)"反复循环。
 *
 * 判读：
 *   Hz 应有几十以上、W 应只有十几毫秒（UI 刷屏那一圈）
 *   Hz 掉到个位数 / W 几十上百毫秒  => 有东西在长时间阻塞主循环，
 *       那就是蓝牙断链的根因，再按 W 去定位是谁（刷屏？写 Flash？RF？）
 *
 * 排完改回 0。 */
#define LOOP_DIAG   0
#endif
/*   USB_DIAG_BARE 分三档：
 *     1 = USB 最先上电，应用初始化一概不跑，主循环只有 UsbKbd_DeviceTask()
 *     2 = 应用初始化全部跑完，然后才 USB 上电，主循环只有 UsbKbd_DeviceTask()
 *   1 和 2 一比，就知道是"应用初始化"还是"主循环任务"把 USB 弄坏的。 */

static void enc_timer_init(void)
{
    TMR0_TimerInit((uint32_t)(GetSysClock() / 1000u));   /* 1ms */
    TMR0_ClearITFlag(TMR0_3_IT_CYC_END);
    TMR0_ITCfg(ENABLE, TMR0_3_IT_CYC_END);
    PFIC_EnableIRQ(TMR0_IRQn);
}

/*==========================================================================
 * 无线需求判定
 *
 * 页面名（ui_page_t.name）是 UI 层给我们的唯一稳定标识：
 *   "home"   主页面            -> 无
 *   "kbd"    键盘模式          -> USB + BLE + 2.4G 键盘从机
 *   "kbset"  键盘设置子页      -> 同上（还是键盘模式里）
 *   "keyedit" 键值编辑子页     -> 同上
 *   "game"   游戏模式          -> 无
 *   "rc"     遥控模式          -> 2.4G 主机（键盘让位）
 *   "setting"/"bright"/"sleep"/"reset" -> 无
 *
 * 注意：这里**只按名字判断**，不看页面结构体指针 —— 指针比较在
 * 不同编译单元里跨模块不可靠，而且名称比较对以后加页面更宽容。
 *========================================================================*/

typedef enum
{
    RADIO_NONE = 0,     /* 全部断电 */
    RADIO_KBD,          /* 键盘：USB 设备 + BLE 广播 + 2.4G 从机 */
    RADIO_RC            /* 遥控：2.4G 主机 */
} radio_demand_t;

static bool page_name_is(const ui_page_t *p, const char *name)
{
    return (p != 0) && (p->name != 0) && (strcmp(p->name, name) == 0);
}

static radio_demand_t radio_demand_of(const ui_page_t *cur)
{
    if (page_name_is(cur, "kbd")     ||
        page_name_is(cur, "kbset")   ||
        page_name_is(cur, "keyedit"))
    {
        return RADIO_KBD;
    }

    if (page_name_is(cur, "rc"))
    {
        return RADIO_RC;
    }

    /* home / game / setting / bright / sleep / reset 及以后新增页
     * 一律归于"无无线"—— 这是需求要的"其他模式不启用"。 */
    return RADIO_NONE;
}

/*==========================================================================
 * 遥控模式（文档 (四)）需要的主机侧状态
 *
 * 【内存账，改这里之前先看这段】
 * 全工程只有两处动态分配，而且共用同一块堆：
 *     * 遥控主机 rc_host_t  —— 进遥控页才申请，离开就还
 *     * 游戏状态池           —— 进游戏才申请，退出就还
 * 两者**不会同时存在**，所以堆只要装得下较大的那个就行。
 *
 * 堆的容量由链接脚本决定：RAM 顶端 - __stack_size - _ebss（见 Ld/Link.ld）。
 * 改 __stack_size、或往 .bss 里加东西，都会直接压缩这块堆 ——
 * 下面的编译期护栏与运行时提示就是防止"悄悄改坏、只有上机才发现"。
 *========================================================================*/
static rc_host_t *s_rch;                  /* 0 = 没进遥控模式，不占 RAM */

/* 链接脚本提供的堆边界（只取地址，不访问内容） */
extern uint8_t _ebss;
extern uint8_t _heap_end;

/*********************************************************************
 * @fn      heap_bytes
 * @brief   当前堆的可用容量（字节）
 * @note    值由链接结果决定，用来在申请失败时给出可诊断的信息。
 *********************************************************************/
static uint32_t heap_bytes(void)
{
    return (uint32_t)((uintptr_t)&_heap_end - (uintptr_t)&_ebss);
}

/*==========================================================================
 * 栈高水位实测（【P2 修复】新增）
 *
 * 为什么需要它：本工程吃过 -fstack-usage 的大亏。
 *   -fstack-usage 只统计**函数自己**的帧，不认识函数指针调用，
 *   也不统计中断入口保存的上下文。实测它把 main 的真实最深链路
 *   （688 B）报成 112 B，差点让人以为 640 B 栈很安全。
 *   唯一的可靠办法是**运行时刷 magic + 找低水位**。
 *
 * 原理：
 *   1) 开机把整段可用栈刷成 0xA5（从栈底 _susrstack 刷到当前 sp，
 *      不动自己的栈帧，返回时不会踩坏）。
 *   2) 之后任何更深的调用都会盖掉这些字节。
 *   3) 周期性从 _susrstack 往上找第一个非 0xA5 的字节，
 *      就是历史最深点；g_stack_hw = _eusrstack - 该地址。
 *
 * 判读：
 *   g_stack_hw 接近 __stack_size（1024）说明快踩穿了，继续加大或砍调用链；
 *   g_stack_hw == __stack_size 说明已经踩到堆了（堆数据可能已被写坏）。
 *   HardFault 画面也会显示它（标签 SK）。
 *========================================================================*/
extern uint8_t _susrstack;      /* 栈底（低地址，堆的尽头）*/
extern uint8_t _eusrstack;      /* 栈顶（高地址，复位时的 sp）*/

#define STACK_PAINT_MAGIC   0xA5u

/* 历史最深用量（字节）。volatile：崩溃后调试器可直接 watch。 */
volatile uint32_t g_stack_hw   = 0u;
/* 本次开机是否已经刷过 magic（防止重复刷把上一次的高水位抹掉）*/
static uint8_t   s_stack_painted = 0u;

/* noinline：保证读到的 sp 是本函数自己的，语义稳定 */
__attribute__((noinline))
static void stack_paint(void)
{
    uint8_t *lo   = &_susrstack;
    uint8_t *stop = &_eusrstack;
    uint32_t sp;

    __asm volatile("mv %0, sp" : "=r"(sp));
    if ((uint8_t *)sp < stop)
    {
        stop = (uint8_t *)sp;         /* 只刷当前帧以下，别踩自己的栈 */
    }
    while (lo < stop)
    {
        *lo++ = STACK_PAINT_MAGIC;
    }
    s_stack_painted = 1u;
}

/* 返回自上次刷 magic 以来的最深用量（字节）。
 * 只读不写，可以随便调；返回 0 表示还没刷过 magic。 */
static uint32_t stack_probe(void)
{
    const uint8_t *lo = &_susrstack;
    const uint8_t *hi = &_eusrstack;
    const uint8_t *p  = lo;

    if (s_stack_painted == 0u)
    {
        return 0u;
    }
    while ((p < hi) && (*p == STACK_PAINT_MAGIC))
    {
        p++;
    }
    return (uint32_t)(hi - p);
}

/* 堆的真实容量（链接实测，以 obj/CH592X.map 的 _heap_end/_ebss 为准）：
 *     _ebss = 0x200058e8（RAM 优化前是 0x20005d60）
 *     RAM 顶端 0x20006800 - _ebss = 2720 B   ← 栈 + 堆共用
 *     【P2 修复】栈 640 -> 1024，于是堆 = _heap_end 0x20006400 - _ebss
 *                            = 1688 B（比 1696 少 8 B：本文件新增的
 *                            g_stack_hw + s_stack_painted 占了 .bss）
 * 配套改动（三者必须同步，见 Ld/Link.ld 顶部的账）：
 *     rf24/proto_rc.h: RC_HOST_MAX_DEV 6 -> 4
 *     rf24/proto_rc.h: RC_KEYMAP_RX_SLOTS 2 -> 1
 *     cmdq / last_rpt_ms **保持**按 RF_RC_MAX_DEV 定尺 —— 这两个数组是按
 *     协议 dev_id 直接索引的（dev_id 空间 1..6，从机侧一致），
 *     不能跟着 RC_HOST_MAX_DEV 缩，否则 dev_id=5/6 的从机一上报就越界。
 * 改后需求 = sizeof(rc_host_t) 1596 + 32 = 1628 B。
 * 改之前 6 设备 + 2 槽 = 2312 + 32 = 2344 B > 当时的堆，遥控模式本来就申请失败。
 *
 * 【2026-10-13 RAM 优化后更新】实测 _ebss = 0x200058e8，
 *     堆 = _heap_end 0x20006000 - _ebss = 1496 B（栈让到 2048 B 之后）。
 *   省下来的 1160 B 来自两组改动，详见《RAM占用分析与优化_2026-10-13.md》：
 *     第一批（.bss 瘦身 840 B）
 *       keyboard/kmap_store.c  768 -> 320 B（Load/Save 共用一块页缓冲 + 分块回读校验）
 *       keyboard/usb_cdc.h     TX 队列 8 -> 4 帧（568 -> 284 B）
 *       bluetooth/bt_port.h    BT_MAX_DEV 8 -> 4（200 -> 100 B）
 *     第二批（干掉 newlib stdio 320 B）
 *       hardware/tft_gfx.c     自带格式化器 + 顶上 printf/puts/putchar，
 *                              __sf(312B) 与 stdio 整条链不再进镜像
 *   现在 1628 B <= 2840 B，余 1212 B，遥控模式申请内存有充足余量。
 *
 * 编译期护栏：rc_host_t 一旦大到堆都装不下就直接编译不过，逼着先改
 * RC_HOST_MAX_DEV / RC_KEYMAP_RX_SLOTS 或从 .bss 里让 RAM，
 * 而不是上机了才发现遥控用不了。
 *   预算 2840 = 当前真实堆（_heap_end - _ebss）；
 *   判据里的 +32 是 malloc 块头/对齐的保守值。
 *   注意这个数会随 __stack_size 和 .bss 变化，改 Link.ld 后要同步回来。 */
#define MAIN_HEAP_BUDGET   1264u    /* 实测（FW39：栈 2048->1536 之后），随 Link.ld 变化 */
typedef char rc_host_must_fit_in_heap[((sizeof(rc_host_t) + 32u) <= MAIN_HEAP_BUDGET) ? 1 : -1];
static uint16_t  s_sf_ms;                 /* 毫秒计数，累到超帧就发一轮 */
static uint32_t  s_rc_last_ms;            /* 上次推进遥控协议栈的真实毫秒 */
static uint8_t   s_rc_synced;             /* 设备表是不是已经抄进界面了 */
static void rc_start(void)
{
    if (s_rch != 0)
    {
        return;                             /* 已经在遥控模式 */
    }

    /* 【P0 修复】先把游戏状态池还回去，再要遥控这块。
     * 两者共用同一块堆、同一时刻只能有一个所有者：若它还占着，
     * GameMem_Acquire() 会直接返回 0（连申请都不会尝试），
     * 表现就是"遥控页停在 0 设备、扫不到任何从机"。
     * 正常路径由 radio_task() 在离开游戏页时就会还清，这里是兜底。 */
    Game_PoolYield();

    s_rch = (rc_host_t *)GameMem_Acquire((uint16_t)sizeof(rc_host_t));
    if (s_rch == 0)
    {
        /* 【修复报告"修复 6"】原来这里是静默 return：屏幕只显示"0 设备"，
         * 完全看不出是内存不够。现在把原因写到遥控页标题上（UiRc_SetStatus），
         * 并留一个启动打点，方便插着调试口复现。 */
        UiRc_SetStatus("MEM LOW");
        BOOT_TRACE("Z rc-nomem");
        return;
    }

    UiRc_SetStatus(0);                      /* 申请成功：清掉上一次的错误提示 */

    /* 先从键盘手里把射频接过来（两者共用一颗射频） */
    UiKbdCh_SetRadioMode(false);

    Rc_HostInit(s_rch, 0x0Bu);              /* 重建主机设备表 */

    /* 【2026-10-19 修复】把本网地址真正装进射频。
     * 直接从上电主页进遥控页时，UiKbdCh_Init() 从未跑过，HALRF 的
     * rfStatusCB / RxMaxlen / TxMaxlen 都还是 0；而 Rc_HostInit() 只
     * 初始化软件结构，不碰 HAL。第一次 HALRF_Tx() 的回调就是空指针，
     * 会跑飞/触发看门狗。这里保证 HAL 已被 UiKbdCh_Init() 配置过，
     * 并把遥控数据地址先装好，后面的枚举/超帧只做安全的换频与换址。 */
    (void)HALRF_SetLink(s_rch->rc_addr, Hop_Current(&s_rch->hop));

    /* 表只在第一次进遥控页时载入。RcTable_Load() 在 Flash 两个 bank
     * 都无效时会重建默认表（擦写 2KB），不能每进一次页面就做一遍。 */
    if (RcTable_CurrentBank() == 0xFFu)
    {
        (void)RcTable_Load();
    }
    s_rc_synced  = 0u;
    s_sf_ms      = 0u;
    s_rc_last_ms = g_ms_tick;
    UiRc_SetDeviceCount(0u);
}

static void rc_stop(void)
{
    UiRc_SetDeviceCount(0u);
    UiRc_SetDeviceName(0u, 0);
    s_rc_synced = 0u;

    /* 射频地址先切回键盘本机的（哪怕下一步马上要断电，
     * 也要切，否则下次进键盘页地址还是遥控的）。 */
    UiKbdCh_SetRadioMode(true);

    if (s_rch != 0)
    {
        GameMem_Yield((void *)s_rch);       /* 还给堆 */
        s_rch = 0;
    }
}

/* 遥控功能页：把 CH592X 自己的 12 个操作变成**动作编号**发给选中的从机。
 *
 * 【12 个操作 -> key_id】9 按键 + 编码器 3 种
 *     key_id 1..9   = 3x3 矩阵键（UiRc_KeyState() 的位图）
 *     key_id 10     = 编码器左转
 *     key_id 11     = 编码器右转
 *     key_id 12     = 编码器单击
 *
 * 【单键规则】每次只传单键；多个同时按下视为无效，不发：
 *     位图恰好 1 位            -> 发这个键的动作编号
 *     位图 >= 2 位              -> 不发，并把"上次"清掉（回到单键能重新触发）
 *     位图 0 位 + 编码器有事件  -> 发编码器的动作编号
 *     位图 0 位 + 无事件        -> 发编号 0（全部松开）
 *
 * 【编号从哪来】RcTable_ActionOf() 查三张表：
 *     设备名 ->(③设备绑定表)-> 表名 ->(②动作编号表)-> key_num[key_id-1]
 *   为了不在每次按键都读 Flash，**选中设备时**用 RcTable_LoadActionsFor()
 *   一次把 12 个编号读进 12 字节缓存，之后按键直接索引缓存。
 *
 * 【从机侧】CH570Q 收到编号后查自己的 Flash 执行，CH592X 完全不关心
 *   它做什么动作 —— 两边不共享任何"动作定义"，所以不会出现
 *   "动作执行错了、界面却显示对的"这种最难查的错。
 */
static uint8_t s_rc_act[RC_OP_COUNT];   /* 12 B：当前选中设备的 12 个动作编号 */
static uint8_t s_rc_act_dev = 0xFFu;    /* 缓存属于哪个设备下标（0xFF = 无效） */
static uint8_t s_rc_act_ok;             /* 缓存是否有效（设备没绑表时为 0） */
static uint8_t s_rc_last_op;            /* 上一次已下发的 key_id，0 = 无 */

/* 9 位位图里置了几位 */
static uint8_t rc_popcount9(uint16_t v)
{
    uint8_t n = 0u;
    uint8_t i;

    for (i = 0u; i < 9u; i++)
    {
        if ((v & (uint16_t)(1u << i)) != 0u) { n++; }
    }
    return n;
}

static void rc_send_ui_keys(void)
{
    uint16_t       keys;
    uint8_t        n;
    uint8_t        key_id = 0u;
    uint8_t        idx;
    rc_host_dev_t *d;

    /* 不在功能页（还在设备列表 / 已退出）：清边沿 + 让缓存作废 */
    if ((s_rch == 0) || (!UiRc_InFunction()))
    {
        s_rc_last_op = 0u;
        s_rc_act_dev = 0xFFu;
        return;
    }

    /* 注意两套编号不是一回事：
     *   UiRc_SelectedDevice() 给的是**界面列表里的下标**（和 Rc_HostDev() 一致）；
     *   Rc_HostSendCmd() 要的是协议里的 dev_id（1..RF_RC_MAX_DEV）。 */
    idx = UiRc_SelectedDevice();
    if (idx == 0xFFu)
    {
        s_rc_last_op = 0u;
        s_rc_act_dev = 0xFFu;
        return;
    }

    d = Rc_HostDev(s_rch, idx);
    if (d == 0)
    {
        s_rc_last_op = 0u;
        s_rc_act_dev = 0xFFu;
        return;
    }

    /* ---- 换设备（或第一次进功能页）：重建 12 个动作编号的缓存 ---- */
    if (s_rc_act_dev != idx)
    {
        s_rc_act_dev = idx;
        s_rc_last_op = 0u;
        s_rc_act_ok  = (uint8_t)(RcTable_LoadActionsFor(d->dev_name, s_rc_act) == 0u);
    }

    /* ---- 定出"当前是哪一次操作" ---- */
    keys = UiRc_KeyState();
    n    = rc_popcount9(keys);

    if (n == 1u)
    {
        uint8_t i;

        for (i = 0u; i < 9u; i++)
        {
            if ((keys & (uint16_t)(1u << i)) != 0u) { key_id = (uint8_t)(i + 1u); break; }
        }
    }
    else if (n == 0u)
    {
        key_id = UiRc_TakePending();        /* 编码器事件；0 = 没有 */
    }
    else
    {
        /* 多键同时按下：无效、不发。把"上次"清掉，
         * 这样松开到只剩一个键时能重新触发一次。 */
        s_rc_last_op = 0u;
        (void)UiRc_TakePending();
        return;
    }

    if (key_id == s_rc_last_op)
    {
        return;                             /* 没有变化：不重发 */
    }
    s_rc_last_op = key_id;

    /* ---- 查动作编号并下发 ---- */
    {
        uint8_t aid = RC_ACT_ID_NONE;
        uint8_t param[2];

        if (key_id != 0u)
        {
            uint8_t k = RcTable_IdxOfKeyId(key_id);

            if (k == 0xFFu)      { return; }        /* key_id 越界 */
            if (s_rc_act_ok == 0u) { return; }      /* 这台设备没绑表：不发 */

            aid = s_rc_act[k];
            if (aid == RC_ACT_ID_NONE) { return; }  /* 这一次操作没配：不发 */
        }
        /* key_id == 0 -> aid = 0 -> 发"全部松开" */

        param[0] = aid;
        param[1] = 0u;
        (void)Rc_HostSendCmd(s_rch, d->dev_id, (uint8_t)RC_CMD_ACTION, param, 1u);
    }
}

static void rc_sync_devices(void)
{
    uint8_t n = (s_rch != 0) ? Rc_HostCount(s_rch) : 0u;
    uint8_t i;

    if (n > (uint8_t)UIRC_DEV_MAX) { n = (uint8_t)UIRC_DEV_MAX; }
    UiRc_SetDeviceCount(n);

    for (i = 0u; i < n; i++)
    {
        rc_host_dev_t *d = Rc_HostDev(s_rch, i);
        UiRc_SetDeviceName(i, (d != 0) ? d->dev_name : 0);
    }
}

/*==========================================================================
 * 惰性无线电管理器
 *
 * s_radio 记录**当前已经上电的**无线档位。每次主循环把"页面需要的"
 * 和它比一下，不一致才动。这样上下电各只发生一次，不会每帧都折腾。
 *========================================================================*/
static radio_demand_t s_radio = RADIO_NONE;
static uint8_t        s_rf_powered = 0u;    /* 2.4G 射频时钟是否上电 */
static uint8_t        s_bt_inited = 0u;     /* BLE 协议栈是否已初始化（只能初始化一次） */
static uint8_t        s_kbdch_inited = 0u;  /* 键盘通道是否已初始化 */

/* 让 2.4G 射频上电（已经在电上的话什么都不做） */
static void rf_power_on(void)
{
    if (s_rf_powered == 0u)
    {
        HALRF_PowerUp();
        s_rf_powered = 1u;
    }
}

/* 让 2.4G 射频断电（省静态电流） */
static void rf_power_off(void)
{
    if (s_rf_powered != 0u)
    {
        HALRF_Shut();           /* 先回 idle 再掉电，避免半途中断留状态 */
        HALRF_PowerDown();
        s_rf_powered = 0u;
    }
}

/* 2.4G HAL 最少初始化一次（只注册回调 / 配置长度和射频参数）。
 *
 * 键盘模式本来就每次进页都会调 UiKbdCh_Init()；但遥控模式可以**从主页
 * 直接进入**，此前从没碰过 HALRF_Init()。HALRF_PowerUp() 会拿一份全 0 的
 * s_cfg 去 RF_Config()，rfStatusCB 是空指针 —— 第一次发遥控 SYNC 的回调
 * 就会跳到地址 0，表现正是"打开遥控页就崩/看门狗重启"。
 *
 * UiKbdCh_Init() 内部只做软件接线 + HALRF_Init() + Kb_DevInit()，不会
 * 启动 USB/BLE，放在遥控入口是安全的；它读的是键盘配对地址，随后
 * rc_start() 会立刻把链路切成遥控主机地址。 */
static void kbdch_ensure_hw_init(void)
{
    if (s_kbdch_inited == 0u)
    {
        UiKbdCh_Init();
        s_kbdch_inited = 1u;
    }
}

/*********************************************************************
 * @fn      want_bt_phy
 * @brief   按通信仲裁的**阶段**判断这颗射频现在该归谁
 * @return  true = 归 BLE 协议栈；false = 交还 2.4G
 *
 * 【为什么必须看阶段，不能只看通道】
 * 仲裁在"扫描期"里 cur 一直是 UI_TR_NONE（还没锁定任何通道），
 * 只看 cur 会把 SCAN_BLE 阶段也判成 2.4G 的 —— 那时 BLE 正在广播等连接，
 * PHY 却还是 2.4G 参数，广播发不出去、主机连不上；
 * 而"连上以后切到 BLE 通道"这个跳变沿又永远等不到（连不上就没有 BLE 通道），
 * 于是死锁。所以按阶段判：
 *     SCAN_USB / SCAN_RF24  -> 正在探 2.4G 主机，PHY 归 2.4G（要收 POLL）
 *     其余（SCAN_BLE / 已锁定）-> 除非锁定在 RF24，否则一律归 BLE
 *********************************************************************/
static bool want_bt_phy(void)
{
    const ui_tr_t *tr = UiKbd_Transport();

    /* BLE 已连接时，射频必须**无条件**留给 BLE。
     *
     * CH592 只有一个射频前端，BLE / 2.4G 物理互斥。
     * 原判断只看 transport 的扫描阶段，一旦 BLE 已连接但阶段还停在
     * SCAN_RF24，PHY 就会被 2.4G 抢走：halrf_ch592_apply() -> RF_Config()
     * 会重写 accessAddress / 频点 / CRC / LLEMode / rfStatusCB，
     * 正在进行的连接事件被物理层摧毁 -> 蓝牙断链 + 后续射频行为不可预期
     * （用户实测：连接上后屏幕直接卡死、蓝牙断链）。
     *
     * 已建立的链路优先级高于任何扫描流程，不能被扫描打断。 */
    if (BT_WCH_IsConnected())
    {
        return true;
    }

    if (tr == 0)
    {
        return true;                        /* 拿不到仲裁状态：保守留给 BLE */
    }

    /* 【2026-10-14 修复：蓝牙无法稳定连接 / 重连要等 2 秒】
     *
     * 原来这里是：
     *     if ((phase == UI_TR_SCAN_USB) || (phase == UI_TR_SCAN_RF24)) return false;
     * 把 a 阶段也一起让给了 2.4G —— 可 **a 阶段是纯 USB 检测，根本不碰射频**
     * （见 ui_transport.c 的 SCAN_USB：只看 usb_plugged / usb_ready 两个软件标志）。
     * 白白把射频前端交出去 0.5 秒，HALRF_BtOwnsPhy(false) 会把 2.4G 的
     * accessAddress / 频点 / CRC / LLEMode 整体 apply 回去，**正在发的 BLE 广播
     * 被物理层摧毁**。
     *
     * 现在只有 b 阶段（真的要用 2.4G 对码）才交出去。 */
    if (tr->phase == UI_TR_SCAN_RF24)
    {
        return false;
    }

    /* 【2026-10-14 新增】已经锁定在 USB 上：射频谁都不需要。
     *
     * 原来这里返回 (cur != UI_TR_RF24) = true，射频被 BLE 白白占着：
     * 广播虽然已经关掉（见 ui_transport.c 的 SCAN_USB 分支），但整颗射频
     * 一直处于 BLE 独占态，于是主循环里 `if (!HALRF_IsBtPhyOwner())` 永远
     * 为假，UiKbdCh_PollRf24() 一次都跑不到 —— 既费电，又让 2.4G 键盘
     * 在插着 USB 时完全不可用。交还 2.4G（谁都不广播，纯待机）即可。
     *
     * 注意顺序：上面的 BT_WCH_IsConnected() 判定在前，所以"USB 插着、
     * 同时 BLE 还连着"这种过渡期，射频仍然归 BLE，链路不会被拆。 */
    if ((tr->phase == UI_TR_LOCKED) && (tr->cur == UI_TR_USB))
    {
        return false;
    }

    return (tr->cur != UI_TR_RF24);
}

/*********************************************************************
 * @fn      radio_set_phy_owner
 * @brief   把射频归属切到目标状态（幂等：只有真的跳变那一拍动硬件）
 * @param   bt_owns  true = 归 BLE 协议栈；false = 交还 2.4G
 *
 * 【两个方向不对称，别合并】
 *   * 交还 2.4G：hal_rf_ch592.c 内部会按缓存 apply 一次，把地址/频点装回去。
 *   * 收回给 BLE：**必须**额外跑一次 RF_RoleInit()（封装在
 *     BT_WCH_RestorePhy 里）—— 协议栈的 LLE 配置已经被 2.4G 改写过了，
 *     不重写的话广播/连接看着正常、实际一个包都发不出去。
 *     而且这一步只在"真的发生交接"时做：BLE 连着的时候再跑 RF_RoleInit
 *     会把正在用的连接带断，所以不能无脑调。
 *********************************************************************/
static void radio_set_phy_owner(bool bt_owns)
{
    if (bt_owns == HALRF_IsBtPhyOwner())
    {
        return;                             /* 没跳变：什么都不做 */
    }

    if (bt_owns)
    {
        HALRF_BtOwnsPhy(true);              /* 先定归属：2.4G 从此不再写 PHY */
        BT_WCH_RestorePhy();                /* 再让协议栈重写 LLE + 重开广播 */
        BT_WCH_Process();                   /* 推一下 TMOS，别把重启停在半路 */
    }
    else
    {
        HALRF_BtOwnsPhy(false);             /* 内部按缓存把 2.4G 配置 apply 回去 */
    }
}

/* 切到"无无线"档：USB 断电、BLE 停广播、射频掉电、遥控释放 */
/* 【2026-10-14 诊断】档位切换计数器。
 * 现场怀疑"蓝牙初始化和别的初始化交替进行" —— 也就是 want 在
 * RADIO_KBD / RADIO_NONE 之间来回跳，每跳一次就 rf_power_off()+重初始化，
 * BLE 连接必死。屏幕上会显示这三个数，只要它们在涨就说明猜想成立。 */
volatile uint32_t g_rc_none_cnt = 0u;   /* 进 RADIO_NONE 次数（会 rf_power_off） */
volatile uint32_t g_rc_kbd_cnt  = 0u;   /* 进 RADIO_KBD  次数（会跑 BLE 初始化） */
volatile uint32_t g_rc_rc_cnt   = 0u;   /* 进 RADIO_RC   次数 */

static void radio_enter_none(void)
{
    g_rc_none_cnt++;
    if (s_radio == RADIO_RC)
    {
        rc_stop();
    }

    /* BLE：停广播（协议栈本身不拆，拆了下次起要重新加载很慢，
     * 而且 TMOS 常驻但不广播时几乎不耗电）。
     * 注意 StopAdvertising **不会**拆掉已经建立的连接，是安全的。 */
    BT_WCH_StopAdvertising();

    /* USB：断开设备，省掉 USB 收发器的静态电流 */
    UsbKbd_DeviceShutdown();

    /* 【2026-10-14】这里是**无条件** rf_power_off()（内部 HALRF_Shut +
     * HALRF_PowerDown）。
     *
     * 这意味着：**离开键盘页 = 射频下电 = BLE 连接立刻断**。
     *
     * 这是**产品要求的行为**（"其他模式不启用无线" + 省电），**不要改**：
     * 主循环的 want = radio_demand_of(cur) 对 home/game/setting/… 返回
     * RADIO_NONE，正是靠这里把射频关干净。
     *
     * 所以排查"蓝牙连不稳"时**不要往这条路径上找** —— 它一断就断在
     * 离开键盘页的那一刻，是预期内的。要查的是【人还在键盘页、连接却
     * 自己掉】的情况。 */
    rf_power_off();
}

/* 切到"键盘"档：USB 上电、BLE 开广播、射频上电走从机 */
static void radio_enter_kbd(void)
{
    g_rc_kbd_cnt++;
#if (USB_DIAG_NO_RADIO)
    /* 【诊断档】只跑 USB：完全不碰 BLE / 2.4G / 射频 */
    UsbKbd_DeviceInit();
    return;
#else
    /* 【顺序很重要】射频/BLE 全部拉起来之后，**最后**才让 USB 上电。
     *
     * 原来第一行就是 UsbKbd_DeviceInit()：D+ 上拉一使能，主机立刻开始枚举，
     * 而下面这几步（BLE 协议栈初始化、UiKbdCh_Init() 里等于再跑一次
     * HALRF_Init 重配射频、射频上电）都还在同一次调用里没跑完 ——
     * 等于"主机正在枚举，系统正在初始化射频"。
     * 改成最后上电：主机开始枚举时系统已经进入稳态。 */

    /* 【2026-10-14】这里**不再**无条件 BT_WCH_StartAdvertising()。
     *
     * 广播的唯一所有者是传输层仲裁器（ui_transport.c）：
     * 只有它走到 c 阶段（SCAN_BLE）才允许开广播。进键盘页就开广播的
     * 坏处是：a 阶段锁上 USB、或 b 阶段把射频交给 2.4G 的那 1.5~2s 里，
     * BLE 广播一直开着却被 RF_Config() 打断物理层 ——
     * 对外表现就是"插上 USB 蓝牙还在广播"，以及"刚连上就被拆"。
     *
     * 首次进入时的广播由 bt_wch.c 的 GAPROLE_STARTED 分支建链后补开
     * （同样要过仲裁器的意图标志），时序不受影响。
     *
     * BLE 协议栈只能初始化一次：第一次进键盘模式时起。
     * 之后只用 Start/StopAdvertising 开关，不重启。 */
    if (s_bt_inited == 0u)
    {
        BT_WCH_Init();      /* 内部会起协议栈并开广播（幂等） */
        s_bt_inited = 1u;
    }

    /* ---- 【P0 修复】射频归属必须先判，再让 2.4G 驱动初始化 ----
     *
     * CH592 只有一套射频前端。BT_WCH_Init() 刚把 LLE PHY 配好，而下面
     * UiKbdCh_Init() 里的 HALRF_Init()、rf_power_on() 的 HALRF_PowerUp()、
     * UiKbdCh_SetRadioMode() 的 HALRF_SetLink() 会紧接着连调三次 RF_Config()，
     * 把广播赖以生存的 accessAddress / 频点 / CRC / rfStatusCB 整体改写成
     * 2.4G 的参数。协议栈随后"看起来在广播、实际一个包也发不出去" ——
     * 这就是"能进 GAPROLE_ADVERTISING 但主机永远扫不到"的根因。
     *
     * 判据按仲裁阶段来（见 want_bt_phy）：探 2.4G 的阶段射频算 2.4G 的，
     * 其余时候归 BLE；归属 BLE 期间，hal_rf_ch592.c 里所有
     * RF_Config()/RF_Shut() 都降级成"只写软件缓存"。
     * 从遥控档切回来时这里还会顺手把 PHY 从 2.4G 交还给协议栈
     * （radio_set_phy_owner 内部判跳变，不会重复做）。 */
    radio_set_phy_owner(want_bt_phy());

    /* 2.4G：底层只更新软件缓存（射频归 BLE 时不写硬件），
     * 真正切到 2.4G 通道时由主循环交还必须一次性 apply 回去。
     * UiKbdCh_Init() 是幂等的，重复调只会重新记一遍参数。 */
    UiKbdCh_Init();
    s_kbdch_inited = 1u;

    /* 射频上电 + 地址切回键盘本机的（从机角色） */
    rf_power_on();
    UiKbdCh_SetRadioMode(true);

    /* ---- 最后才让 USB 上电（D+ 上拉，主机从这里才开始枚举）---- */
    UsbKbd_DeviceInit();
#endif
}

/* 切到"遥控"档：射频上电，键盘让位 */
static void radio_enter_rc(void)
{
    g_rc_rc_cnt++;
    /* 遥控是纯 2.4G，USB / BLE 都用不上，先断掉 */
    BT_WCH_StopAdvertising();
    UsbKbd_DeviceShutdown();

    /* 【P0 修复】先把射频从 BLE 手里收回来再上电。
     * 遥控协议栈（proto_rc.c）随后会调 HALRF_SetLink() 反复换地址
     * （枚举地址 <-> 数据地址），此时若 PHY 还判给 BLE，
     * 这些配置就只会写进缓存、一个字节也发不出去 ——
     * 表现就是"遥控页一直 0 设备"。 */
    radio_set_phy_owner(false);

    /* 【2026-10-19】关键顺序：先让 HALRF 有一份完整配置和到齐的回调，
     * 再 PowerUp，最后才进遥控协议栈。直接从主页进遥控时，缺了这一步
     * 就会用全 0 配置和空回调启动射频，第一次发射即崩。 */
    kbdch_ensure_hw_init();
    rf_power_on();
    rc_start();
}

/* 主循环每帧调：按需求切档（带跳变沿判定） */
static void radio_task(radio_demand_t want)
{
    if (want == s_radio)
    {
        return;                     /* 没变，什么都不做 —— 关键 */
    }

    /* 【P0 修复】离开遥控档一定要走 rc_stop()，它做两件事：
     *   1) 把射频链路地址还给键盘（UiKbdCh_SetRadioMode(true)）
     *   2) 把 s_rch 归还给堆（GameMem_Yield）
     * 原来只有"切到 RADIO_NONE（睡眠）"这一条路径会经过它：从遥控页直接
     * 切到键盘页时遥控主机结构体会一直占着那块共享堆，之后再进游戏就
     * Game_PoolAcquire() 申请不到内存 —— 两级失败叠加，
     * 表现为"游戏进不去、遥控也废了"。 */
    if ((s_radio == RADIO_RC) && (want != RADIO_RC))
    {
        rc_stop();
    }

    /* 先把旧的收干净，再进新的。
     * 顺序很重要：先从遥控退出（rc_stop 会把射频模式还给键盘），
     * 再按新档重新配置，避免残留状态互相打架。 */
    switch (want)
    {
        case RADIO_NONE: radio_enter_none(); break;
        case RADIO_KBD:  radio_enter_kbd();  break;
        case RADIO_RC:   radio_enter_rc();   break;
        default: break;
    }

    s_radio = want;
}

/*==========================================================================
 * 键盘灯任务（主循环里每圈调一次，很快，自己按 20ms 分频）
 *
 *   * 灯效选择：把设置里的 0..4 经 RGB_EffectOfPreset() 映射成实际灯效号，
 *     和"灯效选择页"用的是同一个入口，不会两边对不上。
 *   * 动画推进：RGB_EffectTask() **内部不自己计时**（每调一次 phase++），
 *     所以必须由这里按 20ms 喂 —— 以前没人调，动画一直是不动的。
 *   * 按键提示：开了就把当前键位喂给灯（bit0..bit8 = K1..K9），
 *     按下的键会亮白；RGB 驱动内部会把白色叠加在任何灯效之上，
 *     所以"灯效"和"按键提示"是互相独立的两个设置。
 *     灯的物理顺序由 RGB 驱动里的蛇形换算负责：
 *       K1..K9 -> 物理 0,1,2,5,4,3,6,7,8（即 1,2,3,6,5,4,7,8,9）
 *========================================================================*/
/* 【2026-10-14】从 20ms 放到 50ms。RGB_Show() 每跑一次就要短暂关中断，
 * 而它正是之前蓝牙被"疯狂连接断开"的真凶（详见 rgb_led.c 里 RGB_Show()
 * 上方那段）。逐字节关中断已把单次屏蔽从 270us 降到 10us，
 * 这里再降 2.5 倍频率，两次改善叠加后中断屏蔽占空比从 1.35% 降到 **0.02%**。
 * 20fps 的灯效动画肉眼几乎没差别，50ms（20fps -> 20fps... 实为 20fps->20fps）
 * 对手指跟随更是完全够用。 */
#define LED_TASK_MS     50u

/*==========================================================================
 * 空闲休眠（需求 (五).3：(五).3.b 选 5/10/20/60min/永不，(五).3.c 存 Flash）
 *
 * 【修复 §8-6】原来休眠时间只写进 Flash（sleep 页 save 一下就完了），
 * UiStore_SleepMinutes() 全工程没有任何调用者 —— 选了"5 分钟"也永远不会
 * 真休眠。这里补上"空闲计时 -> 到点休眠 -> 有人动就唤醒"。
 *
 * 休眠做的事：关背光 + 屏进 sleep + 键盘灯灭 + 射频下电。
 * 唤醒：矩阵键按下 / 编码器旋转 / 编码器按键 —— 任意一个。
 *
 * 注意：这里做的是"整机外设休眠"，**没有**进 MCU 的 LowPower 深睡，
 * 因为深睡要先把 USB、BLE 协议栈和射频逐个安顿好（否则醒来要重新枚举/
 * 重连），那是另一件事。好处是唤醒零延迟、不会丢连接状态。
 *========================================================================*/
static uint8_t  s_sleeping;                 /* 1 = 已进休眠 */
static uint32_t s_idle_ms;                  /* 最近一次"有人动"的真实毫秒 */
static int16_t  s_enc_seen;                 /* 上一次看到的编码器位置 */

static void app_sleep_enter(void)
{
    if (s_sleeping != 0u)
    {
        return;
    }

    s_sleeping = 1u;

    TPORT_Bl(0);                    /* 背光最耗电，先关 */
    TFT_EnableSleep(true);          /* 屏进 sleep-in */
    RGB_Switch(0u);                 /* 键盘灯灭 */
    radio_task(RADIO_NONE);         /* 射频/广播全下电 */

    BOOT_TRACE("Z sleep");
}

static void app_sleep_exit(void)
{
    if (s_sleeping == 0u)
    {
        return;
    }

    s_sleeping = 0u;
    s_idle_ms  = g_ms_tick;

    TFT_EnableSleep(false);         /* 先醒屏，再开背光，免得看到花屏 */
    TPORT_Bl(1);

    {
        ui_cfg_t *c = UiStore_Cfg();

        if (c != 0)
        {
            RGB_Switch(c->led_on);
        }
    }

    UI_Dirty();                     /* 整页重画一次 */
    BOOT_TRACE("Z wake");
}

/* 主循环每圈调一次 */
static void app_sleep_task(void)
{
    uint32_t now = g_ms_tick;
    uint16_t minutes;
    uint8_t  active = 0u;
    int16_t  pos;

    /* ---- 1. 有没有人在动 ---- */
    if (KEY_GetMap() != 0u)                                 { active = 1u; }
    if (ENC_GetRawButton() != 0u)                           { active = 1u; }

    pos = ENC_GetPosition();
    if (pos != s_enc_seen)
    {
        s_enc_seen = pos;                                   /* 转了 = 有动作 */
        active = 1u;
    }

    if (active != 0u)
    {
        s_idle_ms = now;
    }

    /* ---- 2. 休眠中：有人动就醒，没人动就接着睡 ---- */
    if (s_sleeping != 0u)
    {
        if (active != 0u)
        {
            app_sleep_exit();
        }
        return;
    }

    /* ---- 3. 没休眠：空闲到点就睡 ---- */
    {
        ui_cfg_t *c = UiStore_Cfg();

        if (c == 0)
        {
            return;
        }

        minutes = UiStore_SleepMinutes(c->sleep_idx);
    }

    if (minutes == 0u)
    {
        return;                     /* 0 = 永不 */
    }

    if ((uint32_t)(now - s_idle_ms) >= ((uint32_t)minutes * 60000u))
    {
        app_sleep_enter();
    }
}

static void led_task(void)
{
    ui_cfg_t *cfg = UiStore_Cfg();
    static uint8_t  s_on  = 0xFFu;      /* 0xFF = 还没同步过 */
    static uint8_t  s_fx  = 0xFFu;
    static uint16_t s_map = 0xFFFFu;
    static uint32_t s_last_ms;

    if (cfg == 0)
    {
        return;
    }

    /* ---- 总开关（变了才动，避免每圈都去刷灯）---- */
    if (s_on != cfg->led_on)
    {
        s_on = cfg->led_on;
        s_fx = 0xFFu;                   /* 强制下面重设一次灯效 */
        RGB_Switch(s_on);
    }
    if (s_on == 0u)
    {
        return;                         /* 灯关着：什么都不用做 */
    }

    /* ---- 灯效选择 ---- */
    if (s_fx != cfg->led_fx)
    {
        s_fx = cfg->led_fx;
        RGB_ShowEffect(RGB_EffectOfPreset(s_fx));
    }

    /* ---- 按键提示：按下的键亮白 ---- */
    {
        uint16_t map = (cfg->key_hint != 0u) ? KEY_GetMap() : 0u;

        if (map != s_map)
        {
            s_map = map;
            RGB_UpdateByKeyMap(map);
        }
    }

    /* ---- 灯效动画：每 20ms 推一帧 ---- */
    {
        uint32_t now = Game_Millis();

        if ((uint32_t)(now - s_last_ms) >= (uint32_t)LED_TASK_MS)
        {
            s_last_ms = now;
            RGB_EffectTask();
        }
    }
}

/*==========================================================================
 * main
 *========================================================================*/
/* 【2026-10-15】复位源 / 开机次数：开机时无条件读一次并递增。诊断行关掉
 * 之后这两个数依然有效（重新打开 LOOP_DIAG 就能直接看到有意义的 B/S）。 */
static volatile uint8_t s_reset_src = 0xEEu;
static volatile uint8_t s_boot_cnt  = 0u;

/* 开机背光闸门：UI 第一帧真正 draw 完之前一直保持 0。
 * UI_Task() 返回 1 表示这一圈执行了 page->draw()，此时才允许点亮背光，
 * 保证用户看到的第一眼就是完整界面，而不是面板上电后的随机 GRAM。 */
static uint8_t s_boot_bl_on = 0u;

#if (LOOP_DIAG)
static uint32_t s_ld_cnt, s_ld_last_ms, s_ld_worst, s_ld_t0;

/* == 【2026-10-14】复位检测 ============================================
 * R8_GLOB_RESET_KEEP(0x40001047) 手册注释是 "value keeper during global
 * reset" —— 它的值在复位后保留，所以拿它当开机计数器，就能数出这颗芯片
 * 到底重启过几次。
 * R8_RESET_STATUS(0x40001044) 低 3 位 RB_RESET_FLAG 是复位源：
 *   000 = SR   软件复位（HardFault 处理器 / SYS_ResetExecute 干的）
 *   001 = RPOR 真实上电复位（= 供电跌落 / 欠压！）
 *   010 = WTR  看门狗超时复位
 *   011 = MR   外部 RST 引脚被拉低
 * 判读：B(开机次数) 随 6<->23 循环一起涨 => 芯片在反复复位，而不是协议栈
 * 拆链重播；再看 S 就知道是供电还是软件。
 * ==================================================================== */
/* 注意：s_reset_src / s_boot_cnt 已在上面（#if 之外）定义过，
 * 这里**不能**再定义一次 —— 否则打开 LOOP_DIAG 就会报 redefinition。 */
static void loop_diag_begin(void) { s_ld_t0 = g_ms_tick; }

static void loop_diag_end(void)
{
    uint32_t now  = g_ms_tick;
    uint32_t iter = (uint32_t)(now - s_ld_t0);

    s_ld_cnt++;
    if (iter > s_ld_worst) { s_ld_worst = iter; }

    if ((uint32_t)(now - s_ld_last_ms) >= 1000u)
    {
        /* FW38：红条已经画了就不要再动屏幕，否则每秒会被诊断行盖掉一块。
         * （这里的 return 无害：只是让 s_ld_* 计数晚一点复位）*/
        if (g_hang_bar != 0u) { return; }
        uint32_t el = (uint32_t)(now - s_ld_last_ms);
        uint32_t hz = (el != 0u) ? ((s_ld_cnt * 1000u) / el) : 0u;
        uint32_t ops = g_tft_ops;   /* 先取样，再画诊断行 */
        uint32_t px  = g_tft_px;

        /* FW40：屏幕只有 240px 宽、字宽 6px，一行最多 40 个字符 —— 原来那行
         * 一共 48 个字符，右半边（U / A / B / 页名）全跑到屏幕外面去了。
         * 改成两行，颜色区分：上行绿、下行青。
         * 行1：R 断开原因码 / B 开机次数 / W 最近一秒最慢一圈(ms)
         *      U UI_Task() 历史最长(ms) / D 单次整页重画最长(ms) / SK 栈高水位
         * 行2：S 复位源 / I 连接间隔(1.25ms) / s 主循环阶段 / b 蓝牙步
         *      T,C 射频通道 phase,cur / A 广播中 / B 已连接 / 页名 */
        TFT_FillRect(0, (int16_t)(LCD_H - 21), (int16_t)LCD_W, 21, 0x0000u);
        TFT_SetTextSize(1);
        TFT_SetTextColorBg(0x07E0u, 0x0000u);
        TFT_SetCursor(1, (int16_t)(LCD_H - 20));
        /* O = 每秒"昂贵绘制操作"次数（TFT_SetAddrWindow），P = 每秒写出的像素数(K)。
         * 用这两个数就能分清是"调用次数太多"还是"数据量太大"：
         *   O 很大 / P 不大  -> 每次调用的固定开销是瓶颈（地址窗口那 11 字节命令）
         *   P 很大            -> SPI 数据量本身是瓶颈（要减少重画面积） */
        {
            /* 【蓝牙诊断】直接显示仲裁状态，一眼看出卡在哪一档：
             *   T = phase（0=USB扫描 1=2.4G扫描 2=BLE扫描 3=已锁定）
             *   C = cur   （0=无 1=USB 2=2.4G 3=BLE）
             *   A = BLE 是否在广播
             *   B = BLE 是否已连接
             * 期望：进键盘页后 T 由 0 → 1 → 2，再过 ~2 秒 A 应该 =1。 */
            const ui_tr_t *tr = UiKbd_Transport();

            /* 【2026-10-14】现场量到 W = 140ms —— 主循环有一圈被阻塞 140 毫秒，
             * 而 BLE 连接间隔只有 7.5~11.25ms，等于整帧错过十几次连接事件，
             * 监督超时到点就断链。这就是 6<->23 循环的根因。
             *
             * 把工程原有的两个量加回来，用来定位这 140ms 到哪去了：
             *   O = 每秒 TFT_SetAddrWindow 次数（每次有固定开销：地址窗口 11 字节命令）
             *   P = 每秒写出的像素数(K)（SPI 数据量本身）
             *   O 大 / P 小 -> 调用次数太多（固定开销是瓶颈）
             *   P 大        -> 数据量太大（要减少重画面积）
             * 断开那一下 O/P 飙高 => 凶手就是整屏重画。 */
            /* 【2026-10-14】加上当前页名 —— 133ms 的一次整屏重画是哪个页面干的，
             * 看这个字段就知道。 */
            {
                const char *pg = "-";
                const ui_page_t *cc = UI_Current();
                if ((cc != 0) && (cc->name != 0)) { pg = cc->name; }
                { extern volatile uint32_t g_tft_reset_cnt;
                /* 【2026-10-14】诊断行改版，把三个"查明真相"的字段提到最前
                 * （放前面是为了即使右边被裁掉也一定能看到）：
                 *   R = 断开原因码(十六进制)  B = 开机次数  S = 复位源
                 *   I = 协商到的连接间隔(单位 1.25ms，I6=7.5ms)
                 * 判读：
                 *   B 一直涨          -> 芯片在复位（看 S：1=供电 0=软件）
                 *   R13               -> 主机主动断（服务发现/配对不满意）
                 *   R08               -> 设备侧监督超时（主循环阻塞）
                 *   R16               -> 本机自己断（代码有鬼）
                 *   R3B / I 异常      -> 连接参数协议问题
                 */
                TFT_Printf("R%02X B%u W%lu U%lu D%lu SK%u",
                           (unsigned)BT_WCH_LastDiscReason(),
                           (unsigned)s_boot_cnt,
                           (unsigned long)s_ld_worst,
                           (unsigned long)g_ui_ms_max,
                           (unsigned long)g_draw_ms_max,
                           (unsigned)g_stack_hw);
                TFT_SetTextColorBg(0x07FFu, 0x0000u);
                TFT_SetCursor(1, (int16_t)(LCD_H - 10));
                TFT_Printf("S%u I%u s%u b%u T%uC%u A%uB%u %s",
                           (unsigned)s_reset_src, (unsigned)BT_WCH_ConnInterval(),
                           (unsigned)g_loop_stage, (unsigned)g_bt_diag_step,
                           (unsigned)((tr != 0) ? (unsigned)tr->phase : 9u),
                           (unsigned)((tr != 0) ? (unsigned)tr->cur   : 9u),
                           (unsigned)(BT_WCH_IsAdvertising() ? 1u : 0u),
                           (unsigned)(BT_WCH_IsConnected()   ? 1u : 0u),
                           pg);
                }
            }

        }
        (void)ops; (void)px; (void)hz;

        s_ld_last_ms = now;
        s_ld_cnt     = 0u;
        s_ld_worst   = 0u;
        g_tft_ops = 0u;
        g_tft_px  = 0u;
    }
}
#endif

int main(void)
{
    ui_cfg_t *cfg;
#if (LCD_BOOT_TRACE)
    uint8_t trace_once = 1u;
#endif

    /* 栈高水位测量的第一步，必须放在最前面：
     * 之后每一次更深的调用（包括 BLE 初始化里那条 688 B 的 printf 链）
     * 都会被记下来，崩溃画面上的 SK 就是它。 */
    /* 【2026-10-14】最早期读复位源 + 递增开机计数器（必须在任何
     * 可能改 R8_GLOB_RESET_KEEP 的操作之前） */
    s_reset_src = (uint8_t)(R8_RESET_STATUS & RB_RESET_FLAG);
    s_boot_cnt  = (uint8_t)(R8_GLOB_RESET_KEEP + 1u);
    R8_GLOB_RESET_KEEP = s_boot_cnt;

    stack_paint();

    SetSysClock(CLK_SOURCE_PLL_60MHz);

#if (USB_DIAG_BARE == 1)
    /* 【诊断档 1】只跑 USB：详见 USB_DIAG_BARE 的说明 */
    UsbKbd_DeviceInit();
    while (1)
    {
        UsbKbd_DeviceTask();
    }
#endif

    /* ---- 1. 屏 ----
     * 这里只做最低限度的初始化：
     *   1) TPORT_InitPins() 先把背光配成"灭"
     *   2) 在背光仍然灭着的时候刷一遍黑，清掉 ST7789 上电 GRAM 的随机内容
     * 真正的横屏初始化在 Game_Init() 里（ST7789_Init(135,240) + 旋转 1），
     * 所以**自检不能放这儿** —— 此时尺寸是错的，画了也看不见。
     *
     * 【2026-10-19 修复】背光绝不能在这里开：
     * ST7789_Init() 里已经发了 DISPON，此时 GRAM 还是随机的；先开背光
     * 就会看到"花屏一帧"。背光统一等到 UI_Task() 真正画完第一帧后再开。 */
    ST7789_Init(LCD_PANEL_W, LCD_PANEL_H);
    TFT_FillScreen(COL_BOOT_BG);        /* 背光灭：先把 GRAM 刷黑 */

    /* ---- 2. 输入 ---- */
    KEY_Init();                 /* 3x3 矩阵键盘 */
    ENC_Init();                 /* 编码器 */

    /* ---- 3. 外设 ---- */
    BAT_Init();
    RGB_Init();

    /* ---- 4. 设置（数据 Flash，掉电保留）---- */
    UiStore_Init();
    (void)UiStore_Load();

    /* ---- 4b. 按键映射：从 Flash 载入 ---- */
    KmapStore_Init();

    /* ---- 4c. 键盘宏：从 Flash 载入（偏移 0x1600，独立一块）---- */
    Macro_Init();

    cfg = UiStore_Cfg();
    if (cfg != 0)
    {
        RGB_Switch(cfg->led_on);
        TPORT_BlSetBright(cfg->scr_bright);   /* 只记亮度，背光仍灭着 */
        RGB_SetBrightness(cfg->led_bright);
        (void)KEY_SetMapGroup(cfg->map_group);
    }

    /* ---- 5. 游戏公共层 ---- */
    Game_Init(GAME_LANDSCAPE);                          /* 横屏 240x135 */
    Game_Srand(SYS_GetSysTickCnt() ^ 0x1234ABCDu);
    enc_timer_init();           /* 1ms 定时器：ENC_Scan() + g_ms_tick */

    /* Game_Init() 内部会**再调一次 ST7789_Init()**，那里面又会跑
     * TPORT_InitPins()，把背光脚（PB4）重新置成关电平。
     * 这是符合预期的：从这一步一直到 UI 画完第一帧，背光都保持熄灭。 */

#if (LCD_BOOT_SELFTEST)
    /* 开机屏自检：必须放在这里（Game_Init 之后）。
     * 此时屏幕才按正确尺寸初始化过，偏移量才对，颜色才画得到屏上。
     * 自检本身就是要看颜色，所以只在这段临时打开背光；结束时重新熄灭，
     * 继续等 UI 第一帧画完再正式开背光。
     *   能看到 红->绿->蓝->白 = 显示链路完全正常；
     *   看不到                = SPI / 面板 / 接线问题。 */
    TPORT_Bl(1);
    TFT_FillScreen(0xF800u); DelayMs(250);   /* 红 */
    TFT_FillScreen(0x07E0u); DelayMs(250);   /* 绿 */
    TFT_FillScreen(0x001Fu); DelayMs(250);   /* 蓝 */
    TFT_FillScreen(0xFFFFu); DelayMs(250);   /* 白 */
    TFT_FillScreen(COL_BOOT_BG);
    TPORT_Bl(0);
#endif

    /*========================================================================
     * ---- 6. 无线：**开机一律不上电** ----
     *
     * 这里只做最轻的准备工作，让后面按需上电时能立刻生效：
     *   * UiKbdCh_Init()  注册三条通道的出口回调（纯软件接线，不碰硬件）
     *   * BLE 协议栈的 TMOS 需要初始化一次才能响应 StartAdvertising，
     *     但 BT_WCH_Init() 内部会直接开广播 —— 所以这里先不调它，
     *     等真进键盘页时由 radio_enter_kbd() 里按需触发。
     *
     * 射频：开机时保持 PowerDown，第一次进键盘/遥控页才 PowerUp。
     *======================================================================*/
    BOOT_TRACE("A kbdch-soft");
    UiKbdCh_InitSoft();         /* 只注册回调，绝不碰射频 */
    BOOT_TRACE("B rf-off");
    rf_power_off();             /* 确保射频是关的（防止复位前状态残留） */

    /* 说明：BT_WCH_Init() 需要 TMOS 常驻，这里用"惰性首次初始化"——
     * 用一个静态标志保证它只会被执行一次，之后由广播开关控制。 */
    /* 见 radio_enter_kbd() 里的 s_bt_inited 处理。 */

    /* ---- 6.5 内存自检：把"链接结果里的真实堆容量"和上面的预算对一次 ----
     * 编译期护栏管的是"结构体本身装不装得下"，这里补的是另一半：
     * 万一以后有人改了 Link.ld 的 __stack_size、或往 .bss 里加了东西，
     * 堆会被悄悄压小，只有到上机用遥控时才会发现。这里启动就比一次，
     * 不一致就把原因挂到遥控页的状态提示上（UiRc_SetStatus）。
     * 只影响诊断显示，不改启动流程。 */
    if (heap_bytes() < ((uint32_t)sizeof(rc_host_t) + 32u))
    {
        UiRc_SetStatus("MEM LOW");
        BOOT_TRACE("Z heap small");
    }

    /* 游戏状态池是这块堆上最大的一笔（雷霆 46 个对象 x 20 B = 920 B）。
     * FW39 之前这里曾经**静默**失败：堆被压到 752 B，Game_PoolAcquire()
     * 返回 false 而 game_start() 只写了 return，表现成"游戏列表页能左右
     * 切、就是进不去"。所以必须显式比一次，别再让它无声无息。 */
    if (heap_bytes() < ((uint32_t)Game_PoolSize() + 32u))
    {
        UiRc_SetStatus("GAME LOW");
        BOOT_TRACE("Z game pool small");
    }

    /* ---- 7. UI ---- */
    BOOT_TRACE("C bat");
    UiW_BatInit();
    UI_Init();                                          /* 栈底是主页面 */

#if (USB_DIAG_BARE == 2)
    /* 【诊断档 2】应用初始化（屏/矩阵/编码器/电量/TMR0/UI）已经全跑完了，
     * 这里才让 USB 上电，主循环里只跑 UsbKbd_DeviceTask()。
     *   能枚举 -> 病在【主循环里的任务】(Game_Task / UI_Task / UiW_BatTask ...)
     *   不能   -> 病在【应用初始化】 */
    UsbKbd_DeviceInit();
    while (1)
    {
        UsbKbd_DeviceTask();
    }
#endif

    /* ---- 8. 主循环 ---- */
    while (1)
    {
#if (LOOP_DIAG)
        loop_diag_begin();
#endif
        g_loop_beat++;
        const ui_page_t *cur;
        radio_demand_t   want;
        uint8_t          ui_drew = 0u;

        g_loop_stage = 1u;
        Game_Task();        /* 1ms 节拍：扫矩阵键盘 + 产生按键事件 + 延时 */

        /* 栈高水位：500ms 采一次。stack_probe() 只读不写，
         * 采完把历史最大值存进 g_stack_hw（崩溃画面 SK 行显示它）。 */
        {
            static uint32_t s_sk_last = 0u;

            g_loop_stage = 2u;
            uint32_t now_sk = g_ms_tick;

            if ((uint32_t)(now_sk - s_sk_last) >= 500u)
            {
                uint32_t hw = stack_probe();
                s_sk_last = now_sk;
                if (hw > g_stack_hw)
                {
                    g_stack_hw = hw;
                }
            }
        }

        /* 当前页决定无线需求 */
        cur  = UI_Current();

#if (USB_DIAG_NO_RADIO)
        /* 【诊断档】锁死键盘档、不睡眠、不跑灯效：
         * 主循环里只剩 USB，用来二分"USB 本身"和"射频 + 负载环境"。 */
        want = RADIO_KBD;
#else
        g_loop_stage = 3u;
        app_sleep_task();   /* 空闲休眠：到点关屏关射频，有动作就唤醒（需求 (五).3） */

        g_loop_stage = 4u;
        led_task();         /* 键盘灯：灯效动画 + 按键提示（内部 20ms 分频） */

        /* 休眠中：无线一律断电 —— 覆盖掉页面本身的需求，
         * 唤醒后这里自然又按当前页要回来 */
        want = (s_sleeping != 0u) ? RADIO_NONE : radio_demand_of(cur);
#endif

        /* 惰性切档：只在跳变沿上下电（BLE 首次初始化也在这里面） */
        g_loop_stage = 5u;
        radio_task(want);

        /* ---- 分档跑各自的无线任务 ---- */

        if (s_radio == RADIO_KBD)
        {
            /* USB：设备状态机（含 CDC 虚拟串口收发）*/
            g_loop_stage = 6u;
            UsbKbd_DeviceTask();

#if (USB_DIAG_NO_RADIO)
            /* 【诊断档】不跑 BLE / 2.4G，主循环只留 USB */
#else
            /* BLE：事件处理 */
            g_loop_stage = 7u;
            BT_WCH_Process();

            /* 键盘通道：喂状态 + 仲裁 + 按需发 HID 报文
             * （radio_enter_kbd() 里已保证初始化过，这里直接跑） */
            g_loop_stage = 8u;
            UiKbdCh_Task();

            /* ---- 【P0 修复】射频归属按仲裁阶段同步 ----
             * CH592 只有一套射频前端，BLE 协议栈与 2.4G 私有协议不能同时用。
             * 这里每圈同步一次（幂等，只有真的跳变才动硬件）：
             *     还在探 2.4G 主机（SCAN_USB / SCAN_RF24）-> PHY 归 2.4G
             *     SCAN_BLE / 已锁定                              -> PHY 归 BLE
             * 注意判据必须看**阶段**而不是当前通道，原因见 want_bt_phy()。
             * 【修复 P0-1】2.4G -> BLE 的交接也在 radio_set_phy_owner() 里
             * 一并完成（RF_RoleInit + 重开广播），所以原来那段
             * "切到 BLE 通道就 RestorePhy" 的跳变沿处理已经删掉：
             * 那段代码其实**永远不会执行** —— 通道变成 BLE 的前提是已经连上，
             * 而连上的前提恰好是广播能发出去，鸡生蛋问题。 */
#if (USB_DIAG_NO_RF24)
            HALRF_BtOwnsPhy(true);      /* 诊断档：2.4G 全程停用，PHY 恒归 BLE */
#else
            g_loop_stage = 9u;
            radio_set_phy_owner(want_bt_phy());
#endif

            /* 2.4G 从机：只有"射频确实归 2.4G"时才去等 POLL。
             * 归属在 BLE 手里时 HALRF_RxStart() 本来就会直接失败，
             * 这里判一次是为了别白跑一趟函数调用，意图也更清楚。 */
#if (USB_DIAG_NO_RF24)
            /* 【诊断档】完全不碰 2.4G，射频一直留给 BLE */
#else
            if (!HALRF_IsBtPhyOwner())
            {
                g_loop_stage = 10u;
                (void)UiKbdCh_PollRf24();
            }
#endif
#endif
        }
        else if (s_radio == RADIO_RC)
        {
            /* 遥控主机：按真实毫秒推进协议栈，到超帧点广播一轮。
             *
             * 【修复 §8-12】原来是"每圈 Rc_HostTickMs() 一次 + s_sf_ms++"，
             * 也就是把主循环一圈当 1ms。一轮超帧名义 16ms，实际要跑十几到
             * 几十毫秒，超帧周期被拉长，从机离线判定（48ms）也跟着失真。 */
            if (s_rch != 0)
            {
                uint32_t now = g_ms_tick;
                uint16_t d   = (uint16_t)(now - s_rc_last_ms);

                s_rc_last_ms = now;
                if (d > 100u) { d = 100u; }

                if (d != 0u)
                {
                    uint8_t i;

                    for (i = 0u; i < (uint8_t)d; i++)
                    {
                        g_loop_stage = 11u;
                        Rc_HostTickMs(s_rch);   /* 协议栈仍按 1ms 一步走 */
                    }

                    /* 功能页按下的键：排进命令队列，下一个超帧从机时隙发出 */
                    rc_send_ui_keys();

                    s_sf_ms = (uint16_t)(s_sf_ms + d);
                    if (s_sf_ms >= (uint16_t)RC_SUPERFRAME_MS)
                    {
                        s_sf_ms = (uint16_t)(s_sf_ms % (uint16_t)RC_SUPERFRAME_MS);
                        g_loop_stage = 12u;
                        Rc_HostSuperframe(s_rch);
                        rc_sync_devices();      /* 发现到设备就刷进界面 */
                        s_rc_synced = 1u;
                    }
                }
            }
        }
        /* RADIO_NONE：不跑任何无线任务 —— 射频、USB、BLE 全部停着 */

        /* 电量：每 1 分钟非阻塞采一次，有读数就让电量条重画（需求 (一).2）。
         * 必须用 UI_DirtyFull()：UI_Dirty() 只置脏，走的是 carousel 的
         * 局部刷新分支，画在状态栏里的电量条不会被重画 —— 屏幕会一直
         * 停在开机那个读数上。 */
        g_loop_stage = 13u;
        if (UiW_BatTask() != 0u)
        {
            UI_DirtyFull();
        }

        /* 【修复：电脑上电量不准】把**应用层标定过的**电量推给 BLE 电池服务。
         *
         * 以前这条路整个是断的：官方 battservice.c 的 battMeasure() 里
         * 写死 `adc = 300`（恒 20%），而且 Batt_SetParameter() 根本没有
         * BATT_PARAM_LEVEL 分支，BtGatt_SetBatteryLevel() 谁都没调过 ——
         * 于是电脑读到的永远是那个假值。
         *
         * 每圈调用没问题：BtGatt_SetBatteryLevel() 内部去抖，
         * 百分比没变就直接返回（不写值、不发通知）。 */
        g_loop_stage = 14u;
        BtGatt_SetBatteryLevel(UiW_BatPercent());

        g_loop_stage = 15u;
        /* 【2026-10-18 功耗优化】休眠中不跑 UI_Task()。
         *
         * 原来这里无条件调用。休眠时背光已关、面板已发 SLPIN，但
         * UI_Task() 里的 page->update() 照样每 20ms 跑一次 —— 只要当前
         * 页面的 update 里调了 UI_Dirty()（轮播动画、时钟、电量刷新都会），
         * 紧接着就会走 page->draw()，把整屏数据重新写进一块**已经 sleep-in
         * 的 GRAM**。代价有两个：
         *   1) 白白耗电：一次整页重画 240x135x2 = 64800 字节 SPI + 主频全开，
         *      而屏幕根本看不见；
         *   2) ST7789 在 SLPIN 期间写 GRAM 的行为数据手册未保证，
         *      属于"能用但没道理"的操作。
         *
         * 为什么跳过它是安全的：任何按键/编码器动作都会在**同一圈更早的**
         * app_sleep_task()（g_loop_stage = 3）里先把 s_sleeping 清掉并唤醒屏幕，
         * 所以走到这里时 s_sleeping 只可能"整圈都没人动"。此时没有新事件
         * 需要排空（键层状态是每圈直接读 KEY_IsPressed()，不依赖事件队列）。
         * 唤醒后 app_sleep_exit() 会调 UI_Dirty() 整页重画一次，画面不会缺。 */
        if (s_sleeping == 0u)
        {
            /* FW38：量 UI_Task() 单次最长耗时（屏底诊断行 U 段 / 红条第三行）*/
            uint32_t _u0 = g_ms_tick;
            ui_drew = UI_Task();    /* 页面栈：编码器分档 + 路由 + 按帧重画 */
            {
                uint32_t _u1 = (uint32_t)(g_ms_tick - _u0);
                if (_u1 > g_ui_ms_max) { g_ui_ms_max = _u1; }
            }
        }

        /* 【2026-10-19】开机第一帧已经真正写进 GRAM，才点亮背光。
         * UI_Task() 有 20ms 帧率门和脏标记，所以不能在这里写死延时；
         * 用它的返回值判断，既不提前亮（花屏），也不会一直不亮。
         * 后续休眠唤醒仍由 app_sleep_exit() 自己管理背光，这里只做一次。 */
        if ((s_boot_bl_on == 0u) && (ui_drew != 0u))
        {
            s_boot_bl_on = 1u;
            TPORT_Bl(1);
        }

        /* ---- BLE 诊断打点：屏底那一行 ----
         *
         * 【2026-10-14】必须在这里调，**绝不能**在 bt_wch.c 的 GAP 回调里调。
         * 这一行要做两次 240x17 的 SPI 矩形填充（约 17ms），放在协议栈回调里
         * 会让 TMOS 整整错过 7~11 个连接事件，主机监督超时一到就断链 ——
         * 那正是 6->20->21->22->23->6 循环的成因。
         * 主循环自己的上下文里做，只会让这一圈慢十几毫秒，不影响链路。
         *
         * 内部只在 g_bt_diag_step 变化时才真正画，所以稳态零开销。 */
        g_loop_stage = 16u;
        BT_WCH_DrawDiag();

#if (LOOP_DIAG)
        g_loop_stage = 17u;
        loop_diag_end();
#endif
        g_loop_stage = 0u;

#if (LCD_BOOT_TRACE)
        if (trace_once != 0u)
        {
            trace_once = 0u;
            BOOT_TRACE("F ui-task ok");
        }
#endif
    }
}


/*==========================================================================
 * HardFault 异常处理
 *
 * 为什么必须有：
 *   Startup/startup_CH592.S 里 HardFault_Handler 只是 .weak 空桩（只有标号，
 *   没有代码）。一旦发生越界访问 / 非法指令 / 栈溢出，任何异常都会静默跑飞，
 *   现象就是"用着用着死机"，完全没有线索可查。
 *
 * 这里把出错时的 PC / mepc / mcause / mstatus 存进全局变量，然后用 RGB
 * 闪灯 + 屏幕打点的方式指示出来，同时 PC 值本身也能直接读出来判断地址范围。
 *
 * 关键约束：
 *   __INTERRUPT  必须带 —— RISC-V 异常返回用 mret，漏了这个属性编译成 ret
 *                      会直接打飞控制流。
 *   __HIGH_CODE   放 RAM 里跑 —— 栈可能已经溢出，且 Flash 在写 Flash 时不可读。
 *   此函数内**绝不能**调用带大栈数组的函数（TFT_Printf / spi刷屏等），
 *   否则会在异常里二次崩溃。
 *==========================================================================*/


/* 事故现场（volatile：调试器可直接 watch） */
volatile uint32_t g_fault_pc     = 0u;
volatile uint32_t g_fault_mepc   = 0u;
volatile uint32_t g_fault_mcause = 0u;
volatile uint32_t g_fault_mstatus = 0u;
volatile uint32_t g_fault_count  = 0u;

/* 把事故信息画到 TFT。独立成函数，**不**在 HardFault_Handler 内部展开，
 * 这样异常处理本身的栈帧可以压到最小；显示函数自身实测栈 <= 80 字节。
 *
 * 安全约束（违反会在异常里二次崩溃）：
 *   只用实测栈 <= 32 字节的函数：TFT_FillScreen(16) / TFT_FillRect(32) /
 *   Game_DrawNumberRight(32) / TFT_WriteChar(16) / TFT_SetCursor(0) 等。
 *   绝不用 TFT_Printf(160) / TFT_DrawChar(176) / memcpy / malloc / DelayMs。
 */
/* noinline：强制编译器不要把它内联进 HardFault_Handler。
 * 之前没加这个属性时，-Os 会把 crash_show 整个内联进异常处理，
 * 导致 HardFault_Handler 栈帧从 16 字节暴涨到 304 字节 ——
 * 而崩溃现场剩多少栈是不可知的，304 字节极可能直接二次溢出。 */
__attribute__((noinline))
static void crash_show(void)
{
    const char *p;

    TFT_FillScreen(0x0000u);                          /* 黑底 */

    /* 顶部红条 + 标题（红底白字，一眼认出是事故画面） */
    TFT_FillRect(0, 0, (int16_t)UI_SCREEN_W, 14, 0xF800u);
    TFT_SetCursor(4, 2);
    TFT_SetTextColorBg(0xFFFFu, 0xF800u);
    TFT_SetTextSize(1u);
    p = "HARDFAULT";
    while (*p != 0) { TFT_WriteChar(*p++); }

    TFT_SetTextSize(2u);
    TFT_SetTextColorBg(0x07E0u, 0x0000u);             /* 绿色数值 */

    /* 行距 18 px（字号 2 高 16 px）。五行 + 一行小字，135 px 屏刚好。
     * SK = 栈历史高水位（字节），是判断"是不是栈踩穿"的直接证据：
     *     接近/等于 1024  -> 栈踩穿了，堆可能已被写坏
     *     远小于 1024      -> 不是栈的问题，去看 PC/CA */
    TFT_SetCursor(4, 20);
    p = "PC";  while (*p != 0) { TFT_WriteChar(*p++); }
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 8), 20,
                         (uint32_t)(g_fault_pc & 0xFFFFu), 4u, 0x07E0u, 0x0000u);

    TFT_SetCursor(4, 38);
    p = "CA";  while (*p != 0) { TFT_WriteChar(*p++); }
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 8), 38,
                         (uint32_t)(g_fault_mcause & 0xFFFFu), 4u, 0x07E0u, 0x0000u);

    TFT_SetCursor(4, 56);
    p = "ST";  while (*p != 0) { TFT_WriteChar(*p++); }
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 8), 56,
                         (uint32_t)(g_fault_mstatus & 0xFFFFu), 4u, 0x07E0u, 0x0000u);

    TFT_SetCursor(4, 74);
    p = "CN";  while (*p != 0) { TFT_WriteChar(*p++); }
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 8), 74,
                         (uint32_t)(g_fault_count & 0xFFFFu), 4u, 0x07E0u, 0x0000u);

    TFT_SetCursor(4, 92);
    p = "SK";  while (*p != 0) { TFT_WriteChar(*p++); }
    Game_DrawNumberRight((int16_t)(UI_SCREEN_W - 8), 92,
                         g_stack_hw, 4u, 0x07E0u, 0x0000u);

    TFT_SetTextColorBg(0xFFFFu, 0x0000u);
    TFT_SetTextSize(1u);
    TFT_SetCursor(4, 114);
    p = ((g_fault_pc & 0xF0000000u) == 0x20000000u) ? "PC IN RAM" : "PC IN FLASH";
    while (*p != 0) { TFT_WriteChar(*p++); }
}

/* HardFault 本体：只读 CSR + 存现场 + 调显示 + 停住。
 * 刻意保持极小，栈帧实测 16 字节，把 1024 字节的栈尽量留给显示函数。 */
__INTERRUPT
__HIGH_CODE
void HardFault_Handler(void)
{
    __asm volatile("csrr %0, mepc"   : "=r"(g_fault_mepc));
    __asm volatile("csrr %0, mcause" : "=r"(g_fault_mcause));
    __asm volatile("csrr %0, mstatus" : "=r"(g_fault_mstatus));

    g_fault_pc = g_fault_mepc;      /* 非中断异常时 mepc 就是出错指令地址 */
    g_fault_count++;

    crash_show();

    while (1) { }                   /* 停住等调试器；退出会让状态不可预期 */
}

/******************* (C) CH592X 键盘整机 *****************/


/*==================================================================================
 * FW36 卡死定位仪（实现）  —— 只在 TMR0 1ms 中断里被调用，只用小栈函数
 *================================================================================*/
volatile uint32_t g_stuck_pc   = 0u;   /* 卡死时被中断打断的 PC（真正停住的位置） */
volatile uint32_t g_stuck_pc2  = 0u;   /* 第二次采样（间隔 1 秒），用于确认死循环 */
volatile uint32_t g_stuck_gisr = 0u;   /* PFIC->GISR：当前正在服务的中断号 */
volatile uint32_t g_stuck_pend = 0u;   /* PFIC->IPR[0]>>16：IRQ16..31 挂起位 */
volatile uint32_t g_stuck_enab = 0u;   /* PFIC->ISR[0]>>16：IRQ16..31 使能位 */
volatile uint32_t g_irq_total  = 0u;   /* 下面这些"补捉中断"被进入的总次数 */
volatile uint32_t g_irq_cnt[20] = {0};

/* FW38：量"整页重画到底多慢" + 红条防重入 */
volatile uint32_t g_ui_ms_max   = 0u;   /* UI_Task() 单次调用最长耗时(ms) */
volatile uint32_t g_draw_ms_max = 0u;   /* page->draw() 单次最长耗时(ms) */
volatile uint8_t  g_hang_bar    = 0u;   /* 1 = 红条已画，之后不许再动屏幕 */

/* 取被中断打断时的 PC：RISC-V 中断进入时 mepc=被打断指令地址 */
static uint32_t hang_mepc(void)
{
    uint32_t v;
    __asm__ volatile ("csrr %0, mepc" : "=r"(v));
    return v;
}

/* 无符号数转 nd 位大写十六进制（中断里不能用 printf/vsnprintf） */
static char *hang_uhex(char *p, uint32_t v, uint8_t nd)
{
    uint8_t i;
    for (i = nd; i != 0u; i--)
    {
        uint8_t d = (uint8_t)((v >> (uint8_t)((i - 1u) * 4u)) & 0x0Fu);
        *p = (char)((d < 10u) ? (0x30u + d) : (0x37u + d));
        p++;
    }
    return p;
}

/* 卡死时画两行红底白字：
 *   H36 s=<阶段> b=<蓝牙步> g=<GISR> P<挂起位> E<使能位>
 *   PC <F/R><5位十六进制> N<补捉中断总次数> X<最靠后的非零槽位>          */
__attribute__((noinline))
static void hang_show2(uint8_t stg, uint8_t bt)
{
    char    b1[40];
    char    b2[40];
    char    b3[40];
    char   *p;
    uint32_t i;
    uint32_t x = 0u;
    uint32_t pc;

    p = b1;
    *p++ = 0x48; *p++ = 0x33; *p++ = 0x36; *p++ = 0x20;             /* H36  */
    *p++ = 0x73; *p++ = 0x3D; p = hang_u16(p, (uint16_t)stg); *p++ = 0x20;
    *p++ = 0x62; *p++ = 0x3D; p = hang_u16(p, (uint16_t)bt);  *p++ = 0x20;
    *p++ = 0x67; *p++ = 0x3D;
    p = hang_u16(p, (uint16_t)(g_stuck_gisr & 0xFFu)); *p++ = 0x20;
    *p++ = 0x50; p = hang_uhex(p, g_stuck_pend & 0xFFFFu, 4u); *p++ = 0x20;
    *p++ = 0x45; p = hang_uhex(p, g_stuck_enab & 0xFFFFu, 4u);
    *p = 0;

    pc = g_stuck_pc;
    p  = b2;
    *p++ = 0x50; *p++ = 0x43; *p++ = 0x20;                         /* PC   */
    *p++ = (pc >= 0x20000000u) ? 0x52 : 0x46;                      /* R / F */
    p = hang_uhex(p, pc & 0x000FFFFFu, 5u);
    *p++ = 0x20; *p++ = 0x4E;
    p = hang_u16(p, (uint16_t)(g_irq_total & 0xFFFFu));
    for (i = 0u; i < 20u; i++) { if (g_irq_cnt[i] != 0u) { x = i; } }
    *p++ = 0x20; *p++ = 0x58; p = hang_u16(p, (uint16_t)x);
    *p = 0;

    /* FW38 第三行：第二次采样的 PC（红条画完之后每 1 秒再采一次，
     * 用来判断"是不是红条自己把主线程搞死的"）+ 最长 UI / 重画耗时。*/
    p = b3;
    *p++ = 0x50; *p++ = 0x32; *p++ = 0x20;                     /* P2    */
    *p++ = (g_stuck_pc2 >= 0x20000000u) ? 0x52 : 0x46;         /* R / F */
    p = hang_uhex(p, g_stuck_pc2 & 0x000FFFFFu, 5u);
    *p++ = 0x20; *p++ = 0x55;                                  /* U     */
    p = hang_u16(p, (uint16_t)((g_ui_ms_max > 9999u) ? 9999u : g_ui_ms_max));
    *p++ = 0x20; *p++ = 0x57;                                  /* W     */
    p = hang_u16(p, (uint16_t)((g_draw_ms_max > 9999u) ? 9999u : g_draw_ms_max));
    *p = 0;

    TFT_FillRect(0, (int16_t)(LCD_H - 33), (int16_t)LCD_W, 33, 0xF800u);
    TFT_SetTextSize(1);
    TFT_SetTextColorBg(0xFFFFu, 0xF800u);
    TFT_SetCursor(1, (int16_t)(LCD_H - 32));
    TFT_Print(b1);
    TFT_SetCursor(1, (int16_t)(LCD_H - 21));
    TFT_Print(b2);
    TFT_SetCursor(1, (int16_t)(LCD_H - 10));
    TFT_Print(b3);
}

/*----------------------------------------------------------------------------------
 * 给原本没有处理函数的中断补上真处理函数。
 * 它们原来全部落在 Startup/startup_CH592.S 的 "1:  j 1b" 上：一旦被投递就永久卡死。
 * 现在改成计数后返回：即使真的意外触发，也只是被记一笔，不再死机；
 * 命中次数会显示在卡死界面第二行的 N<总次数> X<最靠后槽位> 上。
 * 槽位：1 SysTick  2 SW      3 GPIOA  4 GPIOB  5 SPI0   6 BLEB
 *       7 TMR1     8 TMR2    9 UART0 10 UART1 11 ADC   12 I2C
 *      13 PWMX    14 TMR3   15 UART2 16 UART3 17 WDOG_BAT
 * （HardFault / TMR0 / RTC / USB / LLE / Ecall 本来就已有真实现，不覆盖）
 *----------------------------------------------------------------------------------*/
extern void BB_IRQLibHandler(void);     /* BLE 基带中断：协议栈库自带的处理函数 */

#define HANG_IRQ_NOTE(_i)   do { g_irq_cnt[(_i)]++; g_irq_total++; } while (0)

__INTERRUPT __HIGH_CODE void SysTick_Handler(void)
{
    (*(volatile uint32_t *)0xE000F004u) = 0u;   /* SysTick->SR 清挂起，防中断风暴 */
    HANG_IRQ_NOTE(1);
}

__INTERRUPT void SW_Handler(void)          { HANG_IRQ_NOTE(2);  }
__INTERRUPT void GPIOA_IRQHandler(void)    { HANG_IRQ_NOTE(3);  }
__INTERRUPT void GPIOB_IRQHandler(void)    { HANG_IRQ_NOTE(4);  }
__INTERRUPT void SPI0_IRQHandler(void)     { HANG_IRQ_NOTE(5);  }
__INTERRUPT void TMR1_IRQHandler(void)     { HANG_IRQ_NOTE(7);  }
__INTERRUPT void TMR2_IRQHandler(void)     { HANG_IRQ_NOTE(8);  }
__INTERRUPT void UART0_IRQHandler(void)    { HANG_IRQ_NOTE(9);  }
__INTERRUPT void UART1_IRQHandler(void)    { HANG_IRQ_NOTE(10); }
__INTERRUPT void ADC_IRQHandler(void)      { HANG_IRQ_NOTE(11); }
__INTERRUPT void I2C_IRQHandler(void)      { HANG_IRQ_NOTE(12); }
__INTERRUPT void PWMX_IRQHandler(void)     { HANG_IRQ_NOTE(13); }
__INTERRUPT void TMR3_IRQHandler(void)     { HANG_IRQ_NOTE(14); }
__INTERRUPT void UART2_IRQHandler(void)    { HANG_IRQ_NOTE(15); }
__INTERRUPT void UART3_IRQHandler(void)    { HANG_IRQ_NOTE(16); }
__INTERRUPT void WDOG_BAT_IRQHandler(void) { HANG_IRQ_NOTE(17); }

/* BLE 基带：先计数，再交给库自己的处理函数（只有库函数知道怎么清中断源） */
__INTERRUPT __HIGH_CODE void BB_IRQHandler(void)
{
    HANG_IRQ_NOTE(6);
    BB_IRQLibHandler();
}
