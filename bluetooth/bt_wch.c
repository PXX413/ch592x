/**************************************************************************
 * bt_wch.c —— CH592X BLE 从设备（外设角色）：广播名 CH592X
 *
 * 用 WCH 的 BLE 协议栈（libCH59xBLE.a + CH59xBLE_LIB.h + HAL）
 * 依赖：CONFIG.h / HAL.h（从 CH592EVT 的 EVT\EXAM\BLE\HAL 拷进来的）
 *
 * 起机顺序（和官方 HID_Keyboard 例程一致）：
 *     CH59x_BLEInit() -> HAL_Init() -> 注册 TMOS 任务
 *     -> GAPRole_PeripheralInit() -> 设广播数据/设备名
 *     -> GAPRole_PeripheralStartDevice() -> 打开广播
 * 之后主循环里不停调 TMOS_SystemProcess()
 **************************************************************************/

#include "CONFIG.h"
#include "tft_st7789.h"
#include "tft_gfx.h"
#include "HAL.h"
#include "bt_wch.h"
#include "bt_gatt.h"
#include "hiddev.h"
#include "hidkbdservice.h"
#include "battservice.h"


/*==========================================================================
 * BLE 协议栈要的内存堆（例程里放在 main.c，这里放在本文件，只能有一份）
 *========================================================================*/
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/* 初始化走到第几步（给主循环显示用）：1=B1 ... 6=B6，7=初始化返回 */
/* 栈高水位探针，定义在 User/Main.c（g_stack_hw = _eusrstack - 用到的地址） */
extern volatile uint32_t g_stack_hw;

/* 固件标识：每次改了这一区就 +1，屏幕上会显示成 "BT step N FWxx" */
#define BT_FW_TAG  "52"

volatile uint8_t g_bt_step = 0;

/* 在屏上打进度：复位/卡死前最后看到的数字，就是出问题的那一步 */
/* 供主循环读取的诊断快照。主循环自己画到屏上（见 BT_WCH_DrawDiag）。 */
volatile uint8_t  g_bt_diag_step;
volatile uint32_t g_bt_diag_tick;

/*==========================================================================
 * 【2026-10-14】诊断刷屏开关，默认**关闭**
 *
 * 为什么要有这个开关：BT_WCH_DrawDiag() 一次要刷两张 240x17 的矩形
 * （8160 px）+ 两次 TFT_Printf，实测换算约 17ms。对主循环来说可以接受，
 * 但它有两个真实风险：
 *   1) 屏幕处于 sleep-in 时（app_sleep_enter 发的 SLPIN）往GRAM 里写，
 *      行为未验证；
 *   2) 任何一次调用异常都会直接 HardFault，而它换来的只是一行诊断字。
 *
 * 打点本身（g_bt_diag_step / g_bt_diag_tick）永远开着，零开销，
 * 调试器可以直接 watch 这两个变量 —— 多数情况下这就够了，
 * 不需要往屏幕上画。需要看屏时把它改成 1 重编译即可。
 *========================================================================*/
#ifndef BT_DIAG_DRAW
#define BT_DIAG_DRAW  0
#endif

/*********************************************************************
 * @fn      bt_mark
 * @brief   记录一个诊断打点。**绝对不做任何屏幕操作。**
 *
 * 【2026-10-14 关键修复 —— 这就是 6<->23 循环的真凶】
 *
 * 原来这个函数在这里直接 TFT_FillRect() + TFT_Printf() 画到屏上，
 * 而它是被 wch_state_cb() 调用的 —— 也就是**在 TMOS 的 GAP 状态回调
 * 上下文里**。一次连接建立过程要打 20/21/22/23 四个点，断开再打 6，
 * 一轮就是 5 次。
 *
 * 单次开销（按 Main.c 里实测的 487 px/ms 折算）：
 *     每次 bt_mark 刷两张 240x17 的矩形 = 8160 px -> 约 17ms
 *     一轮 5 次                            -> 约 84ms
 *
 * 而 BLE 的连接间隔只有 7.5~11.25ms（BTWCH_CONN_INT_MIN/MAX = 6/9）：
 * 一次 84ms 的阻塞 = 整整错过 7~11 个连接事件。主机收不到从机响应，
 * 监督超时（3000ms）一到就发 GAP_LINK_TERMINATED —— 于是
 *     6 -> 20 -> 21 -> 22 -> 23 -> 6 -> ...
 * 无限循环。
 *
 * 换句话说：**打点本身把它要诊断的东西给弄坏了**，而且越是"连接成功"
 * 走得越远，阻塞越久，断得越快，所以必然是这个循环形状。
 *
 * 现在 bt_mark() 只写两个 volatile 变量（纳秒级），刷屏交给主循环的
 * BT_WCH_DrawDiag() —— 那是主循环自己的上下文，阻塞只影响自己，
 * 不会在协议栈回调里制造 84ms 空洞。
 *********************************************************************/
/*==========================================================================
 * 【2026-10-14 关键修复：bt_mark() 就是蓝牙连不上的真凶】
 *
 * 本函数每次要画两块 240x17 的区域（各 4080 像素）+ 两次 TFT_Printf。
 * 而连接建立的路径上连着调了四次：bt_mark(20) -> 21 -> 22 -> 23。
 * 四次加起来 130~140ms —— 正好就是现场量到的 W=141ms。
 *
 * 后果是致命的：
 *   主机连上 -> 设备画 130+ms -> 这期间 BLE 协议栈完全得不到服务
 *   -> 主机 ATT 服务发现超时 -> 拆链 -> 设备 bt_mark(6) 再画一次
 *   -> 重播 -> 主机回连 -> 无限循环。
 *
 * 也就是说这个诊断语句在【喂养】这个 bug：每次连接都被它堵死，
 * 所以永远连不上。这也是为什么前面把 2.4G / 栈 / 连接参数 / 绑定 /
 * 射频档位全排除掉之后现象依旧。
 *
 * 现在加开关：正式固件 BT_STEP_DIAG=0，一次 TFT 操作都不做
 * （连 TFT_FillRect 都不调），排查时再置 1。
 *========================================================================*/
#ifndef BT_STEP_DIAG
#define BT_STEP_DIAG   0        /* 0 = 不画（正式固件）；1 = 画 BT step 诊断 */
#endif

static void bt_mark(uint8_t n)
{
    /* 【2026-10-14 修正】原来这两行被写在 #if (BT_STEP_DIAG) 里面，而正式固件
     * BT_STEP_DIAG=0 —— 于是 g_bt_diag_step **永远是 0**：主循环的
     * BT_WCH_DrawDiag() 和卡死看门狗读到的都是死值，等于白装。
     * 现在赋值无条件执行（两个 volatile 写，纳秒级），只有画屏才受开关控制。 */
    g_bt_diag_step = n;
    g_bt_diag_tick++;

#if (BT_STEP_DIAG)
    (void)n;
#else
    (void)n;
#endif
}

/*********************************************************************
 * @fn      BT_WCH_DrawDiag
 * @brief   把诊断打点画到屏底。**只能在主循环里调**，绝不能在协议栈
 *         回调里调（理由见 bt_mark 的注释）。
 *********************************************************************/
void BT_WCH_DrawDiag(void)
{
#if (BT_DIAG_DRAW)
    /* 只在打点变化时重画：绘制本身有开销，而这一行没人需要看动画 */
    static uint8_t s_shown = 0xFFu;
    uint8_t st = g_bt_diag_step;

    if (st == s_shown)
    {
        return;
    }
    s_shown = st;

    /* [P3] y=200 -> 118。面板旋转后是 240x135，y=200 在屏幕下方 65 行处，
     * 这个诊断从第一天起就没画在屏上，所以"卡在哪一步"一直看不到。 */
    TFT_FillRect(0, 118, 240, 17, 0x0000u);
    TFT_SetTextSize(1);
    TFT_SetTextColorBg(ST77XX_YELLOW, 0x0000u);
    TFT_SetCursor(4, 121);
    /* BT step N  卡在哪一步（20/21/22/23 = 连接建立路径，6 = 断开）
     * FWxx       构建号，确认烧的是新版
     * SK xxxx    栈高水位，逼近 2048 就是栈快满了 */
    TFT_Printf("BT step %u FW" BT_FW_TAG " SK%u",
               (unsigned)st, (unsigned)g_stack_hw);

    /* 第二行：档位切换计数。no/kb 在涨 = 射频在 KBD<->NONE 之间来回切，
     * 那样 BLE 必断。 */
    {
        extern volatile uint32_t g_rc_none_cnt;
        extern volatile uint32_t g_rc_kbd_cnt;
        extern volatile uint32_t g_rc_rc_cnt;

        TFT_FillRect(0, 100, 240, 17, 0x0000u);
        TFT_SetTextSize(1);
        TFT_SetTextColorBg(0x07FFu, 0x0000u);
        TFT_SetCursor(4, 103);
        TFT_Printf("no%u kb%u rc%u",
                   (unsigned)g_rc_none_cnt,
                   (unsigned)g_rc_kbd_cnt,
                   (unsigned)g_rc_rc_cnt);
    }
#endif /* BT_DIAG_DRAW */
}

uint8_t g_bt_addr[6] = {0};        /* 显示用的蓝牙地址 */

/* ★ 用自定义 MAC，避开 GetMACAddress()/ROM 信息区（有些批次读它会跑飞复位） */
const uint8_t MacAddr[6] = { 0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02 };

/*==========================================================================
 * HID 键盘需要的参数（全部照官方 HID_Keyboard\APP\hidkbd.c 的取值）
 *========================================================================*/
/* 连接参数：intervalMin/Max 单位 1.25ms，latency 单位"跳过的事件数"，timeout 单位 10ms
 *
 * 【修复】LATENCY 从 20 降到 4。
 *
 * 原因：latency=20 表示外设可以连续跳过 20 个连接事件不响应，
 * 配合 interval 6~9（7.5~11.25ms），理论最长静默期可达
 *     20 * 11.25ms ≈ 225ms
 * 部分主机（尤其 Windows 的 BLE 栈）对高 latency 外设支持很差，
 * 可能判定链路不稳定而主动断开，且重连困难。
 *
 * latency=4 时最长静默期 ≈ 45ms，对 HID 键盘的响应性完全够用
 * （人手按键间隔远大于此），同时仍保留可观的省电收益。 */
#define BTWCH_CONN_INT_MIN      6       /* 官方 GGS_PERI_CONN_PARAM_ATT */
#define BTWCH_CONN_INT_MAX      9
#define BTWCH_CONN_LATENCY      4
#define BTWCH_CONN_TIMEOUT      0x012C

#define BTWCH_PASSCODE          0
#define BTWCH_MITM              FALSE
/*==========================================================================
 * 【2026-10-14 已恢复为 TRUE】绑定 = 开
 *
 * 背景（保留记录，避免再走一遍弯路）：
 * 这项曾是排查"连上(23) -> 立刻断(6)"循环时的 A/B 测试档，被临时关掉，
 * 注释里写明"测完改回 TRUE"，但一直没改 —— 于是后续所有测试都跑在
 * "每次连接都要重新配对"这个非正常配置上。
 *
 * 关掉绑定为什么会让问题变形：
 *   * 主机每次建立链路都要走一遍配对（SMP），而 WCH 的配对回调里会
 *     **同步写 SNV 到 EEPROM**（cfg.writeFlashCB = Lib_Write_Flash，
 *     擦写 4KB 整块）。这几十毫秒里 TMOS 完全得不到服务。
 *   * 于是"闪 BLE 的次数"这个观测量本身被改变了：多一轮配对 = 多一次
 *     长阻塞 = 观察到的循环次数与真实故障次数对不上。
 *   * 现场表现就是"闪 3 次 / 闪 7 次"这种数不稳定的假象 —— 用它来判断
 *     改动是否有效是不可靠的。
 *
 * 现在恢复成产品配置（TRUE，与官方 HID_Keyboard 例程一致）。
 * 真正的 6<->23 根因已定位并修掉，见 ui_transport.c 与本文件的
 * s_adv_wanted / BT_WCH_RestorePhy 注释。
 *
 * 如果将来要再做这个 A/B，必须同时记录：SW 版本、绑定档位、连接参数，
 * 否则测出来的次数没法互相比较。
 *========================================================================*/
/* 【2026-10-14】改回 TRUE 会重新打开 SNV 写这条路径，风险已量化：
 *
 *   cfg.writeFlashCB = Lib_Write_Flash
 *     -> Lib_Write_Flash_592A()   【栈上 FLASH_BUF[256] = 288 B 栈帧】
 *        -> EEPROM_READ  4KB
 *        -> EEPROM_ERASE 4KB   <-- 同步阻塞，协议栈此时得不到服务
 *        -> EEPROM_WRITE 256B
 *
 * 它在**配对回调上下文**里跑，而这条路径此前从未在"绑定开启"状态下
 * 被本工程验证过（BONDING 关着的那段时间根本没走到这里）。
 * 直接改回 TRUE 等于第一次启用它 —— 如果卡死与它有关，改回后只会更糟。
 *
 * 所以这里改成一个可一行切换的宏：先按 TRUE 测（产品配置），
 * 若又卡死，把它改回 0 即可立刻排除本项，不用再改代码。
 * 同时把栈上 256B 数组挪到静态区，从根上消掉这条路径的栈风险
 * （见 MCU.c 的 s_snv_buf）。
 */
#ifndef BTWCH_BONDING
#define BTWCH_BONDING           1      /* 1=开绑定(产品) 0=关绑定(排查用) */
#endif
#define BTWCH_IO_CAP            GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT
#define BTWCH_BATT_CRITICAL     6       /* 低于此百分比算"电量危急" */

/*==========================================================================
 * 广播数据
 *========================================================================*/
/* 广播包里只放 Flags，名字放"扫描应答"里（这样名字能放全，也省广播包空间） */
static uint8_t s_adv_data[] =
{
    0x02, GAP_ADTYPE_FLAGS,
    GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,
    /* ★ 名字也直接放进广播包：不扫描应答的手机/电脑才能看到设备 */
    0x07, GAP_ADTYPE_LOCAL_NAME_COMPLETE, 'C', 'H', '5', '9', '2', 'X',
    /* 外观：HID 键盘（安卓设置里会显示对应图标） */
    0x03, GAP_ADTYPE_APPEARANCE, 0xC1, 0x03,
};

/* 扫描应答：名字 + 连接间隔范围 + 服务 UUID。
 * ★ 服务 UUID 这一段是"能不能被当成 HID 键盘"的关键之一：
 *   主机在扫描阶段就靠它判断这是键盘，不能省。
 *   （官方 HID_Keyboard 例程放在 scanRspData 里，照抄） */
static uint8_t s_scan_rsp[] =
{
    /* ---- 完整名字 ---- */
    0x07,
    GAP_ADTYPE_LOCAL_NAME_COMPLETE,
    'C', 'H', '5', '9', '2', 'X',

    /* ---- 从设备连接间隔范围 ---- */
    0x05,
    GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
    LO_UINT16(BTWCH_CONN_INT_MIN), HI_UINT16(BTWCH_CONN_INT_MIN),
    LO_UINT16(BTWCH_CONN_INT_MAX), HI_UINT16(BTWCH_CONN_INT_MAX),

    /* ---- 服务 UUID：HID(0x1812) + 电池(0x180F) ---- */
    0x05,
    GAP_ADTYPE_16BIT_MORE,
    LO_UINT16(HID_SERV_UUID),  HI_UINT16(HID_SERV_UUID),
    LO_UINT16(BATT_SERV_UUID), HI_UINT16(BATT_SERV_UUID),

    /* ---- 发射功率 0dBm ----
     * AD 类型必须用 SDK 的 GAP_ADTYPE_POWER_LEVEL(0x0A)。
     * 原先这里自定义了 GAPADTYPE_POWER_LEVEL_0 = 0，导致这一段变成
     * "长度=2, AD类型=0x00"，而 0x00 是未定义/保留的 AD 类型，会让部分
     * 手机和 PC 的扫描解析器丢弃整个 scan response。 */
    0x02,
    GAP_ADTYPE_POWER_LEVEL,
    0x00
};

/* GATT 里的设备名（手机/电脑配对后看到的名字） */
static const uint8_t s_dev_name[GAP_DEVICE_NAME_LEN] = BT_WCH_NAME;

/*==========================================================================
 * 内部状态
 *========================================================================*/
static uint8_t  s_task_id;
static uint16_t s_conn_handle = GAP_CONNHANDLE_INIT;
static bool     s_advertising;
static bool     s_connected;

/* == 【2026-10-14】断开原因码 + 实际协商到的连接参数 =====================
 * 追了十几轮"连上就断"，一直只打了 step 6（知道"断了"），
 * 却从没读过 gapTerminateLinkEvent_t 里的 reason —— 而 reason 正是
 * 划分责任的那一刀：
 *   0x08 Connection Timeout        设备侧没跟上（射频/时序/主循环阻塞）
 *   0x13 Remote User Terminated    主机主动断（服务发现/配对不满意）
 *   0x16 Connection Terminated by Local Host   本机自己断（代码有鬼）
 *   0x3B Unacceptable Conn Params  协议参数不被接受
 *   0x05 Authentication Failure    绑定密钥对不上
 * 同时把 linkCmpl 里主机实际给过来的 interval/timeout 也存下来 ——
 * 之前只对了自己发的广播参数，没验证过协商结果。
 * ==================================================================== */
static uint8_t  s_disc_reason   = 0u;   /* 最近一次断开原因码 */
static uint8_t  s_conn_interval = 0u;   /* 协商到的连接间隔（单位 1.25ms） */
static uint16_t s_conn_timeout  = 0u;   /* 协商到的监督超时（单位 10ms） */
static uint8_t  s_param_upd_st  = 0xFFu;/* 主机对参数更新请求的应答状态 */

uint8_t  BT_WCH_LastDiscReason(void) { return s_disc_reason; }
uint8_t  BT_WCH_ConnInterval(void)   { return s_conn_interval; }
uint16_t BT_WCH_ConnTimeout(void)    { return s_conn_timeout; }
uint8_t  BT_WCH_ParamUpdStatus(void){ return s_param_upd_st; }


static uint16_t wch_process_event(uint8_t task_id, uint16_t events);
static void     wch_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent);

/* 角色回调（状态变化 + RSSI），配对回调先给空 */
/* 【修复 BLE-2】GAPRole 回调改由 hiddev 持有（见本文件 BT_WCH_Init() 里
 * 的说明），应用层不再自己调 GAPRole_PeripheralStartDevice()，
 * 所以 s_role_cbs / s_bond_cbs / wch_rssi_cb 一并不再需要。
 * 应用层收状态改走 BT_WCH_OnGapState()（hidDevCB_t.pfnStateChange）。
 *
 * 将来若真要自己拿 RSSI：把 hidDev_PeripheralCBs 里的 pfnRssiRead 换掉，
 * 或在 hiddev.h 的 hidDevCB_t 里加一个 RSSI 回调，不要回到"自己调
 * StartDevice"的老路上去 —— 那会再次把 hiddev 的状态机顶掉。 */

/*==========================================================================
 * 广播开关
 *========================================================================*/
/*==========================================================================
 * [P3] BLE 协议栈"是否已经初始化"的门控
 *
 * 为什么需要：Main.c 的 radio_enter_none() / radio_enter_rc() 都会无条件
 * 调 BT_WCH_StopAdvertising()，而 radio_demand_of() 对 home/game/setting
 * 页返回 RADIO_NONE——也就是说，只要用户从没进过键盘页（kbd/kbset/keyedit），
 * s_bt_inited 就一直是 0，BT_WCH_Init() 从未跑过，此时
 *     GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, ...)
 * 作用在一个完全冷的 TMOS/GAPRole 状态上，会直接跑飞。
 * radio_enter_kbd() 的注释早就警告过"未初始化时协议栈的
 * GAPRole_SetParameter 会直接跑飞"，但 Stop 一侧没人守。
 *
 * 现象：进遥控页（或空闲到期进睡眠）时还没连上 BLE 就死机。
 *========================================================================*/
static uint8_t s_stack_inited = 0u;

/*==========================================================================
 * 【2026-10-14 修复：连上(23) 立刻断(6) 循环】
 *
 * 原来 wch_advert() 只看"协议栈建了没"(s_stack_inited)。可协议栈建好
 * **不等于 GAPRole 设备已经启动** —— 按官方架构，启动设备是 hiddev 的事：
 *     HidDev_Init() -> tmos_set_event(START_DEVICE_EVT)
 *     -> 等 TMOS 跑起来 -> GAPRole_PeripheralStartDevice()
 * 而 BT_WCH_Init() 末尾（和 radio_enter_kbd() 里紧接着那次）
 * **在那个事件之前**就把 GAPROLE_ADVERT_ENABLED 置了 1。
 *
 * 本文件 line 156 那段注释早就警告过这件事：
 *     "GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, ...) 作用在一个
 *      完全冷的 TMOS/GAPRole 状态上，会直接跑飞"
 * —— 当时只给 Stop 一侧加了 s_stack_inited 门，Start 一侧没加。
 *
 * 后果正是现场现象：广播以一份"内部状态不一致"的样子发出去，
 * **主机能连上（走到 step 23），但随即发现链路不对又立刻拆掉（step 6）**，
 * 然后重来 —— 6 <-> 23 反复横跳。
 *
 * 修法：再加一道 s_dev_started 门，只有 GAP 状态回调真的进了
 * GAPROLE_STARTED（= 设备起来了）之后，才允许动广播开关；
 * 并且在那一步主动把广播开起来（hiddev 不会再帮我们开）。
 *========================================================================*/
static uint8_t s_dev_started = 0u;

/*==========================================================================
 * 【2026-10-14】"应用层想不想要广播"标志 s_adv_wanted
 *
 * 协议栈自己有三条路径会**重新打开**广播：
 *     GAPROLE_STARTED（设备刚起来）
 *     GAPROLE_WAITING（非断链事件：广播超时/结束 -> 该再播了）
 *     GAPROLE_WAITING + GAP_LINK_TERMINATED_EVENT（断链后重播）
 * 另外 BT_WCH_RestorePhy() 交接射频后也会 wch_advert(1)。
 *
 * 以前本文件只有"开/关"这个动作，没有"意图"这个概念，于是仲裁器
 * （ui_transport.c）在 a/b 阶段关掉的广播，会被上面任意一条路径复活，
 * 且没人能再关掉 —— 这正是"插上 USB 蓝牙还在广播"的直接成因。
 *
 * 现在把意图独立成标志：只有 BT_WCH_StartAdvertising() 置 1、
 * BT_WCH_StopAdvertising() 置 0，wch_advert_auto() 才允许重播。
 * 广播的**唯一所有者**从此是传输层仲裁器。
 *========================================================================*/
static uint8_t s_adv_wanted = 0u;

static void wch_advert(uint8_t on)
{
    uint8_t en = (on != 0u) ? 1u : 0u;

    if (s_stack_inited == 0u)
    {
        return;                         /* 栈还没建，没什么可开关的 */
    }

    /* 设备还没被 hiddev 启起来之前，绝不去碰广播使能 */
    if (s_dev_started == 0u)
    {
        return;
    }

    (void)GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(en), &en);
}

/*********************************************************************
 * @fn      wch_advert_auto
 * @brief   协议栈请求"再广播一次"时统一走这里
 * @note    应用层（传输层仲裁器）说不要广播的时候，这里坚决不开。
 *         绝不能直接调 wch_advert(1u) —— 那等于绕过仲裁器。
 *********************************************************************/
static void wch_advert_auto(void)
{
    if (s_adv_wanted != 0u)
    {
        wch_advert(1u);
    }
}

void BT_WCH_StartAdvertising(void)
{
    s_adv_wanted = 1u;
    wch_advert(1u);
}

void BT_WCH_StopAdvertising(void)
{
    s_adv_wanted = 0u;
    wch_advert(0u);
}

/*==========================================================================
 * 协议栈状态回调
 *========================================================================*/
static void wch_state_cb(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    uint8_t st;
    uint8_t opcode;

    st     = (uint8_t)(newState & GAPROLE_STATE_ADV_MASK);
    opcode = (pEvent != 0) ? pEvent->gap.opcode : 0xFFu;

    /*========================================================================
     * 【修复：BLE 反复连接 / 断开】
     *
     * 原来这里把**所有** GAPROLE_WAITING 都当成"断开了"，无条件做：
     *     BtGatt_ConnChange(REMOVED) -> HidDev_Close() / BT_OnDisconnected()
     *     s_connected = false;
     *     wch_advert(1u);
     *
     * 但 GAPROLE_WAITING 的官方定义是"已启动、当前没在广播、等待再次广播"
     * （见 CH59xBLE_LIB.h: "Device is started but not advertising, is in
     *   waiting period before advertising again"），进入它的原因有三种，
     * 官方例程 HID_Keyboard\APP\hidkbd.c 的 hidEmuStateCB() 是按
     * pEvent->gap.opcode **分开**处理的：
     *     GAP_END_DISCOVERABLE_DONE_EVENT  广播被(主动/超时)结束
     *     GAP_LINK_TERMINATED_EVENT        连接真的断了
     *     GAP_LINK_ESTABLISHED_EVENT       广播超时
     *
     * 把后两种也当断开，后果是致命的：
     *  1) s_connected 被清成 false。而 Main.c 的 want_bt_phy() 正是靠
     *     BT_WCH_IsConnected() 来"无条件把射频留给 BLE"（Main.c 里写得很清楚：
     *     连接期间一旦让 2.4G 抢走 PHY，RF_Config() 会重写
     *     accessAddress / 频点 / CRC / LLEMode，正在进行的连接事件被物理层
     *     摧毁 -> 蓝牙断链）。于是仲裁一旦走到 SCAN_RF24 阶段，
     *     2.4G 就把链路打断 -> 主机重连 -> 再打断 …… 就是"疯狂连接/断开"。
     *  2) 会误调 BT_OnDisconnected()，上层的通道状态跟着抖。
     *
     * 现在按 opcode 分开处理：只有 GAP_LINK_TERMINATED_EVENT 才算真断开。
     *======================================================================*/
    switch (st)
    {
        case GAPROLE_STARTED:
            /* 【2026-10-14】设备真的起来了 —— 到这里广播开关才是安全的。
             * BT_WCH_Init() 末尾和 radio_enter_kbd() 里那两次 wch_advert(1)
             * 都会因为 s_dev_started==0 被挡掉，所以这里必须主动补开一次。 */
            s_dev_started = 1u;
            s_advertising = false;
            s_connected   = false;
            g_bt_step = 7; bt_mark(7);

#if (BT_CLEAR_BONDS_ON_BOOT)
            /* 【一次性救砖】开机清掉设备侧绑定，配合主机端『忘记设备』
             * 把两端拉回一致。用完把 BT_CLEAR_BONDS_ON_BOOT 改回 0。 */
            BT_WCH_EraseAllBonds();
            g_bt_step = 8; bt_mark(8);
#endif
            wch_advert_auto();              /* 设备起来了；要不要播由上层定 */
            break;

        case GAPROLE_ADVERTISING:
            s_advertising = true;               /* 正在广播，手机应该能看到 CH592X */
            break;

        case GAPROLE_CONNECTED:                 /* 有人连上来了 */
        case GAPROLE_CONNECTED_ADV:             /* 连上了、同时还在广播（有绑定信息时） */
            if ((s_connected == false) && (pEvent != 0))
            {
                /* 【2026-10-14 诊断】连接建立路径分四步打桩。
                 * 如果屏幕停在 20/21/22/23 某一档，就说明卡在那一步 ——
                 * 不用再猜了。排完这些 bt_mark 可以删掉。 */
                g_bt_step = 20; bt_mark(20);

                s_connected   = true;
                s_conn_handle = pEvent->linkCmpl.connectionHandle;
                /* 【2026-10-14】主机实际给到的连接参数 */
                s_conn_interval = (uint8_t)pEvent->linkCmpl.connInterval;
                s_conn_timeout  = pEvent->linkCmpl.connTimeout;

                g_bt_step = 21; bt_mark(21);

                /* 让各 GATT 服务记下连接句柄（CCCD 订阅状态要靠它维护） */
                BtGatt_ConnChange(s_conn_handle, LINKDB_STATUS_UPDATE_NEW);

                g_bt_step = 22; bt_mark(22);

                /* 告诉上层：连上了（地址由栈负责，白名单判断留给上层/后续加） */
                {
                    static const uint8_t unknown[6] = { 0, 0, 0, 0, 0, 0 };
                    BT_OnConnected(unknown);
                }

                g_bt_step = 23; bt_mark(23);
            }
            s_advertising = (st == GAPROLE_CONNECTED_ADV) ? true : false;
            break;

        case GAPROLE_WAITING:
            if (opcode == GAP_LINK_TERMINATED_EVENT)
            {
                /* ---- 真的断了：清状态 + 通知上层 + 重新广播 ---- */
                if (s_connected != false)
                {
                    /* 只维护本模块/服务的连接句柄。
                     * 服务侧的完整清理（Batt / ScanParam / hiddev）hiddev 自己
                     * 在 hidDevDisconnected() 里已经做过了，这里不要重复做，
                     * 更不能去调 HidDev_Close() —— 那会拿着还没更新的
                     * hidDevGapState 去 GAPRole_TerminateLink()。 */
                    BtGatt_ConnChange(s_conn_handle, LINKDB_STATUS_UPDATE_REMOVED);
                    BT_OnDisconnected();
                }

                /* 【2026-10-14】抓断开原因码：这一位决定"谁的错"
                 *
                 * 【2026-10-15 实测教训 —— 0x13 那一串】
                 * 现场报"反复断连"时，读出来的就是 0x13（Remote User
                 * Terminated，主机主动发拆链命令）；配合 B（开机次数）不涨、
                 * S 是上电复位，说明压根没重启，是主机在加密/HID 建立
                 * 阶段主动拆链。最后查到真凶是 **电脑端留着旧的蓝牙
                 * 配对记录**（Windows 蓝牙设置里那台 "CH592X" 没删）：主机拿
                 * 自己的旧密钥来握手，和键盘 SNV 里的对不上，就直接断。
                 * => 以后看到 R13：先让用户在电脑上"删除设备"再重连，
                 *    别急着改固件。反过来如果是我们这边留着旧绑定
                 *    （电脑删了、键盘还记着），把 BT_CLEAR_BONDS_ON_BOOT
                 *    临时改成 1 刷一次、开机清空后再改回 0。
                 */
                s_disc_reason = (uint8_t)pEvent->linkTerminate.reason;
                s_connected   = false;
                s_conn_handle = GAP_CONNHANDLE_INIT;
                s_advertising = false;
                g_bt_step = 6; bt_mark(6);
                wch_advert_auto();              /* 断开后重播（上层要才播） */
            }
            else
            {
                /* ---- 只是广播结束 / 广播超时：连接没断 ----
                 * 不动 s_connected、不动 GATT、不喊"断开"。
                 * 只有在确实没连接的时候才需要把广播重新拉起来，
                 * 否则会把 hiddev 在连接建立时主动关掉的广播又打开。 */
                s_advertising = false;

                if (s_connected == false)
                {
                    wch_advert_auto();
                }
            }
            break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      BT_WCH_OnGapState
 * @brief   【修复 BLE-2】把 GAP 状态转交给应用层
 *
 * GAPRole 回调现在由 hiddev（官方 HID profile）持有，它在自己的
 * hidDevGapStateCB() 里处理完状态机后，会回调 hidDevCB_t.pfnStateChange；
 * 参数与原来 gapRolesCBs_t.pfnStateChange 完全一致，所以这里只是把
 * wch_state_cb 暴露出去。
 *
 * 为什么必须这样：hiddev 用 hidDevGapState / gapConnHandle /
 * hidDevConnSecure 三个内部变量决定"能不能发 HID 报文"。应用层一旦自己
 * 调 GAPRole_PeripheralStartDevice()，就把 hiddev 的回调顶掉 —— 它那三个
 * 变量永远停在初始值，HidDev_Report() 只会返回 bleNotReady，
 * BLE 键盘一个字符也发不出去。
 *********************************************************************/
void BT_WCH_OnGapState(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    wch_state_cb(newState, pEvent);
}

/*==========================================================================
 * 本应用自己的 TMOS 任务（只处理系统消息，没有别的活）
 *========================================================================*/
static uint16_t wch_process_event(uint8_t task_id, uint16_t events)
{
    if ((events & SYS_EVENT_MSG) != 0u)
    {
        uint8_t *pMsg = tmos_msg_receive(task_id);

        if (pMsg != 0)
        {
            (void)tmos_msg_deallocate(pMsg);
        }
        return (uint16_t)(events ^ SYS_EVENT_MSG);
    }
    return 0u;
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
/*==========================================================================
 * 【2026-10-14】清掉设备侧所有绑定信息
 *
 * 为什么必须要有这个：
 *   反复烧录如果整片擦除 Flash，**设备端 SNV 里的绑定密钥就没了**，
 *   而手机/PC 还存着旧密钥 —— 每次回连加密都对不上 -> 主机拆链 ->
 *   设备重播 -> 主机回连 -> 死循环（现场 6<->23 反复横跳）。
 *
 *   原来工程**根本没有清绑定的入口**（grep 过：GAPBOND_ERASE_ALLBONDS /
 *   HIDDEV_ERASE_ALLBONDS 全工程只有注释提到，没有任何调用），
 *   所以一旦两端不一致就是永久性的，只能靠刷固件救。
 *
 * 走 HidDev_SetParameter(HIDDEV_ERASE_ALLBONDS) 而不是直接调
 * GAPBondMgr —— 前者会先把链路拆掉再擦，顺序更稳（见 hiddev.c 的实现）。
 *========================================================================*/
void BT_WCH_EraseAllBonds(void)
{
    if (s_dev_started == 0u)
    {
        return;                         /* 设备还没起来，碰不了 GAPBondMgr */
    }

    (void)HidDev_SetParameter(HIDDEV_ERASE_ALLBONDS, 0, 0);
}

/* 【2026-10-14】开机自动清一次绑定（一次性救砖用）。
 * 置 1 烧一次 -> 设备端绑定清空；同时把 PC/手机上的设备『忘记』，
 * 两端就回到干净状态，再改回 0 重烧。 */
#ifndef BT_CLEAR_BONDS_ON_BOOT
#define BT_CLEAR_BONDS_ON_BOOT   0
#endif

void BT_WCH_Init(void)
{
    g_bt_step = 1; bt_mark(1);
    {   /* 把 MAC 抄一份给屏显示用 */
        uint8_t i;
        for (i = 0; i < 6u; i++) { g_bt_addr[i] = MacAddr[5u - i]; }
    }
    CH59x_BLEInit();                            /* 协议栈初始化 */
    g_bt_step = 2; bt_mark(2);
    HAL_Init();                                 /* HAL（时钟/串口等） */
    g_bt_step = 3; bt_mark(3);
    s_task_id = TMOS_ProcessEventRegister(wch_process_event);
    g_bt_step = 4; bt_mark(4);
    /* 广播间隔 20~30ms：安卓"设置->蓝牙"用的是低占空比扫描，间隔小才容易被列出来 */
    (void)GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, 0x20);
    (void)GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, 0x30);

    (void)GAPRole_PeripheralInit();             /* 外设（从设备）角色 */

    /* 广播数据 + 扫描应答（名字 CH592X 就在这里） */
    (void)GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(s_adv_data), s_adv_data);
    (void)GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, sizeof(s_scan_rsp), s_scan_rsp);

    /* GATT 里的设备名，同样叫 CH592X */
    (void)GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, (void *)s_dev_name);

    /* ---- 配对/绑定（官方 HID_Keyboard 的取值）----
     * 键盘要能重连，所以必须开绑定；MITM 关掉（无输入无输出能力），
     * 否则主机会要求输 PIN，而这块板没有输入设备。 */
    {
        uint32_t passcode = BTWCH_PASSCODE;
        uint8_t  pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
        uint8_t  mitm     = BTWCH_MITM;
        uint8_t  ioCap    = BTWCH_IO_CAP;
        uint8_t  bonding  = BTWCH_BONDING;

        (void)GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(passcode), &passcode);
        (void)GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE,    sizeof(pairMode), &pairMode);
        (void)GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(mitm),     &mitm);
        (void)GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(ioCap),    &ioCap);
        (void)GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(bonding),  &bonding);
    }

    /* ---- 从设备连接间隔参数 ---- */
    {
        gapPeriConnectParams_t cp;

        cp.intervalMin = BTWCH_CONN_INT_MIN;
        cp.intervalMax = BTWCH_CONN_INT_MAX;
        cp.latency     = BTWCH_CONN_LATENCY;
        cp.timeout     = BTWCH_CONN_TIMEOUT;
        (void)GGS_SetParameter(GGS_PERI_CONN_PARAM_ATT, sizeof(cp), &cp);
    }

    /* ---- 电池服务的"电量危急"阈值 ---- */
    {
        uint8_t crit = BTWCH_BATT_CRITICAL;
        (void)Batt_SetParameter(BATT_PARAM_CRITICAL_LEVEL, sizeof(crit), &crit);
    }

    /* ---- ★ 挂上 GATT 服务（HID + 电池 + 设备信息 + 扫描参数）----
     * 这一句原来是**缺的**：不注册 HID 服务，主机连上来也看不到键盘，
     * 报文发出去没人收。这是 BLE 通道之前不能用的根因。 */
    BtGatt_Init();

    /* 启动外设角色（注册回调），然后开广播 */
    /* ★★ 【修复 BLE-2】这里**不要**再调 GAPRole_PeripheralStartDevice()！
     *
     * 官方架构（HID_Keyboard\APP\hidkbd_main.c）是：
     *     HidDev_Init()                       ← 内部 tmos_set_event(START_DEVICE_EVT)
     *     ...
     *     Main_Circulation() → TMOS 跑起来 → HidDev_ProcessEvent() 收到
     *     START_DEVICE_EVT → GAPRole_PeripheralStartDevice(hidDevTaskId,
     *                              &hidDevBondCB, &hidDev_PeripheralCBs)
     * 也就是说：**启动设备这件事由 hiddev 自己做**，应用层只调 HidDev_Init()。
     *
     * 为什么不能抢着调（本工程原来就是这么写的）：
     *   反汇编 libCH59xBLE.a 的 GAPRole_PeripheralStartDevice() 可见，它先
     *   GAPBondMgr_PeripheralRegister(pBondCB)，然后**只在 gapRole_state==INIT
     *   时**才把 pAppCallbacks 记下来。而 GAPRole_PeripheralInit() 刚把 state
     *   清成 INIT，本函数在 BT_WCH_Init() 里同步调用 —— 一定抢在 hiddev 的
     *   START_DEVICE_EVT（要等 TMOS 跑）之前，于是记下的是**本文件的**回调，
     *   hiddev 稍后再调就撞上 "state != INIT"，直接返回 0x11，什么也没注册。
     *   后果：hiddev 的 hidDevGapStateCB() 永远不会跑 → hidDevGapState 停在
     *   GAPROLE_INIT、gapConnHandle 停在 0 → HidDev_Report() 永远 bleNotReady
     *   （BLE 键盘发不出字），连接状态也无法同步给 hiddev。
     *
     * 现在的分工：
     *   状态通知  hiddev → hidDevCB_t.pfnStateChange → BT_WCH_OnGapState()
     *   启动设备  hiddev 的 START_DEVICE_EVT（等 TMOS 跑）→ 自己调 StartDevice
     *   广播开关  本文件的 wch_advert()（BT_WCH_StartAdvertising/StopAdvertising）
     */
    g_bt_step = 5; bt_mark(5);
    /* [P3] TMOS 从下一行起可以跑了（主循环里 radio_enter_kbd 返回之后
     * 才会真正调 BT_WCH_Process()，届时 hiddev 的任务会把设备启起来）*/
    s_stack_inited = 1u;

    g_bt_step = 6; bt_mark(6);

    /* 【2026-10-14】这里**不能**开广播：此刻 GAPRole 设备还没被 hiddev
     * 启起来（走的是 START_DEVICE_EVT），提前置 GAPROLE_ADVERT_ENABLED
     * 会让广播带着不一致的内部状态发出去 -> 主机连上就拆 -> 6<->23 循环。
     * 真正的开广播动作挪到了 wch_state_cb() 的 GAPROLE_STARTED 分支。 */
}

void BT_WCH_Process(void)
{
    if (s_stack_inited == 0u)
    {
        return;                         /* TMOS not up yet */
    }
    TMOS_SystemProcess();                       /* 协议栈干活的地方 */
}

/*********************************************************************
 * @fn      BT_WCH_SendHid
 * @brief   把 8 字节 boot 报文推给已连接的主机
 * @note    GATT 通知和 USB/2.4G 不同：对端没订阅（CCCD 没使能）时会失败，
 *          这是**正常现象**（还没完成配对/还没进输入框），不是 bug。
 *          连接参数没协商好时也会失败——上层每帧重试即可。
 *********************************************************************/
bool BT_WCH_SendHid(const uint8_t *report, uint8_t len)
{
    if ((report == 0) || (len < 8u) || (s_connected == false))
    {
        return false;
    }

    /* 【修复 P2-8】只有连接还不够：GATT 通知要主机**订阅**过（输入报文的
     * CCCD 使能）才发得出去。没订阅就发，协议栈会直接失败，
     * 白白占掉一帧，还会让上层"已发送"的判断失准。 */
    if (HidDev_IsReportSubscribed() == 0u)
    {
        return false;
    }

    return BtGatt_SendReport(report);
}

/* 【修复 P0-1】把射频从"2.4G 参数"切回 BLE 参数，并把广播重新拉起来。
 *
 * 修的是什么：原实现是 `RF_RoleInit(); BT_WCH_StopAdvertising();` ——
 * 函数名叫 RestorePhy，语义却是"让出射频 + 关广播"，本末倒置。调用方
 * （Main.c）只好在它后面再补一次 StartAdvertising() 才勉强能用，等于每次
 * 切回 BLE 通道都要"关-开"两拍，中间还夹着一次 RF_RoleInit，广播状态机
 * 到底停在哪一步全凭时序碰运气。
 *
 * 现在按函数名把语义摆正：
 *   1) RF_RoleInit()  —— 让协议栈重新接管射频（按栈内配置重写 LLE）
 *   2) 广播状态机"先清后开" —— RF_RoleInit 会把射频拉回 idle，栈里
 *      "正在广播"的状态跟着失效。不显式重启的话，状态机以为还在广播，
 *      射频却一个包都不发 —— 主机自然扫不到，这正是"广播状态正常但搜不到"
 *      的另一半原因。
 */
void BT_WCH_RestorePhy(void)
{
    if (s_stack_inited == 0u)
    {
        return;                         /* 栈都没建，谈不上"恢复" */
    }

    /*========================================================================
     * 【修复：连接期间绝不重初始化射频】
     *
     * 本函数的名字是"把射频交还给 BLE 协议栈"，实现里那句 RF_RoleInit()
     * 会按栈内配置**重写 LLE**（accessAddress / 频点 / CRC / LLEMode /
     * rfStatusCB）—— 对一条**正在使用中**的链路来说，这等于从物理层把它拆掉。
     * Main.c 的注释也警告过："这一步只在真的发生交接时做：BLE 连着的时候
     * 再跑 RF_RoleInit 会把正在用的连接带断"。
     *
     * 但"连接期间会不会发生 2.4G->BLE 的交接"这件事，光靠调用方自觉是不够的：
     *   * Main.c 的 want_bt_phy() 确实已经写了"连接中无条件判给 BLE"，
     *     所以正常路径下连接中不会有交接请求；
     *   * 可一旦哪天有人改了判据、或者状态被别处误清（例如 GAP 状态回调里
     *     把"广播结束"误当"断开"—— 本次一并修了），这里就会在连接中
     *     被调到，直接把用户的连接打断，表现就是"蓝牙疯狂连接/断开"。
     *
     * 所以这里加一道**本函数自己的**硬保护：只要链在路上，就不动射频。
     * 连接期间本来就没什么可"恢复"的 —— 广播不该开，PHY 也不需要重写。
     *======================================================================*/
    /*========================================================================
     * 【2026-10-14 修正】RF_RoleInit() 必须**无条件**执行。
     *
     * 原来这里是 `if (s_connected != false) return;` —— 理由是"连接期间
     * 重初始化射频会把链路从物理层拆掉"。但这个前提在本函数里不成立：
     * 它只被 radio_set_phy_owner() 在**射频归属真的发生 2.4G -> BLE 跳变**
     * 时调用，而跳变的前一步 HALRF_BtOwnsPhy(false) 已经执行过
     * halrf_ch592_apply() -> RF_Config()，把 accessAddress / 频点 / CRC /
     * LLEMode / rfStatusCB **全部改写成 2.4G 的参数**了。
     * 也就是说：链路在进入本函数之前就已经在物理层被打断，
     * "不动射频"保全的不是链路，只是一条死链。
     *
     * 现场表现：射频在 2.4G <-> BLE 之间来回交接时，BLE 反复"连上->立刻断"，
     * 因为 PHY 停在 2.4G 参数上再也回不去（owner 已经是 BLE，
     * radio_set_phy_owner() 的幂等判断不会再调本函数）。
     *
     * 所以：射频一定要还给协议栈；只有"要不要动广播"才看连接状态。
     *======================================================================*/
    (void)RF_RoleInit();

    if (s_connected != false)
    {
        /* 链路在（或刚断）：广播交给 hiddev 的状态机管，本函数不碰。 */
        return;
    }

    if (s_advertising != false)
    {
        wch_advert(0u);                 /* 先清：把状态机拉回 WAITING */
        s_advertising = false;
    }

    wch_advert_auto();                  /* 再开：上层要广播才真的开 */
}

bool BT_WCH_IsAdvertising(void) { return s_advertising; }
bool BT_WCH_IsConnected(void)   { return s_connected; }
uint16_t BT_WCH_GetConnHandle(void) { return s_conn_handle; }
const char *BT_WCH_GetName(void) { return BT_WCH_NAME; }

void BT_WCH_SetStep(uint8_t s) { g_bt_step = s; }

/******************* (C) CH592X BLE 从设备 *****************/
