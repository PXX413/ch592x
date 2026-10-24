/**************************************************************************
 * link_keymap.c —— 9 键功能名称通道实现
 **************************************************************************/

#include "link_keymap.h"
#include <string.h>

/*==========================================================================
 * 片内偏移
 *========================================================================*/
#define FRAG_OFF_CTRL           0u
#define FRAG_OFF_DEV            1u
#define FRAG_OFF_VER            2u
#define FRAG_OFF_TOTAL          3u
#define FRAG_OFF_INDEX          4u
#define FRAG_OFF_PLEN           5u
#define FRAG_OFF_PAYLOAD        6u
#define FRAG_OFF_CRC            31u

/* 改名帧偏移 */
#define REN_OFF_CTRL            0u
#define REN_OFF_DEV             1u
#define REN_OFF_KEY             2u
#define REN_OFF_ACT             3u
#define REN_OFF_LEN             4u
#define REN_OFF_NAME            5u
#define REN_OFF_CRC             31u

/* 一个 TLV 条目的固定开销：key_id + act + len */
#define TLV_HDR                 3u

/*==========================================================================
 * 默认配置
 *========================================================================*/
void Keymap_Default(rc_keymap_t *m, const char *dev_name)
{
    uint8_t i;
    uint8_t n = 0u;

    if (m == 0)
    {
        return;
    }

    memset(m, 0, sizeof(*m));

    if (dev_name != 0)
    {
        while ((dev_name[n] != '\0') && (n < (uint8_t)RC_DEV_NAME_MAX))
        {
            m->dev_name[n] = dev_name[n];
            n++;
        }
    }
    m->dev_name[n]    = '\0';
    m->dev_name_len   = n;

    for (i = 0; i < (uint8_t)RC_KEY_COUNT; i++)
    {
        /* 默认名 "KEY1".."KEY9"，行为 MOMENTARY，9 键都在 */
        m->key[i].name[0] = 'K';
        m->key[i].name[1] = 'E';
        m->key[i].name[2] = 'Y';
        m->key[i].name[3] = (char)('1' + i);
        m->key[i].name[4] = '\0';
        m->key[i].len     = 4u;
        m->key[i].act     = RC_ACT_BYTE(ACT_MOMENTARY, 0u);
    }

    m->key_mask   = (uint16_t)((1u << RC_KEY_COUNT) - 1u);  /* 9 个 bit 全 1 */
    m->config_ver = 1u;
}

/*==========================================================================
 * 名称合法性（规范 18.2）
 *========================================================================*/
uint8_t Keymap_ValidateName(const char *name, uint8_t len)
{
    uint8_t i;

    if (len > (uint8_t)RC_KEY_NAME_MAX)
    {
        return RC_E_LEN;
    }

    if (len == 0u)
    {
        return RC_OK;                       /* 空名字是合法的（len=0） */
    }

    if (name == 0)
    {
        return RC_E_PARAM;
    }

    for (i = 0; i < len; i++)
    {
        uint8_t c = (uint8_t)name[i];

        if (c == 0u)
        {
            return RC_E_NAME_INVALID;       /* 中间出现 '\0' 视为攻击 */
        }
        if ((c < 0x20u) || (c > 0x7Eu))
        {
            return RC_E_NAME_INVALID;       /* v1.0 不收 UTF-8 / 控制字符 */
        }
        if ((c == '/') || (c == '\\') || (c == '%'))
        {
            return RC_E_NAME_INVALID;       /* 禁止路径 / 格式字符 */
        }
    }

    return RC_OK;
}
/*==========================================================================
 * Flash 名称区（规范 11.2 的 0x026..0x0F4，共 207 字节）
 *   +0    key_name_len[9]
 *   +9    key_name_pool[9][20]
 *   +189  key_action[9]
 *   +198  key_scene_param[9]
 *========================================================================*/
#define NAME_OFF_LEN            0u
#define NAME_OFF_POOL           9u
#define NAME_OFF_ACT            189u
#define NAME_OFF_SCENE          198u

void Keymap_PackNames(const rc_keymap_t *m, uint8_t *out)
{
    uint8_t i;

    if ((m == 0) || (out == 0))
    {
        return;
    }

    memset(out, 0, RC_FLASH_NAME_REGION_LEN);

    for (i = 0; i < (uint8_t)RC_KEY_COUNT; i++)
    {
        uint8_t len = m->key[i].len;
        uint8_t j;

        if (len > (uint8_t)RC_KEY_NAME_MAX)
        {
            len = 0u;                       /* 不存在的键：长度记 0 */
        }

        out[NAME_OFF_LEN + i] = len;

        /* 物理不存在的键：行为一律规范化成 RESERVED，
         * 这样 len==0 && act==RESERVED 的"不存在"表示才能稳定往返 */
        if (m->key[i].len == (uint8_t)RC_KEY_LEN_ABSENT)
        {
            len = 0u;
            out[NAME_OFF_LEN + i] = 0u;
            out[NAME_OFF_ACT + i] = (uint8_t)ACT_RESERVED;
            out[NAME_OFF_SCENE + i] = 0u;
            continue;
        }

        for (j = 0; j < len; j++)
        {
            out[NAME_OFF_POOL + (uint16_t)i * 20u + j] = (uint8_t)m->key[i].name[j];
        }

        out[NAME_OFF_ACT   + i] = RC_ACT_GET(m->key[i].act);
        out[NAME_OFF_SCENE + i] = RC_SCENE_GET(m->key[i].act);
    }
}

void Keymap_UnpackNames(const uint8_t *in, rc_keymap_t *m)
{
    uint8_t  i;
    uint16_t mask = 0u;

    if ((in == 0) || (m == 0))
    {
        return;
    }

    for (i = 0; i < (uint8_t)RC_KEY_COUNT; i++)
    {
        uint8_t len = in[NAME_OFF_LEN + i];
        uint8_t j;

        if (len > (uint8_t)RC_KEY_NAME_MAX)
        {
            /* 长度字段坏了：认成"没配名字"，不让它越界 */
            len = 0u;
        }

        for (j = 0; j < len; j++)
        {
            m->key[i].name[j] = (char)in[NAME_OFF_POOL + (uint16_t)i * 20u + j];
        }
        m->key[i].name[len] = '\0';

        /* 长度 0 且 action 是 RESERVED → 该键物理不存在（规范 6.4） */
        if ((len == 0u) && (in[NAME_OFF_ACT + i] == (uint8_t)ACT_RESERVED))
        {
            m->key[i].len = (uint8_t)RC_KEY_LEN_ABSENT;
        }
        else
        {
            m->key[i].len = len;
        }

        m->key[i].act = RC_ACT_BYTE(in[NAME_OFF_ACT + i], in[NAME_OFF_SCENE + i]);

        if (m->key[i].len != (uint8_t)RC_KEY_LEN_ABSENT)
        {
            mask |= (uint16_t)(1u << i);
        }
    }

    m->key_mask = mask;
}

/*==========================================================================
 * 小工具
 *========================================================================*/
void Key_DisplayName(const rc_key_t *k, uint8_t key_id, char *out, uint8_t cap)
{
    uint8_t n = 0u;

    if ((k == 0) || (out == 0) || (cap == 0u))
    {
        return;
    }

    if (k->len == (uint8_t)RC_KEY_LEN_ABSENT)
    {
        out[0] = '\0';                       /* 物理不存在：隐藏 */
        return;
    }

    if (k->len > 0u)
    {
        while ((n < k->len) && (n < (uint8_t)(cap - 1u)))
        {
            out[n] = k->name[n];
            n++;
        }
    }
    else
    {
        /* 存在但没配名字：显示 "KEYn" */
        n = 0u;
        if (n < (uint8_t)(cap - 1u)) { out[n++] = 'K'; }
        if (n < (uint8_t)(cap - 1u)) { out[n++] = 'E'; }
        if (n < (uint8_t)(cap - 1u)) { out[n++] = 'Y'; }
        if (n < (uint8_t)(cap - 1u)) { out[n++] = (char)('0' + (key_id % 10u)); }
    }

    out[n] = '\0';
}
