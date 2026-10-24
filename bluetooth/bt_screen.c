/**************************************************************************
 * bt_screen.c —— CH592X "附近蓝牙设备"界面实现
 **************************************************************************/

#include "bt_screen.h"

/*==========================================================================
 * 配色 / 布局
 *========================================================================*/
#define BTS_COL_BG      0x0000u
#define BTS_COL_ROW     0x39E7u
#define BTS_COL_SEL     ST77XX_BLUE
#define BTS_COL_TITLE   ST77XX_YELLOW
#define BTS_COL_TEXT    ST77XX_WHITE
#define BTS_COL_RSSI    ST77XX_GREEN
#define BTS_COL_STATUS  ST77XX_CYAN
#define BTS_HINT_Y      (GAME_PANEL_H - GAME_HINT_BAR_H)

/*==========================================================================
 * 内部状态
 *========================================================================*/
static bool    s_need_full = true;
static uint8_t s_rows_dirty = 0xFFu;
static bool    s_status_dirty = true;
static uint8_t s_last_sel = 0xFFu;

/*==========================================================================
 * 画
 *========================================================================*/
static void bts_draw_title(void)
{
    Game_DrawTextCenter(0, GTXT_BT_DEVICES, BTS_COL_TITLE, BTS_COL_BG);
}

static void bts_draw_hint(void)
{
    TFT_FillRect(0, BTS_HINT_Y, GAME_PANEL_W, GAME_HINT_BAR_H, 0x18E3u);
    Game_DrawText(2, (int16_t)(BTS_HINT_Y + 1), GTXT_BT_SET_HOST, BTS_COL_TEXT, 0x18E3u);
    Game_DrawText((int16_t)(GAME_PANEL_W - 34), (int16_t)(BTS_HINT_Y + 1),
                  GTXT_BACK, BTS_COL_TEXT, 0x18E3u);
}

/* 画一行设备 */
static void bts_draw_row(uint8_t i)
{
    const BtDevice_t *d = BT_GetDevice(i);
    int16_t  y = (int16_t)(BTS_ROW_Y0 + (int16_t)i * BTS_ROW_H);
    uint16_t bg = (i == BT_GetSelected()) ? BTS_COL_SEL : BTS_COL_ROW;
    char     buf[20];

    TFT_FillRect(2, y, (int16_t)(GAME_PANEL_W - 4), (int16_t)(BTS_ROW_H - 2), bg);

    if (d == 0)
    {
        return;
    }

    /* 名字：没有名字就显示地址 */
    Game_DrawAscii(6, (int16_t)(y + 4), BT_DevNameOrAddr(d, buf, sizeof(buf)), 1u,
                   BTS_COL_TEXT, bg);

    /* 信号强度：负数，先画一个 '-' 再画数字（右对齐） */
    if (d->rssi != 127)
    {
        int16_t xr = (int16_t)(GAME_PANEL_W - 4);
        uint32_t v = (uint32_t)((d->rssi < 0) ? -(int16_t)d->rssi : d->rssi);

        Game_DrawNumberRight(xr, (int16_t)(y + 4), v, 1u, BTS_COL_RSSI, bg);
        if (d->rssi < 0)
        {
            Game_DrawAscii((int16_t)(xr - Game_NumberWidth(v, 1u) - 6), (int16_t)(y + 4),
                           "-", 1u, BTS_COL_RSSI, bg);
        }
    }
}

static void bts_draw_status(void)
{
    char buf[20];

    TFT_FillRect(0, BTS_STATUS_Y, GAME_PANEL_W, 34, BTS_COL_BG);

    if (BT_IsConnected())
    {
        const BtDevice_t *d = BT_GetConnectedDevice();

        Game_DrawText(4, BTS_STATUS_Y, GTXT_BT_CONNECTED, BTS_COL_STATUS, BTS_COL_BG);
        if (d != 0)
        {
            Game_DrawAscii(54, BTS_STATUS_Y,
                           BT_DevNameOrAddr(d, buf, sizeof(buf)), 1u, BTS_COL_TEXT, BTS_COL_BG);
        }
    }
    else if (BT_IsScanning())
    {
        Game_DrawText(4, BTS_STATUS_Y, GTXT_BT_SCAN, BTS_COL_STATUS, BTS_COL_BG);
    }
    else if (BT_GetDeviceCount() == 0u)
    {
        Game_DrawText(4, BTS_STATUS_Y, GTXT_BT_NODEV, BTS_COL_TEXT, BTS_COL_BG);
    }
    else
    {
        Game_DrawAscii(4, BTS_STATUS_Y, "K7 = rescan", 1u, BTS_COL_TEXT, BTS_COL_BG);
    }

    /* 第二行：宿主（白名单）。只有它能连我们 */
    {
        const BtDevice_t *host = BT_GetHost();
        if (host != 0)
        {
            Game_DrawText(4, (int16_t)(BTS_STATUS_Y + 17), GTXT_BT_HOST, BTS_COL_STATUS, BTS_COL_BG);
            Game_DrawAscii(42, (int16_t)(BTS_STATUS_Y + 17),
                           BT_DevNameOrAddr(host, buf, sizeof(buf)), 1u, BTS_COL_TEXT, BTS_COL_BG);
        }
    }
}

/*==========================================================================
 * 对外接口
 *========================================================================*/
void BT_SCREEN_Invalidate(void)
{
    s_need_full = true;
    s_rows_dirty = 0xFFu;
    s_status_dirty = true;
    s_last_sel = 0xFFu;
}

void BT_SCREEN_Enter(void)
{
    Game_SetOrientation(GAME_PORTRAIT);
    Game_Clear(BTS_COL_BG);

    BT_ClearDevices();
    (void)BT_StartScan();

    BT_SCREEN_Invalidate();
    BT_SCREEN_Draw();
}

BtScreenAction_t BT_SCREEN_HandleKey(const GameKeyEvent_t *ev)
{
    if (ev == 0)
    {
        return BT_SCREEN_NONE;
    }

    /* 只有按下/连发才认，松开不处理 */
    if (ev->type == GAME_EV_RELEASE)
    {
        return BT_SCREEN_NONE;
    }

    switch ((int)ev->key)
    {
        case GKEY_ENC_CW:
        case GKEY_K6:
            BT_SelectNext(1);
            break;

        case GKEY_ENC_CCW:
        case GKEY_K4:
            BT_SelectNext(-1);
            break;

        case GKEY_K5:
        case GKEY_ENC_BTN:
            return BT_SCREEN_SET_HOST;

        case GKEY_K7:
            return BT_SCREEN_RESCAN;

        case GKEY_K9:
            return BT_SCREEN_BACK;

        default:
            break;
    }

    return BT_SCREEN_NONE;
}

void BT_SCREEN_Update(uint16_t dt_ms)
{
    BtEventType_t ev;
    uint8_t       want_rows = 0u;

    BT_Task(dt_ms);

    /* 有没有需要重画的行（新设备 / 选项变化） */
    if (s_last_sel != BT_GetSelected())
    {
        s_last_sel = BT_GetSelected();
        want_rows = 0xFFu;
    }

    while (BT_PollEvent(&ev))
    {
        switch (ev)
        {
            case BT_EV_DEVICE_FOUND:
                want_rows = 0xFFu;
                s_status_dirty = true;
                break;

            case BT_EV_SCAN_DONE:
            case BT_EV_CONNECTED:
            case BT_EV_DISCONNECTED:
            case BT_EV_HID_READY:
                s_status_dirty = true;
                break;

            default:
                break;
        }
    }

    if (want_rows != 0u)
    {
        s_rows_dirty |= want_rows;
    }
}

void BT_SCREEN_Draw(void)
{
    uint8_t i;

    if (s_need_full)
    {
        Game_Clear(BTS_COL_BG);
        bts_draw_title();
        bts_draw_hint();
        s_need_full = false;
        s_rows_dirty = 0xFFu;
        s_status_dirty = true;
    }

    for (i = 0; i < BTS_ROWS; i++)
    {
        if ((s_rows_dirty & (uint8_t)(1u << i)) != 0u)
        {
            bts_draw_row(i);
        }
    }
    s_rows_dirty = 0u;

    if (s_status_dirty)
    {
        bts_draw_status();
        s_status_dirty = false;
    }
}

/******************* (C) CH592X 蓝牙设备界面 *****************/
