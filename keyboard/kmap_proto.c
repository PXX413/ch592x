/**************************************************************************
 * kmap_proto.c —— 按键映射配置协议实现（设备端）
 *
 * 见 kmap_proto.h 的帧格式说明。
 *
 * 这里的解析器刻意写成「逐字节喂」的形态：
 *   - 不依赖一次收到完整帧（串口可能半包到）
 *   - 收到脏数据能自己重新同步
 *   - 上层只管 while(有字节) { if (Kmap_ParserPush(...)) 处理帧; }
 **************************************************************************/

#include "kmap_proto.h"
#include <string.h>

/*==========================================================================
 * CRC8（poly 0x07，初值 0，不反转）
 *   和 rf24/proto_common.c 的 crc8 是同一个算法，故意保持一致。
 *========================================================================*/
uint8_t Kmap_Crc8(const uint8_t *p, uint16_t len)
{
    uint8_t crc = 0x00u;
    uint16_t i;
    uint8_t  b;

    if (p == 0)
    {
        return 0u;
    }

    for (i = 0u; i < len; i++)
    {
        crc ^= p[i];
        for (b = 0u; b < 8u; b++)
        {
            if ((crc & 0x80u) != 0u)
            {
                crc = (uint8_t)((uint8_t)(crc << 1) ^ KMAP_CRC8_POLY);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

/*==========================================================================
 * 组帧
 *========================================================================*/
uint16_t Kmap_BuildFrame(uint8_t *out, uint8_t cmd, uint8_t seq,
                         const uint8_t *payload, uint16_t len)
{
    uint16_t n;
    uint8_t  crc;

    if (out == 0)
    {
        return 0u;
    }
    if (len > (uint16_t)KMAP_MAX_PAYLOAD)
    {
        return 0u;
    }
    if ((len > 0u) && (payload == 0))
    {
        return 0u;
    }

    n = 0u;
    out[n++] = KMAP_SOF0;
    out[n++] = KMAP_SOF1;
    out[n++] = cmd;
    out[n++] = seq;
    Kmap_PutU16(&out[n], len);
    n = (uint16_t)(n + 2u);

    if (len > 0u)
    {
        memcpy(&out[n], payload, len);
        n = (uint16_t)(n + len);
    }

    /* CRC 覆盖【帧头到负载末尾】 */
    crc = Kmap_Crc8(out, n);
    out[n++] = crc;

    return n;
}

/*==========================================================================
 * 解析器
 *========================================================================*/
void Kmap_ParserInit(kmap_parser_t *ps)
{
    if (ps == 0)
    {
        return;
    }
    memset(ps, 0, sizeof(*ps));
}

/* 把收到的这一帧收尾：校验 + 填 out + 更新统计 */
static bool kmap_finish(kmap_parser_t *ps, kmap_frame_t *out)
{
    uint16_t paylen;
    uint16_t need;
    uint8_t  crc_calc;
    uint8_t  crc_recv;

    if (ps->len < 7u)
    {
        ps->frames_bad++;
        return false;                   /* 太短，不可能完整 */
    }

    paylen = Kmap_GetU16(&ps->buf[4]);
    if (paylen > (uint16_t)KMAP_MAX_PAYLOAD)
    {
        ps->frames_bad++;
        return false;
    }

    need = (uint16_t)(7u + paylen);      /* 头7 + 负载 + CRC1 */
    if (ps->len != need)
    {
        ps->frames_bad++;
        return false;
    }

    /* CRC 覆盖除最后一字节外的全部 */
    crc_calc = Kmap_Crc8(ps->buf, (uint16_t)(need - 1u));
    crc_recv = ps->buf[need - 1u];
    if (crc_calc != crc_recv)
    {
        ps->frames_bad++;
        return false;
    }

    if (out != 0)
    {
        out->cmd = ps->buf[2];
        out->seq = ps->buf[3];
        out->len = paylen;
        if (paylen > 0u)
        {
            memcpy(out->payload, &ps->buf[6], paylen);
        }
    }

    ps->frames_ok++;
    return true;
}

/*********************************************************************
 * @brief  逐字节推进状态机
 *
 * 状态就用 ps->len 表示：
 *   len == 0  还没收到帧头，找 0xA5
 *   len == 1  收到了 0xA5，等 0x5A（不是就退回）
 *   len >= 2  正在攒帧，攒够了就校验
 *********************************************************************/
bool Kmap_ParserPush(kmap_parser_t *ps, uint8_t byte, kmap_frame_t *out)
{
    uint16_t paylen;
    uint16_t need;

    if (ps == 0)
    {
        return false;
    }

    ps->bytes_in++;

    /* ---- 找帧头 ---- */
    if (ps->len == 0u)
    {
        if (byte == KMAP_SOF0)
        {
            ps->buf[0] = byte;
            ps->len = 1u;
        }
        return false;                   /* 不是帧头就丢掉 */
    }

    if (ps->len == 1u)
    {
        if (byte == KMAP_SOF1)
        {
            ps->buf[1] = byte;
            ps->len = 2u;
        }
        else if (byte == KMAP_SOF0)
        {
            /* 连着好几个 0xA5：停在"刚收到第一个"的状态 */
            ps->buf[0] = byte;
            ps->len = 1u;
        }
        else
        {
            ps->len = 0u;               /* 退回找帧头 */
        }
        return false;
    }

    /* ---- 攒帧 ---- */
    if (ps->len >= (uint16_t)KMAP_MAX_FRAME)
    {
        /* 缓冲满了还没成帧，说明前面数据不对，复位重来 */
        ps->frames_bad++;
        ps->len = 0u;
        return false;
    }

    ps->buf[ps->len] = byte;
    ps->len++;

    /* 收到 LEN 字段（buf[5]）后，就能算出整帧要多少字节 */
    if (ps->len < 6u)
    {
        return false;
    }

    paylen = Kmap_GetU16(&ps->buf[4]);
    if (paylen > (uint16_t)KMAP_MAX_PAYLOAD)
    {
        /* 长度字段不合法，这帧废了，重同步 */
        ps->frames_bad++;
        ps->len = 0u;
        return false;
    }

    need = (uint16_t)(7u + paylen);

    if (ps->len < need)
    {
        return false;                   /* 还没收齐 */
    }

    /* 攒齐了：校验并出帧 */
    {
        bool ok = kmap_finish(ps, out);
        ps->len = 0u;                   /* 不管成败都复位，准备下一帧 */
        return ok;
    }
}
