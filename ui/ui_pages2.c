/**************************************************************************
 * ui_pages2.c —— 后三个页面：键盘设置页 / 键值编辑页 / 遥控页
 *
 * 依据：需求文档
 *   (2).3  键盘设置页：3 套默认键值映射 + 1 套自定义（进键值编辑页）
 *   (7).3  键值编辑页：左侧 50px（上=按键名、下=井字格）+ 右侧 HID 名称列表
 *   (4)    遥控页：2.4G 广播 -> 设备列表 -> 9 键功能
 *
 * 视觉：列表部分全部改用**卡片式列表**（ui_cardlist），选中卡片是绿色。
 *       键值编辑页左侧的井字格与分隔线保留（那是功能性的，不是装饰）。
 **************************************************************************/

#include "ui_pages2.h"
#include "rc_table.h"
#include "ui_cardlist.h"
#include "ui_widget.h"
#include "ui_kbd.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_icon.h"
#include "ui_card.h"
#include "tft_gfx.h"
#include "key_matrix.h"
#include "game_port.h"
#include "game_text.h"

/* 屏上画 ASCII 用的小工具（字库没有键名/映射名词条） */
/*********************************************************************
 * @fn      p2_ascii_center
 * @brief   画一行 ASCII 并水平居中
 * @param   cy     这一行**垂直中心**的 y
 * @param   scale  字号：1 = 8px 高，2 = 16px 高
 * @note    ★ 原来固定 scale=1 且写死 cy-4，而 p2_title() 传的是 cy=1
 *          -> 字被画到 y=-3，顶部直接被切掉。改成按真实字高算偏移。
 *********************************************************************/
static void p2_ascii_center(int16_t cy, int16_t x0, int16_t w,
                            const char *s, uint8_t scale,
                            uint16_t fg, uint16_t bg)
{
    int16_t tw;
    int16_t th;
    int16_t x;

    if (scale == 0u) { scale = 1u; }

    tw = Game_AsciiWidth(s, scale);
    th = (int16_t)(8 * (int16_t)scale);     /* 5x7 点阵放在 8 行的格子里 */
    x  = (int16_t)(x0 + (w - tw) / 2);

    if (x < x0) { x = x0; }
    Game_DrawAscii(x, (int16_t)(cy - th / 2), s, scale, fg, bg);
}

/*********************************************************************
 * @fn      p2_ascii_in_card
 * @brief   卡片列表里画一行 ASCII：自动选字号 + 在 16px 槽位里垂直居中
 * @note    优先 16px（scale 2，和卡片高度匹配）；名字太长放不下再退 8px
 *********************************************************************/
static void p2_ascii_in_card(int16_t cx, int16_t cy, int16_t cw,
                             const char *s, uint16_t fg, uint16_t bg)
{
    uint8_t sc = 2u;
    int16_t th;

    if (Game_AsciiWidth(s, 2u) > cw)
    {
        sc = 1u;                            /* 太长：退回 8px 保证不越界 */
    }
    th = (int16_t)(8 * (int16_t)sc);
    Game_DrawAscii(cx, (int16_t)(cy + (16 - th) / 2), s, sc, fg, bg);
}

/* 顶部标题条 */
static void p2_title(const char *s)
{
    /* 标题条是 y 0..16（y=17 是分隔线）：16px 的字垂直居中 -> 字顶落在 y=0。
     * 名字太长（遥控设备名最多 20 字符）自动退 8px，别顶出屏幕。 */
    uint8_t sc = 2u;

    if (Game_AsciiWidth(s, 2u) > (int16_t)(UI_SCREEN_W - 8))
    {
        sc = 1u;
    }
    p2_ascii_center(8, 0, (int16_t)UI_SCREEN_W, s, sc, C_ACCENT, C_BG);
    TFT_FillRect(0, (int16_t)17, UI_SCREEN_W, 1, C_ACCENT_DIM);
}

/*==========================================================================
 * 1. 键盘设置页（文档 (2).3）
 *========================================================================*/
/* 【修复 §8-1 / §8-11】列表 = 3 套默认映射 + 2 套自定义映射 + RGB 亮度。
 *
 * 原来是 KBSET_ITEMS=4 而判断写成 sel < KEY_MAP_GROUPS（当时也是 4），
 * 于是 sel 永远取 0..3、恒 < 4 —— "进键值编辑页"那条分支是死代码，
 * 设备上根本进不去编辑页。现在：
 *     idx 0..2            默认映射组：选中即切换生效
 *     idx 3..G-1          自定义映射组：先切生效，再进键值编辑页改键值
 *     idx G（最后一项）    RGB 亮度：跳亮度页（需求 (二).3.c）      */
#define KBSET_ITEMS         ((uint8_t)KEY_MAP_GROUPS + 1u)
#define KBSET_ITEM_BRIGHT   ((uint8_t)KEY_MAP_GROUPS)      /* 最后一项 = 亮度 */
#define KBSET_CUSTOM_FIRST  3u                             /* 组 3 起是自定义 */

static ui_cardlist_t s_kbset;

static void kbset_draw_item(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                            uint8_t idx, uint8_t selected, void *user)
{
    char        buf[24];
    const char *nm;
    uint16_t    fg = (selected != 0u) ? C_TEXT : C_TEXT_DIM;
    uint16_t    bg = (selected != 0u) ? Theme_Blend(C_BG_CARD_HI, C_GREEN, 40)
                                      : C_BG_CARD;

    (void)ch; (void)user;

    if (idx >= KBSET_ITEM_BRIGHT)
    {
        nm = "RGB";                     /* 最后一项：亮度页入口 */
    }
    else
    {
        nm = KEY_GetMapGroupName(idx);  /* 前 5 项：映射组名（网页可改） */

        if (idx >= KBSET_CUSTOM_FIRST)
        {
            /* 自定义组后面挂个 '*'：提示"选它还会进编辑页" */
            uint8_t n = 0u;

            while ((nm[n] != '\0') && (n < (uint8_t)(sizeof(buf) - 2u)))
            {
                buf[n] = nm[n];
                n++;
            }
            buf[n]      = '*';
            buf[n + 1u] = '\0';
            nm = buf;
        }
    }

    (void)cw;
    p2_ascii_in_card(cx, cy, cw, nm, fg, bg);
}

static void kbset_enter(void *arg)
{
    (void)arg;
    UiCList_Init(&s_kbset, ICON_KEYBOARD, kbset_draw_item, 0);
    UiCList_SetCount(&s_kbset, (uint8_t)KBSET_ITEMS);
    UiCList_SetSel(&s_kbset, KEY_GetMapGroup());
    UI_Dirty();
}

static ui_nav_t kbset_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }

    if ((ev->key == GKEY_ENC_CW) || (ev->key == GKEY_DOWN) || (ev->key == GKEY_RIGHT))
    {
        UiCList_Move(&s_kbset, +1);
        UI_Dirty();
        return UI_NAV_STAY;
    }
    /* 【修复 §8-8】上移不认 GKEY_UP —— GKEY_UP 就是 GKEY_OK(K5)，
     * 认了它 K5 就只能上移、永远确认不了。列表页矩阵键按需求 (六).2 是"确认"。 */
    if ((ev->key == GKEY_ENC_CCW) || (ev->key == GKEY_LEFT))
    {
        UiCList_Move(&s_kbset, -1);
        UI_Dirty();
        return UI_NAV_STAY;
    }

    if ((ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK) ||
        (ev->key == GKEY_ENC_HOLD))
    {
        uint8_t sel = UiCList_Selected(&s_kbset);

        if (sel < KBSET_ITEM_BRIGHT)
        {
            (void)KEY_SetMapGroup(sel);     /* 选中这套映射，立刻生效 */
            {
                ui_cfg_t *c = UiStore_Cfg();
                if (c != 0)
                {
                    c->map_group = sel;
                    (void)UiStore_Save();
                }
            }

            if (sel >= KBSET_CUSTOM_FIRST)
            {
                /* 需求 (二).3.b：自定义那两套选中后进键值编辑页 */
                UI_Push(&UI_PAGE_KEYEDIT, (void *)(uintptr_t)sel);
            }
        }
        else
        {
            /* 需求 (二).3.c：RGB 亮度调节，功能同亮度页 */
            UI_Push(&UI_PAGE_BRIGHT, 0);
        }
        UI_Dirty();
    }
    return UI_NAV_STAY;
}

static void kbset_draw(void)
{
    if (UiCList_TakeBg(&s_kbset) != 0u)
    {
        TFT_FillRect(0, 0, (int16_t)UI_SCREEN_W, (int16_t)UI_SCREEN_H, C_BG);
        p2_title("KEYSET");
        UiCList_Draw(&s_kbset);
    }
    else
    {
        UiCList_DrawText(&s_kbset);
    }
}

const ui_page_t UI_PAGE_KBSET =
    { "kbset", kbset_enter, 0, kbset_key, 0, kbset_draw };

/*==========================================================================
 * 2. 键值编辑页（文档 (7).3）
 *
 * 左 50px：上 = 正在编辑的按键名（K1..K9），下 = 3x3 井字格（显示当前按下）
 * 1px 分隔线
 * 右 189px：HID 键值名称卡片列表（23px 行高、可循环）
 *
 * 交互（文档 (6)）：旋转 = 在 HID 列表里选，单击 = 把选中的键值赋给当前键、
 * 然后自动跳到下一个按键（编辑完 9 个键就回到键盘设置页）。
 *========================================================================*/
/* HID 用途码表：能编辑的键值（ASCII 名，屏上用 Game_DrawAscii 画）
 *
 * 这里的顺序就是编码器滚动的顺序。加减键值只改这张表，名字配在
 * ui_kbd.c 的 UiKbd_UsageName() 里；网页工具 tools/keymap_editor.html
 * 的 HID_USAGE / USAGE_NAME 要保持同步（两边一致性的自测脚本：
 * tools/test_web_fw_consistency.py 的 T9）。
 *
 * 修饰键说明：0xE0~0xE7 是 HID 的 8 个修饰键。它们**不能**当普通键码
 * 发出去（Windows 不认），ui_kbd.c 会把它们塞进报文第 0 字节的修饰位，
 * 所以"映射成 LCtrl"的键可以按住再按别的键当组合键用。 */
static const uint8_t s_hid_usage[] =
{
    /* 修饰键放最前面：进键值编辑页后一转编码器就到 Ctrl，
       不用滚 80 项（进页面时光标落在"当前键已用的值"上，
       没映射过就是第一项）。0xE0~0xE7 进报文第 0 字节，可当组合键前缀。 */
    0xE0u, /* LCtrl */
    0xE1u, /* LShft */
    0xE2u, /* LAlt  */
    0xE3u, /* LGUI  */
    0xE4u, /* RCtrl */
    0xE5u, /* RShft */
    0xE6u, /* RAlt  */
    0xE7u, /* RGUI  */
    /* 字母 A~Z */
    0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du,
    0x0Eu, 0x0Fu, 0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
    0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du,
    /* 数字行 1~9、0 */
    0x1Eu, 0x1Fu, 0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u,
    /* 编辑键 */
    0x28u, /* Enter */
    0x29u, /* Esc   */
    0x2Au, /* Bksp  */
    0x2Bu, /* Tab   */
    0x2Cu, /* Space */
    0x39u, /* Caps  */
    /* 功能键 F1~F12 */
    0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu,
    0x40u, 0x41u, 0x42u, 0x43u, 0x44u, 0x45u,
    /* 编辑/导航键 */
    0x49u, /* Ins  */
    0x4Au, /* Home */
    0x4Bu, /* PgUp */
    0x4Cu, /* Del  */
    0x4Du, /* End  */
    0x4Eu, /* PgDn */
    /* 方向键 */
    0x4Fu, 0x50u, 0x51u, 0x52u,
    /* 小键盘 1~9 */
    0x59u, 0x5Au, 0x5Bu, 0x5Cu, 0x5Du, 0x5Eu, 0x5Fu, 0x60u, 0x61u,
    /* 媒体键 */
    0x80u, 0x81u
};
#define HID_USAGE_COUNT ((uint8_t)(sizeof(s_hid_usage) / sizeof(s_hid_usage[0])))

static ui_cardlist_t s_ke_list;
static uint8_t   s_ke_key;          /* 正在编辑第几个按键（1..9） */

static void ke_draw_item(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                         uint8_t idx, uint8_t selected, void *user)
{
    const char *nm = UiKbd_UsageName(s_hid_usage[idx]);

    (void)ch; (void)user;
    p2_ascii_in_card(cx, cy, cw, nm,
                     (selected != 0u) ? C_TEXT : C_TEXT_DIM,
                     (selected != 0u) ? Theme_Blend(C_BG_CARD_HI, C_GREEN, 40)
                                      : C_BG_CARD);
}

static void ke_load_list(void)
{
    uint8_t i;
    uint8_t sel = 0u;
    uint16_t cur;

    /* 光标先落在"当前这个键已经用的键值"上 */
    cur = KEY_GetMapped(s_ke_key);
    for (i = 0u; i < HID_USAGE_COUNT; i++)
    {
        if (s_hid_usage[i] == (uint8_t)cur) { sel = i; break; }
    }

    UiCList_Init(&s_ke_list, ICON_KEYBOARD, ke_draw_item, 0);
    /* 右侧列表：左边界让开 50px 的左栏 + 1px 分隔线
     * （UiCList_Init 默认 x=4/w=232，这里按住键值编辑页版面覆盖） */
    s_ke_list.x      = (int16_t)(UIK2_KEY_W + 3);
    s_ke_list.w      = (int16_t)(UI_SCREEN_W - UIK2_KEY_W - 5);
    s_ke_list.view_y = (int16_t)UI_BAT_H;
    s_ke_list.view_h = (int16_t)(UI_SCREEN_H - UI_BAT_H);
    UiCList_SetCount(&s_ke_list, HID_USAGE_COUNT);
    UiCList_SetSel(&s_ke_list, sel);
}

/* 左下角 3x3 井字格：位置和键盘页一样贴左下 */
#define KE_GRID_X       0
#define KE_GRID_Y       85
#define KE_GRID_SIZE    50

static void ke_draw(void)
{
    /* 换 HID 项时只重画右侧列表；左边那栏（按键名 + 井字格）只在
     * 换按键/键位变化时才动 —— 那时候 ke_update() 会 ForceBg。 */
    if (UiCList_TakeBg(&s_ke_list) != 0u)
    {
        char buf[8];

        TFT_FillRect(0, 0, (int16_t)UI_SCREEN_W, (int16_t)UI_SCREEN_H, C_BG);

        /* 左侧：上=按键名，下=井字格 */
        buf[0] = 'K';
        buf[1] = (char)('0' + (char)s_ke_key);
        buf[2] = '\0';
        p2_ascii_center(35, 0, (int16_t)UIK2_KEY_W, buf, 1u, C_GREEN, C_BG);

        /* 左栏第二行：当前键的映射值（4 位十六进制）。
         * 高 2 位是修饰键位图、低 2 位是 HID 键值 —— 是不是组合键一眼可见。 */
        {
            static const char s_hex[] = "0123456789ABCDEF";
            uint16_t v = KEY_GetMapped(s_ke_key);

            buf[0] = s_hex[(v >> 12) & 0x0Fu];
            buf[1] = s_hex[(v >>  8) & 0x0Fu];
            buf[2] = s_hex[(v >>  4) & 0x0Fu];
            buf[3] = s_hex[v & 0x0Fu];
            buf[4] = '\0';
            p2_ascii_center(58, 0, (int16_t)UIK2_KEY_W, buf, 1u, C_TEXT_DIM, C_BG);
        }

        UiW_DrawKeyGrid((int16_t)KE_GRID_X, (int16_t)KE_GRID_Y,
                        (int16_t)KE_GRID_SIZE, KEY_GetMap());

        /* 三部分 1px 分隔线（文档 (7).3.h）：改用主题暗青，跟卡片风统一 */
        TFT_FillRect(0, 70, (int16_t)UIK2_KEY_W, 1, C_ACCENT_DIM);
        TFT_FillRect(0, 84, (int16_t)UIK2_KEY_W, 1, C_ACCENT_DIM);
        TFT_FillRect((int16_t)UIK2_KEY_W, 20, 1, 115, C_ACCENT_DIM);

        /* 右侧：HID 键值名称卡片列表 */
        UiCList_Draw(&s_ke_list);
    }
    else
    {
        /* 只换了 HID 项：左边那栏不动，右列表只重画文字 */
        UiCList_DrawText(&s_ke_list);
    }
}

static void ke_enter(void *arg)
{
    uint8_t grp = (uint8_t)(uintptr_t)arg;

    if (grp < (uint8_t)KEY_MAP_GROUPS)
    {
        (void)KEY_SetMapGroup(grp);
    }

    s_ke_key = 1u;
    ke_load_list();
    UI_Dirty();
}

static ui_nav_t ke_key(const GameKeyEvent_t *ev)
{
    if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }

    /* 返回统一走双击：UI_PostKey() 里全局处理（需求 (六).3）。
     * 长按不再当返回键用 —— 那是 HOLD 档位，要留给"执行当前操作"。 */

    if ((ev->key == GKEY_ENC_CW) || (ev->key == GKEY_DOWN) || (ev->key == GKEY_RIGHT))
    {
        UiCList_Move(&s_ke_list, +1);
        UI_Dirty();
        return UI_NAV_STAY;
    }
    /* 【修复 §8-8】同上：GKEY_UP == GKEY_OK == K5，不能算"上移" */
    if ((ev->key == GKEY_ENC_CCW) || (ev->key == GKEY_LEFT))
    {
        UiCList_Move(&s_ke_list, -1);
        UI_Dirty();
        return UI_NAV_STAY;
    }

    if ((ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK) ||
        (ev->key == GKEY_ENC_HOLD))
    {
        uint8_t  usage = s_hid_usage[UiCList_Selected(&s_ke_list)];
        uint16_t old   = KEY_GetMapValue(KEY_GetMapGroup(), s_ke_key);

        /* 【修复 §8-2】只换低 8 位的 HID 键值，**高 8 位的修饰键位图原样保留**。
         * 原来写的是 (uint16_t)usage，高字节恒为 0 —— 网页上设好的组合键
         * （比如 0x0106 = 左Ctrl + H）在设备上确认一次就被降级成单键 0x0006。
         * 组合键本身由网页 / 上位机下发，设备这页只负责换主键。 */
        KEY_SetMapValue(KEY_GetMapGroup(), s_ke_key,
                        (uint16_t)((old & 0xFF00u) | (uint16_t)usage));
        {
            ui_cfg_t *c = UiStore_Cfg();
            if (c != 0)
            {
                c->map_group = KEY_GetMapGroup();
                (void)UiStore_Save();
            }
        }

        if (s_ke_key >= 9u)
        {
            return UI_NAV_POP;              /* 9 个键编完，回键盘设置页 */
        }
        s_ke_key++;
        ke_load_list();
        UI_Dirty();
    }
    return UI_NAV_STAY;
}

/* 本页左下角的 3x3 井字格也是现读 KEY_GetMap() 画的（见 ke_draw）。
 * 同键盘页：ke_key() 不标脏，必须在每帧回调里盯着位图，一变就重画。 */
static uint16_t s_ke_shown_map;

static void ke_update(uint16_t dt_ms)
{
    uint16_t map = KEY_GetMap();

    (void)dt_ms;

    if (map != s_ke_shown_map)
    {
        s_ke_shown_map = map;
        UiCList_ForceBg(&s_ke_list);    /* 左下井字格要跟着重画（不只是列表） */
        UI_Dirty();
    }
}

const ui_page_t UI_PAGE_KEYEDIT =
    { "keyedit", ke_enter, 0, ke_key, ke_update, ke_draw };

/*==========================================================================
 * 3. 遥控页（文档 (4)）
 *
 * 阶段一：设备列表 —— 2.4G 主机持续广播，从机回心跳和设备名，
 *         列表里列出发现的设备（名字用 ASCII，字库没有设备名词条）。
 * 阶段二：选中设备后显示 9 个按键的功能，按下就把对应指令发出去。
 *========================================================================*/
#define RC_STAGE_LIST   0xFFu

static uint8_t  s_rc_dev;           /* 选中的从机下标，RC_STAGE_LIST = 还在列表里 */
static uint8_t  s_rc_dev_n;         /* 当前发现了几台 */
static uint8_t  s_rc_fn;            /* 9 键里选中的那个（0..8） */
static uint16_t s_rc_keys;          /* 当前按下的 9 键位图 */
static uint8_t  s_rc_pend;          /* 待发的编码器操作：0=无，10=左转 11=右转 12=单击 */
static uint8_t  s_rc_op;           /* 当前操作 key_id 1..12，0 = 无 */
static uint8_t  s_rc_act;          /* 该操作对应的动作编号，0 = 没配 */
static char     s_rc_actname[RC_ACT_NAME_MAX + 1u];

/* 【显示用】上面三个是当前操作的功能名缓存。
 * 查一次要读 Flash（RcTable_NameOfId 最多扫 64 条），所以**只在操作变化时
 * 查一次并缓存**，rc_draw() 每帧只画缓存、不碰 Flash。 */
static ui_cardlist_t s_rc_list;

/* 从机名字：主循环从 Rc_HostDev()->km.dev_name 抄进来（ASCII，字库没有名字词条） */
#define UIRC_NAME_MAX   20
static char    s_rc_name[UIRC_DEV_MAX][UIRC_NAME_MAX];
static uint8_t s_rc_name_len[UIRC_DEV_MAX];

/* 顶部状态提示：0 = 按设备数自动选默认文案。
 * 指向**字符串常量**（放在 Flash 里），只占一个指针，不额外吃 RAM。
 * 主循环在"遥控内存申请失败"这类故障时喂进来，免得故障表现成
 * "屏幕停在 0 设备、没有任何提示"（诊断报告"修复 6"）。 */
static const char *s_rc_msg;

static void rc_draw_dev_item(int16_t cx, int16_t cy, int16_t cw, int16_t ch,
                             uint8_t idx, uint8_t selected, void *user)
{
    char buf[12];
    uint16_t fg = (selected != 0u) ? C_TEXT : C_TEXT_DIM;
    uint16_t bg = (selected != 0u) ? Theme_Blend(C_BG_CARD_HI, C_GREEN, 40)
                                   : C_BG_CARD;

    (void)cw; (void)ch; (void)user;

    /* 有名字就画名字，没有就退回 "DEV n" */
    if ((idx < (uint8_t)UIRC_DEV_MAX) && (s_rc_name_len[idx] != 0u))
    {
        p2_ascii_in_card(cx, cy, cw, s_rc_name[idx], fg, bg);
        return;
    }

    buf[0] = 'D'; buf[1] = 'E'; buf[2] = 'V'; buf[3] = ' ';
    buf[4] = (char)('1' + (char)idx);
    buf[5] = 0;

    p2_ascii_in_card(cx, cy, cw, buf, fg, bg);
}

static void rc_enter(void *arg)
{
    (void)arg;
    s_rc_dev  = RC_STAGE_LIST;
    s_rc_fn   = 0u;
    s_rc_pend = 0u;
    s_rc_op   = 0u;
    s_rc_act  = 0u;
    s_rc_actname[0] = 0;
    s_rc_keys = 0u;

    UiCList_Init(&s_rc_list, ICON_RF24, rc_draw_dev_item, 0);
    UiCList_SetCount(&s_rc_list, s_rc_dev_n);
    UI_Dirty();
}

/* 从机上下线由外部喂进来（真机里由 Rc_HostCount() 定期刷新） */
void UiRc_SetDeviceCount(uint8_t n)
{
    uint8_t changed;

    if (n > (uint8_t)UIRC_DEV_MAX) { n = (uint8_t)UIRC_DEV_MAX; }

    changed = (n != s_rc_dev_n) ? 1u : 0u;
    s_rc_dev_n = n;

    /* 【2026-10-19 修复】列表控件的 count 没跟着设备数同步。
     * rc_enter() 初始化时设备数还是 0，之后主循环枚举到从机只改了
     * s_rc_dev_n；s_rc_list.count 一直是 0，于是设备列表永远空白，
     * 编码器也移不动。设备数量变化时同步刷新列表并请求重画。 */
    if ((changed != 0u) && (s_rc_dev == RC_STAGE_LIST))
    {
        UiCList_SetCount(&s_rc_list, s_rc_dev_n);
        UI_Dirty();
    }

    if (s_rc_dev != RC_STAGE_LIST)
    {
        if (s_rc_dev_n == 0u)            { s_rc_dev = RC_STAGE_LIST; }
        else if (s_rc_dev >= s_rc_dev_n) { s_rc_dev = (uint8_t)(s_rc_dev_n - 1u); }
    }
}

uint8_t  UiRc_DeviceCount(void)    { return s_rc_dev_n; }

/* 主循环把第 idx 台从机的名字喂进来（idx 从 0 起） */
void UiRc_SetDeviceName(uint8_t idx, const char *name)
{
    uint8_t i = 0u;
    uint8_t changed = 0u;

    if (idx >= (uint8_t)UIRC_DEV_MAX) { return; }

    if (name == 0)
    {
        changed = (s_rc_name_len[idx] != 0u) ? 1u : 0u;
        s_rc_name_len[idx] = 0u;
        s_rc_name[idx][0] = 0;
    }
    else
    {
        while ((name[i] != 0) && (i < (uint8_t)(UIRC_NAME_MAX - 1u)))
        {
            if (s_rc_name[idx][i] != name[i]) { changed = 1u; }
            s_rc_name[idx][i] = name[i];
            i++;
        }
        if ((i != s_rc_name_len[idx]) || (s_rc_name[idx][i] != 0))
        {
            changed = 1u;
        }
        s_rc_name[idx][i] = 0;
        s_rc_name_len[idx] = i;
    }

    /* 主循环每个超帧都会把名字重新喂一遍；只有真变了才置脏，
     * 否则遥控页会每 16ms 整页重画一次。 */
    if (changed != 0u)
    {
        UI_Dirty();
    }
}
/* 设置/清除遥控页顶部的状态提示（0 = 清除，回到默认文案） */
void UiRc_SetStatus(const char *msg)
{
    s_rc_msg = msg;
}

bool     UiRc_InFunction(void)     { return (s_rc_dev != RC_STAGE_LIST); }
uint8_t  UiRc_SelectedDevice(void) { return s_rc_dev; }
uint16_t UiRc_KeyState(void)       { return s_rc_keys; }
uint8_t  UiRc_Function(void)       { return s_rc_fn; }

/* 松开的按键要从位图上清掉（遥控是"按住才发"） */
void UiRc_ReleaseAll(void) { s_rc_keys = 0u; s_rc_pend = 0u; }

/* 主循环取一次"待发的编码器操作"，取完自动清零 */
uint8_t UiRc_TakePending(void)
{
    uint8_t v = s_rc_pend;
    s_rc_pend = 0u;
    return v;
}


/* 操作变了就查一次表：设备名 -> 表名 -> 动作编号 -> 动作目录里的名称 */
static void rc_refresh_op(uint8_t key_id)
{
    s_rc_op  = key_id;
    s_rc_act = 0u;
    s_rc_actname[0] = 0;

    if ((key_id == 0u) || (s_rc_dev >= (uint8_t)UIRC_DEV_MAX)) { return; }

    s_rc_act = RcTable_ActionOf(s_rc_name[s_rc_dev], key_id);
    if (s_rc_act == 0u) { return; }        /* 没配：屏幕上显示 ACT -- */

    (void)RcTable_NameOfId(s_rc_act, s_rc_actname, (uint8_t)sizeof(s_rc_actname));
}

static void rc_draw(void)
{
    if (s_rc_dev == RC_STAGE_LIST)
    {
        if (UiCList_TakeBg(&s_rc_list) != 0u)
        {
            TFT_FillRect(0, 0, (int16_t)UI_SCREEN_W, (int16_t)UI_SCREEN_H, C_BG);

            if (s_rc_msg != 0)
            {
                p2_title(s_rc_msg);         /* 有故障提示就优先显示它 */
            }
            else
            {
                p2_title((s_rc_dev_n == 0u) ? "SCAN..." : "SELECT DEV");
            }

            UiCList_Draw(&s_rc_list);
        }
        else
        {
            UiCList_DrawText(&s_rc_list);
        }
        return;
    }

    /* 阶段二：整屏重画（不走列表的局部刷新） */
    TFT_FillRect(0, 0, (int16_t)UI_SCREEN_W, (int16_t)UI_SCREEN_H, C_BG);

    /* 9 个按键的功能。设备名标题 + 井字格 + 当前键名 */
    p2_title(s_rc_name[s_rc_dev]);

    UiW_DrawKeyGrid(95, 45, 50, s_rc_keys);

    /* 左侧三行：① 是哪个操作 ② 动作编号 ③ 功能名（查动作目录得来）
     *
     * 【为什么不在这里查 Flash】rc_draw() 每次 UI_Dirty 都会跑，
     * 而 RcTable_NameOfId() 最多要扫 64 条记录（每条读一次 Flash）。
     * 所以改成【操作一变就查一次、存进 s_rc_* 缓存】，这里只画缓存。 */
    {
        char        buf[16];
        const char *lbl;

        switch (s_rc_op)
        {
            case 10u: lbl = "LEFT"; break;   /* 编码器左转 */
            case 11u: lbl = "RIGHT"; break;   /* 编码器右转 */
            case 12u: lbl = "CLICK"; break;   /* 编码器单击 */
            case 0u:  lbl = "---"; break;
            default:
                buf[0] = 'K';
                buf[1] = (char)('0' + (char)s_rc_op);
                buf[2] = 0;
                lbl = buf;
                break;
        }
        p2_ascii_center(52, 6, 84, lbl, 1u, C_GREEN, C_BG);

        if (s_rc_act != 0u)
        {
            buf[0] = 'A'; buf[1] = 'C'; buf[2] = 'T'; buf[3] = ' ';
            buf[4] = (char)('0' + (char)((s_rc_act / 10u) % 10u));
            buf[5] = (char)('0' + (char)(s_rc_act % 10u));
            buf[6] = 0;
        }
        else
        {
            const char *t = "ACT --";
            uint8_t     i = 0u;
            while (t[i] != 0) { buf[i] = t[i]; i++; }
            buf[i] = 0;
        }
        p2_ascii_center(72, 6, 84, buf, 1u, C_TEXT_DIM, C_BG);

        p2_ascii_center(90, 6, 84,
                        (s_rc_actname[0] != 0) ? s_rc_actname : "(no name)",
                        1u, C_AMBER, C_BG);
    }
}

static ui_nav_t rc_key(const GameKeyEvent_t *ev)
{
    uint8_t  k;
    uint16_t bit;

    /* 返回统一走双击（框架全局规则，需求 (六).3）；长按不再当返回用。 */

    /* 阶段一：选设备（这一阶段不关心"松开"） */
    if (s_rc_dev == RC_STAGE_LIST)
    {
        if (ev->type == (uint8_t)GAME_EV_RELEASE) { return UI_NAV_STAY; }
        if (s_rc_dev_n == 0u) { return UI_NAV_STAY; }

        if ((ev->key == GKEY_ENC_CW) || (ev->key == GKEY_DOWN))
        {
            UiCList_Move(&s_rc_list, +1);
            UI_Dirty();
        }
        else if ((ev->key == GKEY_ENC_CCW) || (ev->key == GKEY_LEFT))
        {
            /* 【修复 §8-8】GKEY_UP == GKEY_OK == K5，不能当"上移" */
            UiCList_Move(&s_rc_list, -1);
            UI_Dirty();
        }
        else if ((ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK) ||
                 (ev->key == GKEY_ENC_HOLD))
        {
            s_rc_dev = (uint8_t)UiCList_Selected(&s_rc_list);
            rc_refresh_op(0u);          /* 刚进功能页：先显示三个横杠 */
            UI_Dirty();
        }
        return UI_NAV_STAY;
    }

    /* 阶段二：按键发指令（需求 (四).5）
     *
     * 【修复 §8-3】按下置位、**松开必须清位**：主循环靠位图的边沿
     * （rc_send_ui_keys() 里的 fresh 位）决定什么时候发一条 RC_CMD_SET。
     * 原来这里只置位不清位，UiRc_ReleaseAll() 又没人调 —— 位图一旦置起来
     * 就永远是 1，按第二下被当成"没变化"，同一条指令再也发不出去。 */
    k = (uint8_t)(ev->key - GKEY_K1);           /* GKEY_K1..K9 -> 0..8 */
    if (k < 9u)
    {
        bit = (uint16_t)(1u << k);

        if (ev->type == (uint8_t)GAME_EV_RELEASE)
        {
            s_rc_keys = (uint16_t)(s_rc_keys & (uint16_t)(~bit));
        }
        else
        {
            s_rc_keys = (uint16_t)(s_rc_keys | bit);
            s_rc_fn   = k;
            rc_refresh_op((uint8_t)(k + 1u));   /* 查一次功能名并缓存 */
        }
        UI_Dirty();
    }
    else if (ev->type != (uint8_t)GAME_EV_RELEASE)
    {
        /* 【遥控 12 个操作】编码器左转 / 右转 / 单击 = key_id 10 / 11 / 12。
         *
         * 旋转是"瞬时事件"，没有松开那一拍，所以不进 s_rc_keys 位图，
         * 而是记一个"待发"标志，由主循环取一次就清（UiRc_TakePending()）。
         *
         * 原来这里是"单击 = 回设备列表"。现在改掉了：
         * 单击要留给"第 12 个操作"，返回统一走**双击**
         * （框架全局规则，见 UI_PostKey() 的 GKEY_ENC_DBL）。 */
        if (ev->key == GKEY_ENC_CW)         { s_rc_pend = 11u; }   /* 右转 */
        else if (ev->key == GKEY_ENC_CCW)   { s_rc_pend = 10u; }   /* 左转 */
        else if (ev->key == GKEY_ENC_CLICK) { s_rc_pend = 12u; }   /* 单击 */
        else                                { return UI_NAV_STAY; }

        rc_refresh_op(s_rc_pend);           /* 查一次功能名并缓存 */
        UI_Dirty();
    }
    return UI_NAV_STAY;
}

const ui_page_t UI_PAGE_RC =
    { "rc", rc_enter, 0, rc_key, 0, rc_draw };

/******************* (C) CH592X 后三个页面 *****************/
