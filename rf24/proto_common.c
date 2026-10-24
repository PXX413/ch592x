/**************************************************************************
 * proto_common.c —— 2.4G 私有协议公共层实现
 *
 * 只做三件事：CRC、帧长度表、地址拼装。没有硬件依赖。
 **************************************************************************/

#include "proto_common.h"

/*==========================================================================
 * CRC
 *========================================================================*/

/*********************************************************************
 * @fn      Rf_Crc8
 * @brief   CRC-8：poly 0x07，初值 0x00，MSB first，不反转，不异或
 *********************************************************************/
uint8_t Rf_Crc8(const uint8_t *p, uint16_t n)
{
    uint8_t crc = (uint8_t)RF_CRC8_INIT;
    uint16_t i;
    uint8_t  b;

    if (p == 0)
    {
        return crc;
    }

    for (i = 0; i < n; i++)
    {
        crc ^= p[i];

        for (b = 0; b < 8u; b++)
        {
            if ((crc & 0x80u) != 0u)
            {
                crc = (uint8_t)((uint8_t)(crc << 1) ^ 0x07u);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

/*********************************************************************
 * @fn      Rf_Crc16
 * @brief   CRC-16/CCITT-FALSE：poly 0x1021，初值 0xFFFF，MSB first
 * @note    和工程里 game_score.c 用的算法完全一致
 *********************************************************************/
uint16_t Rf_Crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = (uint16_t)RF_CRC16_INIT;
    uint16_t i;
    uint8_t  b;

    if (p == 0)
    {
        return crc;
    }

    for (i = 0; i < n; i++)
    {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);

        for (b = 0; b < 8u; b++)
        {
            if ((crc & 0x8000u) != 0u)
            {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

/*==========================================================================
 * 帧长度表
 *========================================================================*/
uint8_t Rf_FrameLen(uint8_t frame_ctrl)
{
    switch (FRAME_TYPE(frame_ctrl))
    {
        /* ---- 键盘模式 ---- */
        case KB_ACK:            return (uint8_t)KB_ACK_LEN;
        case KB_POLL:           return (uint8_t)KB_POLL_LEN;
        case KB_HID_6KRO:       return (uint8_t)KB_HID_6KRO_LEN;
        case KB_HID_NKRO:       return (uint8_t)KB_HID_NKRO_LEN;
        case KB_RETRIEVE:       return (uint8_t)KB_RETRIEVE_LEN;
        case KB_LED:            return (uint8_t)KB_LED_LEN;
        case KB_PAIR:           return (uint8_t)KB_PAIR_LEN;

        /* ---- 遥控模式 ---- */
        case RC_SYNC:           return (uint8_t)RC_SYNC_LEN;
        case RC_CMD:            return (uint8_t)RC_CMD_LEN;
        case RC_ENUM_RSP:       return (uint8_t)RC_ENUM_RSP_LEN;
        case RC_ENUM_ACK:       return (uint8_t)RC_ENUM_ACK_LEN;
        case RC_SLEEP:          return (uint8_t)RC_SLEEP_LEN;
        case RC_RESET:          return (uint8_t)RC_RESET_LEN;
        case RC_PING:           return (uint8_t)RC_PING_LEN;

        case RC_REPORT:         return (uint8_t)RC_REPORT_LEN;
        case RC_BATT:           return (uint8_t)RC_BATT_LEN;
        case RC_TEMP:           return (uint8_t)RC_TEMP_LEN;
        case RC_EMERGENCY:      return (uint8_t)RC_EMERGENCY_LEN;
        case RC_SET_SCENE:      return (uint8_t)RC_SET_SCENE_LEN;

        default:                return 0u;
    }
}

/*==========================================================================
 * 帧名字表（调试打印，纯 ASCII）
 *========================================================================*/
const char *Rf_FrameName(uint8_t frame_ctrl)
{
    switch (FRAME_TYPE(frame_ctrl))
    {
        case KB_ACK:            return "KB_ACK";
        case KB_POLL:           return "KB_POLL";
        case KB_HID_6KRO:       return "KB_HID_6KRO";
        case KB_HID_NKRO:       return "KB_HID_NKRO";
        case KB_RETRIEVE:       return "KB_RETRIEVE";
        case KB_LED:            return "KB_LED";
        case KB_PAIR:           return "KB_PAIR";

        case RC_SYNC:           return "RC_SYNC";
        case RC_CMD:            return "RC_CMD";
        case RC_ENUM_RSP:       return "RC_ENUM_RSP";
        case RC_ENUM_ACK:       return "RC_ENUM_ACK";
        case RC_SLEEP:          return "RC_SLEEP";
        case RC_RESET:          return "RC_RESET";
        case RC_PING:           return "RC_PING";

        case RC_REPORT:         return "RC_REPORT";
        case RC_BATT:           return "RC_BATT";
        case RC_TEMP:           return "RC_TEMP";
        case RC_LOG:            return "RC_LOG";
        case RC_EMERGENCY:      return "RC_EMERGENCY";
        case RC_SET_SCENE:      return "RC_SET_SCENE";
        case RC_SET_MACRO:      return "RC_SET_MACRO";

        default:                return "?";
    }
}

/*==========================================================================
 * 地址拼装
 *========================================================================*/
void Rf_MakeAddr(uint8_t out[RF_ADDR_LEN], uint32_t prefix_le, uint8_t suffix)
{
    out[0] = (uint8_t)(prefix_le & 0xFFu);
    out[1] = (uint8_t)((prefix_le >> 8) & 0xFFu);
    out[2] = (uint8_t)((prefix_le >> 16) & 0xFFu);
    out[3] = (uint8_t)((prefix_le >> 24) & 0xFFu);
    out[4] = suffix;
}

void Rf_MakeRcHostAddr(uint8_t out[RF_ADDR_LEN])
{
    out[0] = (uint8_t)RF_ADDR_RC_HOST0;
    out[1] = (uint8_t)RF_ADDR_RC_HOST1;
    out[2] = (uint8_t)RF_ADDR_RC_HOST2;
    out[3] = (uint8_t)RF_ADDR_RC_HOST3;
    out[4] = (uint8_t)RF_ADDR_RC_HOST4;
}

void Rf_MakeRcDevAddr(uint8_t out[RF_ADDR_LEN], uint8_t dev_id)
{
    if (dev_id == 0u)
    {
        /* 未枚举：监听枚举地址 */
        out[0] = (uint8_t)RF_ADDR_RC_ENUM0;
        out[1] = (uint8_t)RF_ADDR_RC_ENUM1;
        out[2] = (uint8_t)RF_ADDR_RC_ENUM2;
        out[3] = (uint8_t)RF_ADDR_RC_ENUM3;
        out[4] = (uint8_t)RF_ADDR_RC_ENUM4;
        return;
    }

    if (dev_id > (uint8_t)RF_RC_MAX_DEV)
    {
        dev_id = (uint8_t)RF_RC_MAX_DEV;
    }

    out[0] = (uint8_t)RF_ADDR_RC_DEV0;
    out[1] = (uint8_t)RF_ADDR_RC_DEV1;
    out[2] = (uint8_t)RF_ADDR_RC_DEV2;
    out[3] = (uint8_t)RF_ADDR_RC_DEV3;
    out[4] = (uint8_t)RF_ADDR_RC_DEV4(dev_id);
}
