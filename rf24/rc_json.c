/**************************************************************************
 * rc_json.c —— JSON 导出实现
 *
 * 这里不用 snprintf 拼接（省 Flash 也省栈），而是自己往缓冲区里追加。
 **************************************************************************/

#include "rc_json.h"
#include <string.h>

/*==========================================================================
 * 一个很朴素的可截断输出器
 *========================================================================*/
typedef struct
{
    char    *buf;
    uint16_t cap;
    uint16_t len;
} js_out_t;

static void js_putc(js_out_t *o, char c)
{
    if ((uint16_t)(o->len + 1u) < o->cap)
    {
        o->buf[o->len] = c;
        o->len++;
        o->buf[o->len] = '\0';
    }
}

static void js_puts(js_out_t *o, const char *s)
{
    if (s == 0)
    {
        return;
    }
    while (*s != '\0')
    {
        js_putc(o, *s);
        s++;
    }
}

static void js_putu(js_out_t *o, uint32_t v)
{
    char    tmp[11];
    uint8_t n = 0u;

    if (v == 0u)
    {
        js_putc(o, '0');
        return;
    }

    while ((v > 0u) && (n < (uint8_t)sizeof(tmp)))
    {
        tmp[n] = (char)('0' + (char)(v % 10u));
        v /= 10u;
        n++;
    }

    while (n > 0u)
    {
        n--;
        js_putc(o, tmp[n]);
    }
}

static void js_puti(js_out_t *o, int32_t v)
{
    if (v < 0)
    {
        js_putc(o, '-');
        js_putu(o, (uint32_t)(-v));
    }
    else
    {
        js_putu(o, (uint32_t)v);
    }
}

/*==========================================================================
 * 转义
 *========================================================================*/
uint16_t RcJson_Escape(const char *in, uint8_t len, char *out, uint16_t cap)
{
    uint16_t n = 0u;
    uint8_t  i;

    if ((in == 0) || (out == 0) || (cap == 0u))
    {
        return 0u;
    }

    for (i = 0u; i < len; i++)
    {
        char c = in[i];

        if ((c == '"') || (c == '\\'))
        {
            if ((uint16_t)(n + 2u) >= cap)
            {
                break;
            }
            out[n++] = '\\';
            out[n++] = c;
        }
        else if (((uint8_t)c < 0x20u) || ((uint8_t)c > 0x7Eu))
        {
            if ((uint16_t)(n + 1u) >= cap)
            {
                break;
            }
            out[n++] = ' ';                 /* 控制字符/非 ASCII 一律换成空格 */
        }
        else
        {
            if ((uint16_t)(n + 1u) >= cap)
            {
                break;
            }
            out[n++] = c;
        }
    }

    out[n] = '\0';
    return n;
}

static void js_putesc(js_out_t *o, const char *s, uint8_t len)
{
    uint8_t i;

    for (i = 0u; i < len; i++)
    {
        char c = s[i];

        if ((c == '"') || (c == '\\'))
        {
            js_putc(o, '\\');
            js_putc(o, c);
        }
        else if (((uint8_t)c < 0x20u) || ((uint8_t)c > 0x7Eu))
        {
            js_putc(o, ' ');
        }
        else
        {
            js_putc(o, c);
        }
    }
}

/*==========================================================================
 * 行为名
 *========================================================================*/
const char *RcJson_ActionName(uint8_t action)
{
    switch (action)
    {
        case ACT_MOMENTARY: return "MOMENTARY";
        case ACT_TOGGLE:    return "TOGGLE";
        case ACT_LATCH:     return "LATCH";
        case ACT_INCREMENT: return "INCREMENT";
        case ACT_DECREMENT: return "DECREMENT";
        case ACT_SCENE:     return "SCENE";
        case ACT_MACRO:     return "MACRO";
        case ACT_RESERVED:  return "NONE";
        default:            return "UNKNOWN";
    }
}

/*==========================================================================
 * 导出
 *========================================================================*/
uint16_t RcJson_Export(const rc_host_t *h, char *buf, uint16_t cap)
{
    js_out_t o;
    uint8_t  i;
    uint8_t  n;
    uint8_t  k;   /* 已 (void)，见下 */

    (void)k;                /* 键数组已空，下面会 (void) 掉 */
    uint8_t  first;

    if ((h == 0) || (buf == 0) || (cap < 2u))
    {
        return 0u;
    }

    o.buf = buf;
    o.cap = cap;
    o.len = 0u;
    buf[0] = '\0';

    js_puts(&o, "{\"devices\":[");

    n     = Rc_HostCount(h);
    first = 1u;

    for (i = 0u; i < n; i++)
    {
        const rc_host_dev_t *d = Rc_HostDev((rc_host_t *)h, i);

        if (d == 0)
        {
            continue;
        }

        if (first == 0u)
        {
            js_putc(&o, ',');
        }
        first = 0u;

        js_puts(&o, "{\"dev_id\":");
        js_putu(&o, d->dev_id);

        js_puts(&o, ",\"name\":\"");
        js_putesc(&o, d->dev_name, d->dev_name_len);
        js_putc(&o, '"');

        js_puts(&o, ",\"hw_ver\":");
        js_putu(&o, d->hw_ver);
        js_puts(&o, ",\"sw_ver\":");
        js_putu(&o, d->sw_ver);

        js_puts(&o, ",\"rssi\":");
        js_puti(&o, d->rssi);

        js_puts(&o, ",\"battery_mv\":");
        js_putu(&o, d->batt_mv);

        js_puts(&o, ",\"online\":");
        js_putu(&o, (d->phase == RC_DEV_OFFLINE) ? 0u : 1u);

        /* 【协议瘦身】9 键名称不再从从机上传，主机也不驻留；
         * 键名/功能名现在由主机的"动作编号表"提供（见 rc_table.c），
         * 网页改从那边取。这里保留一个空数组只为不破坏现有 JSON 结构。 */
        js_puts(&o, ",\"keys\":[]");
        js_puts(&o, "]}");
    }

    js_puts(&o, "]}");

    return o.len;
}
