/**************************************************************************
 * ui_home.c —— 主页（磁贴轮播）
 *
 * 轮播的绘制/动画/局部刷新都在 ui_carousel 里，本文件只负责：
 *   * 定义四个入口（键盘 / 游戏 / 遥控 / 设置）各自的图标 + 本色
 *   * 编码器左右循环切换
 *   * OK 进对应页面
 **************************************************************************/

#include "ui_page.h"
#include "ui_theme.h"
#include "ui_carousel.h"
#include "icon_mode_60x60.h"
#include "game_port.h"
#include "game_text.h"

/* 四个入口：磁贴位图（60x60 彩色圆角图标）/ 名字 / 主题色
 * 主题色只用来画底栏顶线和状态栏强调线，和图标自己的渐变色调一致。 */
static const uicar_item_t s_items[4] =
{
    { keyboard_mode_60x60, GTXT_KB_MODE,   0x2DFFu, 0u },   /* 键盘：蓝 */
    { game_mode_60x60,     GTXT_GAME_MODE, 0xFD20u, 0u },   /* 游戏：橙 */
    { remote_mode_60x60,   GTXT_RC_MODE,   0x2FE6u, 0u },   /* 遥控：青 */
    { settings_mode_60x60, GTXT_SETTINGS,  0xA99Fu, 0u }    /* 设置：紫 */
};

static uicar_t s_car;

static void home2_enter(void *arg)
{
    (void)arg;
    UiCar_Init(&s_car, s_items, 4u, GTXT_MENU);
    UiCar_Enter(&s_car);
}

static void home2_leave(void)
{
    /* 没什么要收的 */
}

static ui_nav_t home2_key(const GameKeyEvent_t *ev)
{
    /* 【2026-10-13】REPEAT 也要挡掉！原来只挡了 RELEASE，
     * 结果长按产生的连发事件会被当成"又一次操作"，反复进页。
     * 本页的"上/下/选"都是一次性动作，不需要连发。 */
    if ((ev->type == (uint8_t)GAME_EV_RELEASE) ||
        (ev->type == (uint8_t)GAME_EV_REPEAT))
    {
        return UI_NAV_STAY;
    }

    /* 左：编码器逆时针 / 左键 */
    if ((ev->key == GKEY_ENC_CCW) || (ev->key == GKEY_LEFT))
    {
        UiCar_Step(&s_car, -1);
        return UI_NAV_STAY;
    }

    /* 右：编码器顺时针 / 右键 */
    if ((ev->key == GKEY_ENC_CW) || (ev->key == GKEY_RIGHT))
    {
        UiCar_Step(&s_car, +1);
        return UI_NAV_STAY;
    }

    /* OK：闪一下 + 进对应页 */
    if ((ev->key == GKEY_ENC_CLICK) || (ev->key == GKEY_OK) ||
        (ev->key == GKEY_ENC_BTN) ||
        (ev->key == GKEY_ENC_HOLD))     /* 需求 (六).4：长按 0.5s 也执行当前操作 */
    {
        UiCar_Tap(&s_car);

        switch (UiCar_Sel(&s_car))
        {
            case 0:  UI_Push(&UI_PAGE_KBD,     0); break;
            case 1:  UI_Push(&UI_PAGE_GAME,    0); break;
            case 2:  UI_Push(&UI_PAGE_RC,      0); break;
            default: UI_Push(&UI_PAGE_SETTING, 0); break;
        }
        return UI_NAV_STAY;
    }

    return UI_NAV_STAY;
}

static void home2_update(uint16_t dt_ms)
{
    UiCar_Update(&s_car, dt_ms);
}

static void home2_draw(void)
{
    UiCar_Draw(&s_car);
}

const ui_page_t UI_PAGE_HOME =
{
    "home",
    home2_enter,
    home2_leave,
    home2_key,
    home2_update,
    home2_draw
};
