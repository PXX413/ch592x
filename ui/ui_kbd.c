/**************************************************************************
 * ui_kbd.c —— 键盘页实现（三通道驱动 + 版面绘制）
 *
 * 这一版做的是"键盘页 + 通道仲裁 + 6KRO 报文 + 键值显示"，
 * 三个通道的**出口**通过 UiKbd_SetSender() 交给上层接线：
 *   USB   -> UsbKbd_SendReportNow()
 *   2.4G  -> Kb_BuildHid6() 后交给 hal_rf
 *   BLE   -> BT_HID_Flush()
 * 这样 PC 上能把仲裁时序、报文内容、版面坐标全部断言掉，
 * 真机上只需在 Main.c 里接三根线。
 **************************************************************************/

#include "ui_kbd.h"
#include "ui_kbd_ch.h"
#include "ui_widget.h"
#include "ui_kbd_font.h"     /* 专用笔画字体（不是点阵放大） */
#include "tft_gfx.h"
#include "key_matrix.h"
#include "macro.h"
#include "game_port.h"
#include "game_text.h"

static uint8_t s_mods;      /* 修饰键：6KRO 报文的第 0 字节 */

/* 额外键：编码器按键。非列表页（键盘页）单击时当"3x3 之外的按键"用
 * （需求 (六).2），默认敲一下回车 —— 和 keyboard 模块的默认表一致。 */
static uint8_t  s_extra_usage = UIK_EXTRA_USAGE_ENTER;

/* 【2026-10-13】脉冲键现在也可以来自映射表（编码器正转/反转/单击）：
 *   s_extra_mods = 脉冲期间要叠加到报文第 0 字节的修饰位（组合键脉冲用）。
 * 于是 Ctrl+C 这种 16 位映射值（0x0106）也能挂在编码器上。 */
static uint8_t  s_extra_mods;
static uint8_t  s_extra_down;       /* 1 = 这一帧报文里要带上它 */
static uint32_t s_extra_until_ms;   /* 到这个时刻就松开（脉冲宽度） */

/*==========================================================================
 * HID 用途码 -> 屏幕上的短名字
 *   字库里没有键名条目（font_texts.txt 未收录），所以这里用 ASCII 名字配
 *   UiW / Game_DrawAscii 画。等字库补了键名词条，把这个表换成 GTXT 编号即可。
 *========================================================================*/
const char *UiKbd_UsageName(uint8_t usage)
{
    switch (usage)
    {
        case 0x04u: return "A";     case 0x05u: return "B";
        case 0x06u: return "C";     case 0x07u: return "D";
        case 0x08u: return "E";     case 0x09u: return "F";
        case 0x0Au: return "G";     case 0x0Bu: return "H";
        case 0x0Cu: return "I";     case 0x0Du: return "J";
        case 0x0Eu: return "K";     case 0x0Fu: return "L";
        case 0x10u: return "M";     case 0x11u: return "N";
        case 0x12u: return "O";     case 0x13u: return "P";
        case 0x14u: return "Q";     case 0x15u: return "R";
        case 0x16u: return "S";     case 0x17u: return "T";
        case 0x18u: return "U";     case 0x19u: return "V";
        case 0x1Au: return "W";     case 0x1Bu: return "X";
        case 0x1Cu: return "Y";     case 0x1Du: return "Z";
        case 0x1Eu: return "1";     case 0x1Fu: return "2";
        case 0x20u: return "3";     case 0x21u: return "4";
        case 0x22u: return "5";     case 0x23u: return "6";
        case 0x24u: return "7";     case 0x25u: return "8";
        case 0x26u: return "9";     case 0x27u: return "0";
        case 0x28u: return "Enter"; case 0x29u: return "Esc";
        case 0x2Au: return "Bksp";  case 0x2Bu: return "Tab";
        case 0x2Cu: return "Space"; case 0x39u: return "Caps";
        case 0x3Au: return "F1";    case 0x3Bu: return "F2";
        case 0x3Cu: return "F3";    case 0x3Du: return "F4";
        case 0x3Eu: return "F5";    case 0x3Fu: return "F6";
        case 0x40u: return "F7";    case 0x41u: return "F8";
        case 0x42u: return "F9";    case 0x43u: return "F10";
        case 0x44u: return "F11";   case 0x45u: return "F12";
        case 0x49u: return "Ins";   case 0x4Au: return "Home";
        case 0x4Bu: return "PgUp";  case 0x4Cu: return "Del";
        case 0x4Du: return "End";   case 0x4Eu: return "PgDn";
        case 0x4Fu: return "Right"; case 0x50u: return "Left";
        case 0x51u: return "Down";  case 0x52u: return "Up";
        case 0x59u: return "Num1";  case 0x5Au: return "Num2";
        case 0x5Bu: return "Num3";  case 0x5Cu: return "Num4";
        case 0x5Du: return "Num5";  case 0x5Eu: return "Num6";
        case 0x5Fu: return "Num7";  case 0x60u: return "Num8";
        case 0x61u: return "Num9";  case 0x80u: return "Vol+";
        case 0x81u: return "Vol-";
        /* 修饰键：HID 规定 0xE0~0xE7 报在报文第 0 字节，不进键数组 */
        case 0xE0u: return "LCtrl"; case 0xE1u: return "LShft";
        case 0xE2u: return "LAlt";  case 0xE3u: return "LGUI";
        case 0xE4u: return "RCtrl"; case 0xE5u: return "RShft";
        case 0xE6u: return "RAlt";  case 0xE7u: return "RGUI";
        default:    return "?";
    }
}

/*==========================================================================
 * 映射值拆两半
 *
 *   key_matrix 里的映射值是 16 位：高字节 = 修饰键位图，低字节 = 主键
 *   HID Usage ID（网页在线改映射存的就是这个格式，例如 Ctrl+C = 0x0106）。
 *   只认低 8 位的话组合键会被当成"没定义"丢掉，所以这里统一拆开：
 *   usage 进报文的键数组，mods 进报文第 0 字节。
 *
 *   另外 HID 规定 0xE0~0xE7 这 8 个用途码必须报在修饰字节里、不能塞进
 *   键数组，所以"单独一个 Ctrl 键"（映射值 0x00E0）也要转成位。
 *========================================================================*/
uint16_t UiKbd_MapValueOf(uint8_t key)
{
    if ((key < 1u) || (key > 9u))
    {
        return 0u;
    }

    return KEY_GetMapped(key);
}

uint8_t UiKbd_UsageModBit(uint8_t usage)
{
    if ((usage >= 0xE0u) && (usage <= 0xE7u))
    {
        return (uint8_t)(1u << (usage - 0xE0u));
    }

    return 0u;
}

uint8_t UiKbd_UsageOf(uint8_t key)
{
    /* 键数组里要用的主键用途码 = 映射值低字节；0 = 该键不进键数组 */
    return (uint8_t)(UiKbd_MapValueOf(key) & 0x00FFu);
}

uint8_t UiKbd_ModsOf(uint8_t key)
{
    uint16_t v = UiKbd_MapValueOf(key);

    /* 高字节的修饰位，再叠加"低字节是 0xE0~0xE7 时"该用途码对应的位 */
    return (uint8_t)(((v >> 8) & 0x00FFu) |
                     UiKbd_UsageModBit((uint8_t)(v & 0x00FFu)));
}

void UiKbd_BuildPressedText(char *buf, uint8_t buf_len)
{
    uint16_t map;
    uint8_t  i;
    uint8_t  n = 0u;

    if ((buf == 0) || (buf_len == 0u))
    {
        return;
    }

    buf[0] = '\0';
    map    = KEY_GetMap();

    for (i = 1u; i <= 9u; i++)
    {
        const char *nm;
        uint8_t     j;

        if ((map & (uint16_t)(1u << (i - 1u))) == 0u)
        {
            continue;
        }

        if (n != 0u)
        {
            buf[n++] = '+';
        }

        nm = UiKbd_UsageName(UiKbd_UsageOf(i));
        for (j = 0u; (nm[j] != '\0') && (n < (uint8_t)(buf_len - 1u)); j++)
        {
            buf[n++] = nm[j];
        }
    }

    buf[n] = '\0';
}

/* 取当前按下键的键名指针列表（供分排大字显示用） */
uint8_t UiKbd_BuildPressedList(const char *out[], uint8_t max_n)
{
    uint16_t map;
    uint8_t  i;
    uint8_t  n = 0u;

    if ((out == 0) || (max_n == 0u))
    {
        return 0u;
    }

    map = KEY_GetMap();

    for (i = 1u; i <= 9u; i++)
    {
        if ((map & (uint16_t)(1u << (i - 1u))) == 0u)
        {
            continue;
        }
        if (n >= max_n)
        {
            break;                  /* 排数上限：多的丢掉 */
        }

        out[n] = UiKbd_UsageName(UiKbd_UsageOf(i));
        n++;
    }

    return n;
}

/*==========================================================================
 * 6KRO 引导报文
 *   [0] 修饰键  [1] 保留 0  [2..7] 最多 6 个用途码（不足补 0）
 *========================================================================*/
uint8_t UiKbd_BuildHid6(uint8_t *out8, uint16_t keymap)
{
    uint8_t i;
    uint8_t n = 0u;

    if (out8 == 0)
    {
        return 0u;
    }

    for (i = 0u; i < UIK_HID_REPORT_LEN; i++)
    {
        out8[i] = 0u;
    }

    out8[0] = s_mods;               /* 修饰键：全局的 + 每个键各自的 */

    for (i = 1u; i <= 9u; i++)
    {
        uint16_t v;
        uint8_t  usage;
        uint8_t  mbit;

        if ((keymap & (uint16_t)(1u << (i - 1u))) == 0u)
        {
            continue;
        }

        v     = UiKbd_MapValueOf(i);
        usage = (uint8_t)(v & 0x00FFu);
        mbit  = UiKbd_UsageModBit(usage);

        /* 组合键：映射值高字节就是修饰位（Ctrl+C 存的是 0x0106） */
        out8[0] |= (uint8_t)((v >> 8) & 0x00FFu);

        /* 单独一个修饰键（0xE0~0xE7）：只进第 0 字节，不占 6 个键码槽位。
         * 注意不能写进 for 的 n<6 条件里：按满 6 个键之后再按 Ctrl，
         * 循环会提前退出，修饰位就丢了。 */
        if (mbit != 0u)
        {
            out8[0] |= mbit;
            continue;
        }

        /* Phantom Key 保护：6 键满了之后不再塞，只报前 6 个 */
        if (usage == 0u)
        {
            continue;
        }

        if (n < 6u)
        {
            out8[2u + n] = usage;
            n++;
        }
    }

    return (uint8_t)UIK_HID_REPORT_LEN;
}

/*==========================================================================
 * 通道
 *========================================================================*/

static ui_tr_t s_tr;
static uint16_t s_last_map;
static uint8_t  s_inited;
static uint32_t s_tr_last_ms;               /* 上次推进仲裁的真实毫秒 */

void UiKbd_Init(void)
{
    s_last_map        = 0u;
    s_mods            = 0u;
    s_extra_down      = 0u;
    s_extra_until_ms  = 0u;
    s_inited          = 1u;

    s_tr_last_ms      = Game_Millis();

    UiTr_Init(&s_tr);
}

void UiKbd_SetUsb(bool plugged, bool ready)
{
    UiTr_SetUsb(&s_tr, plugged, ready);
}

void UiKbd_SetRf24(bool alive)
{
    UiTr_SetRf24(&s_tr, alive);
}

void UiKbd_SetBle(bool ready)
{
    UiTr_SetBle(&s_tr, ready);
}

void UiKbd_SetSender(bool (*fn)(ui_transport_t tr, const uint8_t *rep, uint8_t len))
{
    UiTr_SetSend(&s_tr, fn);
}

void UiKbd_SetBroadcaster(void (*fn)(ui_transport_t tr, bool starting))
{
    UiTr_SetAction(&s_tr, fn);
}

void           UiKbd_SetModifier(uint8_t mods) { s_mods = mods; }
uint8_t        UiKbd_Modifier(void)             { return s_mods; }

/*==========================================================================
 * 额外键（编码器按键）
 *========================================================================*/
void UiKbd_SetExtraUsage(uint8_t usage)
{
    s_extra_usage = usage;

    if (usage == 0u)
    {
        s_extra_down = 0u;              /* 关掉功能时顺手松开 */
    }
}

bool UiKbd_ExtraIsDown(void)
{
    return (s_extra_down != 0u);
}

void UiKbd_PulseExtra(void)
{
    if (((s_extra_usage == 0u) && (s_extra_mods == 0u)) ||
        (UiTr_Connected(&s_tr) == false))
    {
        return;                         /* 没配键值 / 没通道：不发 */
    }

    s_extra_down     = 1u;
    s_extra_until_ms = Game_Millis() + (uint32_t)UIK_EXTRA_PULSE_MS;

    (void)UiKbd_Flush();                /* 按下那一帧 */
}

/*********************************************************************
 * @fn      UiKbd_PulseMapping
 * @brief   按一个 16 位映射值发一次"按下+松开"脉冲
 *
 * 映射值格式和九个键一样：高字节 = 修饰键位图，低字节 = HID Usage。
 * 例：0x0028 回车、0x0106 Ctrl+C、0x00E1 单独一个左 Shift。
 *
 * 用在编码器上：正转/反转/单击各取当前映射组里对应槽的值（KEY_ENC_*），
 * 值由网页通过 kmap_proto 0x16 写入、0x30 存 Flash，掉电不丢。
 * 值为 0 = 这个动作不按键（默认正反转就是 0，保持旧行为）。
 *********************************************************************/
void UiKbd_PulseMapping(uint16_t map_value)
{
    uint8_t usage = (uint8_t)(map_value & 0x00FFu);

    if ((usage == 0u) && (((map_value >> 8) & 0x00FFu) == 0u))
    {
        return;                         /* 没配键值：什么都不发 */
    }

    s_extra_mods = (uint8_t)((map_value >> 8) & 0x00FFu);

    /* 单独一个修饰键（0xE0~0xE7）：只报修饰位，不占 6 个键码槽位 */
    if ((usage >= 0xE0u) && (usage <= 0xE7u))
    {
        s_extra_mods |= (uint8_t)(1u << (usage - 0xE0u));
        usage = 0u;
    }

    s_extra_usage = usage;
    UiKbd_PulseExtra();
}

bool UiKbd_Flush(void)
{
    uint8_t rep[UIK_HID_REPORT_LEN];

    (void)UiKbd_BuildHid6(rep, KEY_GetMap());

    /* 额外键（编码器按键）塞进第一个空槽位：它不属于 3x3 映射，
     * 也不进修饰位；6 个槽位都被矩阵键占满时这一帧就不发它。 */
    if ((s_extra_down != 0u) && (s_extra_usage != 0u))
    {
        uint8_t i;

        for (i = 2u; i < (uint8_t)UIK_HID_REPORT_LEN; i++)
        {
            if (rep[i] == 0u)
            {
                rep[i] = s_extra_usage;
                break;
            }
        }
    }

    /* 脉冲期间还要把修饰位叠上去（组合键脉冲，如 Ctrl+C） */
    if ((s_extra_down != 0u) && (s_extra_mods != 0u))
    {
        rep[0] = (uint8_t)(rep[0] | s_extra_mods);
    }

    return UiTr_SendHid(&s_tr, rep, (uint8_t)UIK_HID_REPORT_LEN);
}

/* 只有"按键状态真的变了"且"通道已经连上"才发；否则不发。
 * 单独抽出来是为了让 PC 测试能按"上一次的键状态"直接驱动。 */
bool UiKbd_OnKeyState(uint16_t map)
{
    if (map == s_last_map)
    {
        return false;                       /* 没变化 -> 不发 */
    }

    /* 【修复】**先判有没有通道，再更新 s_last_map**。
     *
     * 原来是反的：先把 s_last_map 改成新图，然后才发现没通道。
     * 于是——
     *   未连接时按下 K1 : s_last_map 已被改成"按着 K1"
     *   连上后 K1 还在手上 : map == s_last_map -> 直接 return false
     *   松手                        : 发出一帧空报文
     * 结果从头到尾没有任何按键上报，表现就是"按了没反应"。
     *
     * 现在没通道就不动 s_last_map，等连上了，下一帧 map 仍不等于
     * s_last_map，自然就把当前按着的键发出去。 */
    if (!UiTr_Connected(&s_tr))
    {
        return false;                       /* 没通道 -> 不发，也不记 */
    }

    s_last_map = map;

    return UiKbd_Flush();
}

void UiKbd_Task(void)
{
    if (s_inited == 0u)
    {
        UiKbd_Init();
    }

    /* 【修复 §8-12】仲裁按"真实毫秒"推进。
     *
     * 原来是 UiTr_Tick(&s_tr, 1u)：把主循环一圈当成 1ms。可一圈里还跑了
     * USB / BLE / 2.4G，UI 每 20ms 还要刷一次屏，刷屏那一圈几毫秒到几
     * 十毫秒不等 —— 于是"USB 0.5s / 2.4G 1.5s"两档判定被成倍拉长，
     * 通道切换明显变慢。
     *
     * 现在用 Game_Millis()（TMR0 1ms 中断累加的真实毫秒）算差值。
     * 上限 100ms：正常卡顿照实累加，极端停顿（写 Flash、进休眠）时限幅，
     * 免得一次跨过好几个判定相位。 */
    {
        uint32_t now = Game_Millis();
        uint32_t d   = now - s_tr_last_ms;

        s_tr_last_ms = now;
        if (d == 0u)  { d = 1u; }       /* 同一毫秒内被调两次：仍推进 1ms */
        if (d > 100u) { d = 100u; }

        UiTr_Tick(&s_tr, (uint16_t)d);
    }

    /* 额外键的松开：脉冲宽度到了就补发一帧"没有它"的报文。
     * 不能按下后马上连发两帧 —— USB 一个时刻只挂得下一个 IN 报文，
     * 第二帧会被丢掉，主机那边就看到这个键一直按着。 */
    if ((s_extra_down != 0u) &&
        ((int32_t)(Game_Millis() - s_extra_until_ms) >= 0))
    {
        s_extra_down = 0u;
        s_extra_mods = 0u;
        (void)UiKbd_Flush();
    }

    /* 【2026-10-15】绑了宏的键不出原映射：宏键按下去只放宏那一串。
     * 屏蔽是按"当前组 + 此刻按着的键"实时算的，所以改绑定/换组立刻生效。 */
    (void)UiKbd_OnKeyState((uint16_t)(KEY_GetMap() & (uint16_t)(~Macro_BoundMask())));

    /* 【2026-10-15】键盘宏：扫按下沿触发 + 推进正在播放的宏。
     * 放在最后：宏每一步都是一次"额外键脉冲"，而脉冲的松开要等下一次
     * UiKbd_Task()（上面那一段）来补 —— 所以先让本帧的按键处理完。 */
    Macro_Task();
}

ui_transport_t UiKbd_Channel(void)     { return UiTr_Current(&s_tr); }
bool           UiKbd_Connected(void)   { return UiTr_Connected(&s_tr); }
const char    *UiKbd_ChannelText(void) { return UiTr_StateText(&s_tr); }
const char    *UiKbd_StateText(void)   { return UiTr_Connected(&s_tr) ? "OK" : "NO"; }
const ui_tr_t *UiKbd_Transport(void)   { return &s_tr; }

/*==========================================================================
 * 页面：绘制
 *========================================================================*/
/* 在右侧 panel_w 宽的区域内水平居中：不能用整屏 240 去算，
 * 否则文字会越过 x=189 那条分割线跑到左侧区去。 */
/*********************************************************************
 * @fn      kbd_draw_channel_top
 * @brief   顶部右上角显示当前通道状态：USB / 2.4G / BLE；没锁定就是 "--"
 *
 *   顶部 y=0..19 这一条原来只填了底色（代码注释里说留给"电量条"），
 *   现在把**通道状态**放它右边 —— 16px，比右栏里那行 8px 的字清楚得多，
 *   而且右栏只有 43px 宽，"2.4G" 在 16px 下要 48px，那里根本放不下。
 *
 *   颜色跟连接状态走：
 *       绿 = 已连接（仲裁器锁定在这个通道上）
 *       灰 = 还没锁定 / 没通道（这时文字是 "--"）
 *********************************************************************/
static void kbd_draw_channel_top(void)
{
    const char *s  = UiKbd_ChannelText();
    int16_t     w  = Game_AsciiWidth(s, 2u);          /* 16px */
    int16_t     x  = (int16_t)(UI_SCREEN_W - 6 - w);
    uint16_t    fg = UiKbd_Connected() ? (uint16_t)UI_COL_GREEN
                                       : (uint16_t)UI_COL_DIM;

    if (x < 0) { x = 0; }

    /* 先把这一小块擦回底色：状态字长度会变（"--" 24px、"2.4G" 48px），
     * 不擦的话短字盖不住长字，会留残影 */
    TFT_FillRect((int16_t)(UI_SCREEN_W - 72), 0, 72, 20, (uint16_t)UI_COL_BG);

    Game_DrawAscii(x, 2, s, 2u, fg, (uint16_t)UI_COL_BG);
}

static void kbd_draw_center_y(int16_t cy, const char *s, uint16_t fg, uint16_t bg)
{
    int16_t w = Game_AsciiWidth(s, 1u);

    /* 只清右栏这一条 8px 高的行（43x8 = 344 像素），不是整屏 */
    TFT_FillRect((int16_t)UIK_PANEL_X, (int16_t)(cy - 4), (int16_t)UIK_PANEL_W, 8, bg);

    Game_DrawAscii((int16_t)(UIK_PANEL_X + (UIK_PANEL_W - w) / 2),
                   (int16_t)(cy - 4), s, 1u, fg, bg);
}

/* 左侧 189px 区里水平居中 */
/*==========================================================================
 * 左侧键值显示（189 x 115 区域）—— 按"按了几个键"自适应排版
 *
 * 需求：
 *   1 个键值  -> 单行，字高 64px
 *   2 个键值  -> 分两排，每排字高 32px（两排合起来 64px 高）
 *   3 个键值  -> 分三排，每排字高 32px
 *   全部在显示区内**左右 + 上下居中**
 *
 * 字体：**专用的矢量笔画字体**（ui_kbd_font），
 *      不再是拿 5x7 小点阵放大 —— 那样每个原始像素会变成 8x8 的方块，又粗又糊。
 *      这里用"大写字母高度 cap"当字号：
 *          整行高（含下延） = KbdFont_BoxH(cap) = cap * 128 / 100
 *          目标整行 64px -> cap = 50
 *          目标整行 32px -> cap = 25
 *      名字过宽时按**像素宽度**自动压 cap，保证不超出 189px。
 *========================================================================*/

#define KBD_REG_X       0                       /* 显示区左上角 */
#define KBD_REG_Y       UIK_REGION_Y            /* = 20 */
#define KBD_REG_W       UIK_LEFT_W              /* = 189 */
#define KBD_REG_H       UIK_REGION_H            /* = 115 */

/* 每排之间的间隙 */
#define KBD_ROW_GAP     2

/* 两档字号：整行高（含下延）分别约 64px / 32px */
#define KBD_CAP_BIG     50
#define KBD_CAP_SMALL   25

/* 把 cap 从 want 往下压，直到整行宽度 <= max_w */
/*==========================================================================
 * 【改版】键名区的"伤害矩形"
 *
 * 键名是矢量笔画字，宽高随"按了几个键 / 名字多长"变。旧实现每次都先
 * TFT_FillRect(整屏 240x135) 再全画一遍 —— 一次就是 64800 像素 ≈ 130KB
 * SPI，在 15MHz 上要几十毫秒，正好卡在主循环里拖慢按键读取与上报。
 *
 * 现在：每次画完把"这次真正画到的矩形"记下来，下次重画前只擦这一块
 *（例如单键 64px 高时约 60x64 = 3840 像素，只有整屏的 1/17）。
 *========================================================================*/
static int16_t  s_dmg_x, s_dmg_y, s_dmg_w, s_dmg_h;

static void kbd_damage_reset(void)
{
    s_dmg_x = 0; s_dmg_y = 0; s_dmg_w = 0; s_dmg_h = 0;
}

static void kbd_damage_add(int16_t x, int16_t y, int16_t w, int16_t h)
{
    int16_t x2, y2;

    if ((w <= 0) || (h <= 0)) { return; }

    if (s_dmg_w <= 0)
    {
        s_dmg_x = x; s_dmg_y = y; s_dmg_w = w; s_dmg_h = h;
        return;
    }

    x2 = (int16_t)(s_dmg_x + s_dmg_w);
    y2 = (int16_t)(s_dmg_y + s_dmg_h);
    if ((int16_t)(x + w) > x2) { x2 = (int16_t)(x + w); }
    if ((int16_t)(y + h) > y2) { y2 = (int16_t)(y + h); }
    if (x < s_dmg_x) { s_dmg_x = x; }
    if (y < s_dmg_y) { s_dmg_y = y; }
    s_dmg_w = (int16_t)(x2 - s_dmg_x);
    s_dmg_h = (int16_t)(y2 - s_dmg_y);
}

/* 把一块矩形夹到"键名显示区"内。
 *
 * 为什么必须夹：伤害矩形按墨迹余量外扩之后，会比显示区大出几个像素 ——
 *   * 上边会压到 y < KBD_REG_Y(=20) 的通道状态条；
 *   * 右边会压到 x >= KBD_REG_X + KBD_REG_W(=189) 的 1px 分割线和右栏。
 * 不夹的话每次重画都会把状态条和分割线擦掉一块。 */
static int16_t kbd_clamp16(int16_t v, int16_t lo, int16_t hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

static void kbd_erase_damage(void)
{
    if (s_dmg_w > 0)
    {
        int16_t x1 = kbd_clamp16(s_dmg_x, (int16_t)KBD_REG_X,
                                 (int16_t)(KBD_REG_X + KBD_REG_W));
        int16_t y1 = kbd_clamp16(s_dmg_y, (int16_t)KBD_REG_Y,
                                 (int16_t)(KBD_REG_Y + KBD_REG_H));
        int16_t x2 = kbd_clamp16((int16_t)(s_dmg_x + s_dmg_w), (int16_t)KBD_REG_X,
                                 (int16_t)(KBD_REG_X + KBD_REG_W));
        int16_t y2 = kbd_clamp16((int16_t)(s_dmg_y + s_dmg_h), (int16_t)KBD_REG_Y,
                                 (int16_t)(KBD_REG_Y + KBD_REG_H));

        if ((x2 > x1) && (y2 > y1))
        {
            TFT_FillRect(x1, y1, (int16_t)(x2 - x1), (int16_t)(y2 - y1),
                         (uint16_t)UI_COL_BG);
        }
    }
    kbd_damage_reset();
}


static int16_t kbd_fit_cap(const char *s, int16_t want, int16_t max_w)
{
    int16_t cap = want;

    if ((s == 0) || (*s == '\0'))
    {
        return want;
    }

    while ((cap > 8) && (KbdFont_Width(s, cap) > max_w))
    {
        cap--;
    }
    return cap;
}

/* 画一行：在显示区内水平居中 */
static void kbd_draw_row(const char *s, int16_t row_y, int16_t cap,
                         uint16_t fg, uint16_t bg)
{
    int16_t w = KbdFont_Width(s, cap);
    int16_t m = KbdFont_InkMargin(cap);         /* 墨迹余量 */
    int16_t x = (int16_t)(KBD_REG_X + ((int16_t)KBD_REG_W - w) / 2);

    /* 水平居中，但保证"墨迹"（含余量）不越出显示区左右边界 */
    if (x < (int16_t)(KBD_REG_X + m))
    {
        x = (int16_t)(KBD_REG_X + m);
    }
    if ((int16_t)(x + w + m) > (int16_t)(KBD_REG_X + KBD_REG_W))
    {
        x = (int16_t)(KBD_REG_X + KBD_REG_W - w - m);
    }
    if (x < (int16_t)KBD_REG_X)
    {
        x = (int16_t)KBD_REG_X;                 /* 极端情况：兜底，宁可右溢 */
    }

    /* 【修复"上一次的字擦不干净"】伤害矩形把墨迹余量算进去（上下左右各 m）。
     * 原来记的是 (x, row_y, w, BoxH) —— 正好是逻辑框，笔画溢出那一圈没记进来，
     * 下次只擦逻辑框，溢出部分就留在屏上成了"残影"。 */
    kbd_damage_add((int16_t)(x - m), (int16_t)(row_y - m),
                   (int16_t)(w + 2 * m), (int16_t)(KbdFont_BoxH(cap) + 2 * m));
    KbdFont_Draw(x, row_y, s, cap, fg, bg);
}

/* 主入口：按按下键数自适应画左侧键值 */
static void kbd_draw_pressed(uint16_t fg, uint16_t bg)
{
    const char *names[6];
    uint8_t     n;
    uint8_t     i;

    n = UiKbd_BuildPressedList(names, 6u);
    if (n == 0u)
    {
        return;                             /* 没按键：留白 */
    }

    if (n == 1u)
    {
        /* ---- 单键：一行，整行高 64px ---- */
        int16_t cap = kbd_fit_cap(names[0], KBD_CAP_BIG, (int16_t)KBD_REG_W);
        int16_t h   = KbdFont_BoxH(cap);
        int16_t mg  = KbdFont_InkMargin(cap);   /* 上下各留出墨迹余量 */
        int16_t y   = (int16_t)(KBD_REG_Y + mg +
                                ((int16_t)KBD_REG_H - h - 2 * mg) / 2);

        kbd_draw_row(names[0], y, cap, fg, bg);
        return;
    }

    /* ---- 组合键：2 排或 3 排，每排整行高 32px ----
     * 每排可能因名字宽窄压出不同的 cap，统一取最小的，保证各排字号一致。 */
    {
        int16_t cap   = KBD_CAP_SMALL;
        int16_t rows  = (int16_t)n;
        int16_t row_h;
        int16_t total;
        int16_t y0;
        int16_t mg;

        for (i = 0u; i < n; i++)
        {
            int16_t c = kbd_fit_cap(names[i], KBD_CAP_SMALL, (int16_t)KBD_REG_W);
            if (c < cap)
            {
                cap = c;
            }
        }

        row_h = KbdFont_BoxH(cap);
        total = (int16_t)(rows * row_h + (rows - 1) * KBD_ROW_GAP);
        /* 上下各留出墨迹余量，保证第一排顶、最后一排底的笔画都在显示区内 */
        mg    = KbdFont_InkMargin(cap);
        y0    = (int16_t)(KBD_REG_Y + mg +
                          ((int16_t)KBD_REG_H - total - 2 * mg) / 2);

        for (i = 0u; i < n; i++)
        {
            kbd_draw_row(names[i], (int16_t)(y0 + i * (row_h + KBD_ROW_GAP)),
                         cap, fg, bg);
        }
    }
}

/*==========================================================================
 * 【改版】键盘页分区刷新（需求：禁止整屏刷新、优先按键读取与传输）
 *
 * 三层各管各的，谁也整屏重画：
 *   1) 静态层：整页底色 + 两条 1px 分割线 —— **只在进页面时**画一次；
 *   2) 键值层：左侧 189x115 的键名 + 右下 3x3 井字格 —— 只在位图变化时重画，
 *              而且只擦上一次真画过的矩形（见上面的伤害矩形）；
 *   3) 状态层：右上通道大字 + 右栏"通道 / OK-NO" —— 只在通道或连接态变化时重画。
 *
 * 重画频率另由 kbd_update() 限到 <=10fps：主循环里的按键扫描与 HID 上报
 * 优先级更高，屏幕慢一两帧没人看得出来。
 *========================================================================*/
#define UIK_REDRAW_MIN_MS   100u        /* 键盘页重画间隔下限：约 10fps */
#define UIK_ENTER_BLOCK_MS  500u        /* 进页面后屏蔽按键的时长（需求 0.5s） */

static bool     s_page_full = true;     /* 还没画静态层 */
static uint8_t  s_want_redraw;          /* 有变化、但被节流推迟 */
static uint32_t s_last_draw_ms;
static uint16_t s_draw_map  = 0xFFFFu;  /* 上次真正画上屏的位图 */
static ui_transport_t s_draw_ch;
static uint8_t  s_draw_conn = 0xFFu;

/* 静态层：整页底色 + 分割线（本页唯一的整屏点，只在进页面时执行） */
static void kbd_draw_static(void)
{
    TFT_FillRect(0, 0, (int16_t)UI_SCREEN_W, (int16_t)UI_SCREEN_H, (uint16_t)UI_COL_BG);

    UiW_VLine((int16_t)UIK_LEFT_W, (int16_t)UIK_REGION_Y, (int16_t)UIK_REGION_H,
              (uint16_t)UIW_LINE_WHITE);
    UiW_HLine((int16_t)UIK_PANEL_X, 70, (int16_t)UIK_PANEL_W,
              (uint16_t)UIW_LINE_WHITE);
}

/* 状态层（右上大字 + 右栏两行） */
static void kbd_draw_status(void)
{
    kbd_draw_channel_top();
    kbd_draw_center_y(45, UiKbd_ChannelText(), (uint16_t)UI_COL_TEXT, (uint16_t)UI_COL_BG);
    kbd_draw_center_y(76, UiKbd_StateText(),
                      UiKbd_Connected() ? (uint16_t)UI_COL_GREEN : (uint16_t)UI_COL_DIM,
                      (uint16_t)UI_COL_BG);
}

static void kbd_draw(void)
{
    uint16_t       map  = KEY_GetMap();
    ui_transport_t ch   = UiKbd_Channel();
    uint8_t        conn = UiKbd_Connected() ? 1u : 0u;

    if (s_page_full)
    {
        kbd_draw_static();
        s_page_full = false;

        /* 逼着下面三块都重画一遍 */
        kbd_damage_reset();
        s_draw_map  = 0xFFFFu;
        s_draw_ch   = (ui_transport_t)0xFF;
        s_draw_conn = 0xFFu;
    }

    if (map != s_draw_map)
    {
        kbd_erase_damage();                                 /* 只擦上次那块 */
        kbd_draw_pressed((uint16_t)UI_COL_GREEN, (uint16_t)UI_COL_BG);
        UiW_DrawKeyGrid((int16_t)UIK_PANEL_X, 85, (int16_t)UIK_GRID_SIZE, map);
        s_draw_map = map;
    }

    if ((ch != s_draw_ch) || (conn != s_draw_conn))
    {
        kbd_draw_status();
        s_draw_ch   = ch;
        s_draw_conn = conn;
    }
}


/*==========================================================================
 * 页面：按键
 *
 *   长按 1 秒  -> 键盘设置页（需求 (二).3）
 *   单击       -> 非列表页当"3x3 之外的按键"用：敲一下回车（需求 (六).2）
 *   双击       -> 返回上一级（框架全局规则，页面不用管）
 *   长按 3 秒  -> 回主页面（框架全局规则）
 *
 * 注意：0.5s 那档 GKEY_ENC_HOLD 本页**不接**。文档给键盘页写的是
 * "长按 1 秒进设置页"，接 0.5s 会让设置页提前弹出来，而且和单击撞车。
 *========================================================================*/
static ui_nav_t kbd_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE)
    {
        return UI_NAV_STAY;
    }

    if (ev->key == GKEY_ENC_HOLD1)
    {
        UI_Push(&UI_PAGE_KBSET, 0);     /* 长按 1 秒：键盘设置页（需求 (二).3） */
    }
    else if (ev->key == GKEY_ENC_CLICK)
    {
        /* 【2026-10-13】单击 = 当前组"编码器单击"槽的键值（默认回车，
         * 和旧行为一致）。网页改完 SAVE_FLASH 后长期生效。 */
        UiKbd_PulseMapping(KEY_GetEncMapped(KEY_ENC_CLICK));
    }
    else if (ev->key == GKEY_ENC_CW)
    {
        UiKbd_PulseMapping(KEY_GetEncMapped(KEY_ENC_CW));
    }
    else if (ev->key == GKEY_ENC_CCW)
    {
        UiKbd_PulseMapping(KEY_GetEncMapped(KEY_ENC_CCW));
    }
    return UI_NAV_STAY;
}

/*==========================================================================
 * 页面：每帧回调
 *
 * 本页的"当前按下按键"文字和右下角 3x3 井字格，都是在 kbd_draw() 里现读
 * KEY_GetMap() 画出来的。而 UI_Task() 只在 s_dirty != 0 时才会调 draw()，
 * s_dirty 又只由页面切换和 on_key() 里的 UI_Dirty() 置位 ——
 * 矩阵键走的是 kbd_key()，那里不标脏，所以页面进来画一次后就再也不重画，
 * 按什么键屏幕都不会动（看着像"按键读不到"）。
 *
 * 所以在这里自己盯着位图：一变就标脏，让 UI_Task() 重画这一页。
 *==========================================================================*/
static uint16_t       s_shown_map  = 0xFFFFu;  /* 上一次"看到"的按键位图 */
static ui_transport_t s_shown_ch;
static uint8_t        s_shown_conn = 0xFFu;

/*********************************************************************
 * @fn      kbd_enter
 * @brief   进键盘页：屏蔽按键 0.5 秒 + 标记整页重画
 *
 * 需求：进入键盘页的前 0.5 秒不读取按键。
 * 切页那一瞬间手上往往还按着键（或触点还在抖），这些不该被当成
 * "用户刚按下"灌进 HID 报文。KEY_BlockInput() 期间扫描结果恒为无按键，
 * 之前按着的键会在去抖窗后自然上报松开，不会卡住主机。
 *
 * 同时把"上次画过"的记录全部清掉：本页现在只画变化的部分，
 * 不清的话回到本页会少画静态层。
 *********************************************************************/
/* 【2026-10-14 关键修复】kbd_enter() 必须幂等。
 *
 * 现场证据：
 *   1) loop_diag 显示 T0 C0 —— 仲裁 phase 被重置回 UI_TR_SCAN_USB。
 *      而 phase 只在 UiTr_Init() 里被设成 SCAN_USB，UiTr_Init() 只在本函数调。
 *   2) 每次进本函数都会 s_page_full = true，触发一次整屏重画；
 *      实测一次就是 133ms（W=133，和 64800 字节 / 487KB/s 完全吻合）。
 *
 * 也就是说本函数【在反复执行】—— 于是 BLE 协议栈每隔一会儿就被堵死
 * 133ms，主机的 ATT 服务发现超时就把链路拆掉：连接 6<->23 反复横跳，
 * 而且关掉绑定后循环更快（少一次配对往返，堵得更频繁）。
 *
 * 修法：只有【真的刚进本页】才做初始化。用 s_entered 记一次，
 * 由 kbd_leave() 清掉；重复调用变成空操作。
 *
 * 注意：整屏重画本身是需求允许的（进页画一次静态层），要治的是
 * 它被反复触发。 */
static uint8_t s_entered = 0u;

static void kbd_enter(void *arg)
{
    (void)arg;

    if (s_entered != 0u)
    {
        return;                         /* 已经在页里，重复调用直接忽略 */
    }
    s_entered = 1u;

    /* 【2026-10-13 修复：9 键间歇性锁死 的根因】
     * 原来这里无条件续期。可页面处理函数会被【长按连发】反复调用：
     * KEY 按住 450ms 后，key_track() 每 90ms 发一条 GAME_EV_REPEAT，
     * 而页面处理函数普遍只过滤了 RELEASE、没过滤 REPEAT（见 ui_home.c 等），
     * 于是 UI_Push()/enter 被反复执行 -> 这里每 90ms 把屏蔽续期一次
     * -> KEY_IsBlocked() 永远是 1 -> KEY_Scan() 跳过整段扫描
     * -> **9 个矩阵键全死**（编码器走 ENC_ButtonPressed()，不经过 KEY_Scan，
     *    所以它照常好用，正好对上现场现象）。松开按键后不再有 REPEAT，
     * 500ms 后自动解除 —— 也就是现场看到的"等一会儿自己恢复"。
     *
     * 修法：屏蔽只在【还没屏蔽】时才armed，重复进入本页不会续期。 */
    /* 【2026-10-14 撤掉 0.5 秒按键屏蔽】
     *
     * 这个屏蔽当初是为了挡住【用 KEY5 进本页、结果本页立刻显示 5】。
     * 但它的实现是让 KEY_Scan() 整段跳过 GPIO 扫描 0.5 秒 —— 代价太大：
     * 屏蔽标志一旦退不出来，9 个矩阵键会全部锁死（现场实测过 B=1 卡死），
     * 而且进页后头 0.5 秒按键是死的，很别扭。
     *
     * 真正的病根是【事件跨页投递】：切页那一刻，队列里还没排完的
     * PRESS/REPEAT 会投给新页面。现在改成在 UI_Push/UI_Pop/UI_GoHome
     * 里调 Game_FlushEvents() 把旧页面的事件丢掉 —— 病根堵住了，
     * 这里就不再需要屏蔽。
     *
     * 需求文档里那句【进入键盘页的前 0.5 秒不读取按键】，其意图
     *（不要把进页那一瞬间残留的键当成新按键）由上面的根治完全满足，
     * 而且不会再出现按键被锁死的副作用。 */

    s_page_full    = true;
    s_want_redraw  = 1u;
    s_last_draw_ms = 0u;

    s_shown_map  = 0xFFFFu;
    s_shown_ch   = (ui_transport_t)0xFF;
    s_shown_conn = 0xFFu;
}

/*********************************************************************
 * @fn      kbd_update
 * @brief   每帧回调：只负责"该不该重画"，重画本身在 kbd_draw()
 *
 * 本页有三样东西是现读的：井字格/键名（KEY_GetMap）、右上角通道名
 *（UiKbd_Channel）、右中角 OK/NO（UiKbd_Connected）。任意一样变了才标脏。
 *
 * 【10fps 上限】标脏之后还要过一道 100ms 闸门才真正 UI_Dirty()：
 * 主循环里按键扫描 -> HID 上报 -> 界面刷新 是排好的，屏幕慢一两帧
 * 没人看得出来，但按键的读取与传输不能被刷屏拖住。
 *********************************************************************/
static void kbd_update(uint16_t dt_ms)
{
    uint16_t       map  = KEY_GetMap();
    ui_transport_t ch   = UiKbd_Channel();
    uint8_t        conn = UiKbd_Connected() ? 1u : 0u;
    uint32_t       now;

    (void)dt_ms;

    if ((map != s_shown_map) || (ch != s_shown_ch) || (conn != s_shown_conn))
    {
        s_shown_map   = map;
        s_shown_ch    = ch;
        s_shown_conn  = conn;
        s_want_redraw = 1u;
    }

    now = Game_Millis();

    /* 刚进页面必须立刻画一次；其余情况限到 <=10fps */
    if ((s_want_redraw != 0u) &&
        (s_page_full || ((uint32_t)(now - s_last_draw_ms) >= (uint32_t)UIK_REDRAW_MIN_MS)))
    {
        s_want_redraw  = 0u;
        s_last_draw_ms = now;
        UI_Dirty();
    }
}


/* 【2026-10-14】离开本页时清掉进入标志，下次进来才会重新初始化。 */
static void kbd_leave(void)
{
    s_entered = 0u;
}

const ui_page_t UI_PAGE_KBD = { "kbd", kbd_enter, kbd_leave, kbd_key, kbd_update, kbd_draw };

/******************* (C) CH592X 键盘页 *****************/
