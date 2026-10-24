/**************************************************************************
 * ui_enc.c —— 编码器点击分类器实现
 **************************************************************************/

#include "ui_enc.h"
#include "game_port.h"

void UiEnc_Reset(ui_enc_t *e)
{
    if (e == 0)
    {
        return;
    }

    e->last        = 0u;
    e->pressed     = 0u;
    e->hold_fired  = 0u;
    e->hold1_fired = 0u;
    e->hold3_fired = 0u;
    e->wait_dbl    = 0u;
    e->press_ms    = 0u;
    e->release_ms  = 0u;
    e->started     = 0u;
}

ui_enc_evt_t UiEnc_Step(ui_enc_t *e, uint8_t pressed, uint32_t now_ms)
{
    uint32_t held;

    if (e == 0)
    {
        return UI_ENC_NONE;
    }

    pressed = (pressed != 0u) ? 1u : 0u;

    /* 第一次调用只做对齐，不产生事件 —— 免得开机时把"本来就按着"误判成一次点击 */
    if (e->started == 0u)
    {
        e->started = 1u;
        e->last    = pressed;
        if (pressed != 0u)
        {
            e->press_ms     = now_ms;
            e->hold_fired   = 0u;
            e->hold1_fired  = 0u;
            e->hold3_fired  = 0u;
        }
        e->pressed = pressed;
        return UI_ENC_NONE;
    }

    /* ---------------- 按下边沿 ---------------- */
    if ((pressed != 0u) && (e->last == 0u))
    {
        e->pressed      = 1u;
        e->press_ms     = now_ms;
        e->hold_fired   = 0u;
        e->hold1_fired  = 0u;
        e->hold3_fired  = 0u;

        if (e->wait_dbl != 0u)
        {
            /* 第二击来了：双击。
             * 打上 dbl_consumed 标记 —— 否则这一下松开时又会武装一次
             * wait_dbl，等窗口过期就白送一个 CLICK（一次双击变成"双击+单击"）。 */
            e->wait_dbl     = 0u;
            e->dbl_consumed = 1u;
            e->last         = pressed;
            return UI_ENC_DOUBLE;
        }

        e->last = pressed;
        return UI_ENC_NONE;
    }

    /* ---------------- 按住期间 ---------------- */
    if ((pressed != 0u) && (e->last != 0u))
    {
        held = now_ms - e->press_ms;

        /* 三个档位各只吐一次：0.5s 的 HOLD、1s 的 HOLD1、3s 的 HOLD3。
         * 页面只接自己关心的那一档（键盘页只接 HOLD1）。 */
        if ((e->hold3_fired == 0u) && (held >= (uint32_t)UI_ENC_HOLD3_MS))
        {
            e->hold3_fired = 1u;
            e->last        = pressed;
            return UI_ENC_HOLD3;
        }

        if ((e->hold1_fired == 0u) && (held >= (uint32_t)UI_ENC_HOLD1_MS))
        {
            e->hold1_fired = 1u;
            e->last        = pressed;
            return UI_ENC_HOLD1;
        }

        if ((e->hold_fired == 0u) && (held >= (uint32_t)UI_ENC_HOLD_MS))
        {
            e->hold_fired = 1u;
            e->last       = pressed;
            return UI_ENC_HOLD;
        }

        e->last = pressed;
        return UI_ENC_NONE;
    }

    /* ---------------- 松开边沿 ---------------- */
    if ((pressed == 0u) && (e->last != 0u))
    {
        e->pressed = 0u;
        e->last    = 0u;

        if (e->dbl_consumed != 0u)
        {
            /* 这一按是拿来凑双击的，松开后不再产生任何事件 */
            e->dbl_consumed = 0u;
            e->hold_fired   = 0u;
            e->hold1_fired  = 0u;
            e->hold3_fired  = 0u;
            return UI_ENC_NONE;
        }

        if ((e->hold_fired != 0u) || (e->hold1_fired != 0u) || (e->hold3_fired != 0u))
        {
            /* 任一档长按已经报过了，松开不再算点击 */
            e->wait_dbl    = 0u;
            e->hold_fired  = 0u;
            e->hold1_fired = 0u;
            e->hold3_fired = 0u;
            return UI_ENC_NONE;
        }

        e->wait_dbl   = 1u;
        e->release_ms = now_ms;
        return UI_ENC_NONE;
    }

    /* ---------------- 空闲：等第二击超时 ---------------- */
    if ((pressed == 0u) && (e->last == 0u))
    {
        if (e->wait_dbl != 0u)
        {
            if ((uint32_t)(now_ms - e->release_ms) >= UI_ENC_DOUBLE_MS)
            {
                e->wait_dbl = 0u;
                return UI_ENC_CLICK;
            }
        }
    }

    e->last = pressed;
    return UI_ENC_NONE;
}

uint8_t UiEnc_ToGameKey(ui_enc_evt_t evt)
{
    switch (evt)
    {
        case UI_ENC_CLICK:  return (uint8_t)GKEY_ENC_CLICK;
        case UI_ENC_DOUBLE: return (uint8_t)GKEY_ENC_DBL;
        case UI_ENC_HOLD:   return (uint8_t)GKEY_ENC_HOLD;
        case UI_ENC_HOLD1:  return (uint8_t)GKEY_ENC_HOLD1;
        case UI_ENC_HOLD3:  return (uint8_t)GKEY_ENC_HOLD3;
        default:            return (uint8_t)GKEY_NONE;
    }
}
