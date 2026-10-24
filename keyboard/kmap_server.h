/**************************************************************************
 * kmap_server.h —— 把「映射配置协议」接到实际操作上（设备端命令服务）
 *
 * 分层：
 *   usb_cdc.c      只管 USB 端点收发字节          ← 传输层
 *   kmap_proto.c   只管字节流 <-> 帧              ← 协议层
 *   kmap_server.c  只管帧 -> 具体动作（本文件）    ← 业务层
 *
 * 这样协议能单测，业务逻辑也能单测，互不干扰。
 **************************************************************************/

#ifndef __KMAP_SERVER_H__
#define __KMAP_SERVER_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * 设备信息（GET_INFO 的回包内容，上位机用来确认"对面是我的键盘"）
 *========================================================================*/
#define KMAP_INFO_MAGIC     0x4B42u     /* 'K''B' */

typedef struct
{
    uint16_t magic;         /* KMAP_INFO_MAGIC                      */
    uint8_t  ver_major;     /* 协议主版本，目前 1                   */
    uint8_t  ver_minor;     /* 协议次版本，目前 0                   */
    uint8_t  groups;        /* 映射组数（= KEY_MAP_GROUPS）         */
    uint8_t  keys;          /* 每组键数（= KEY_MAP_KEYS）           */
    uint8_t  cur_group;     /* 当前生效的组号                       */
    uint8_t  has_record;    /* Flash 里有没有存过映射（0/1）        */
    uint16_t fw_build;      /* 固件编译标记，随便填，用来区分版本   */
} kmap_info_t;

/*==========================================================================
 * 服务
 *========================================================================*/

/*********************************************************************
 * @fn      KmapServer_Init
 * @brief   初始化：清零统计；映射本身由 KmapStore_Init 负责
 *********************************************************************/
void KmapServer_Init(void);

/*********************************************************************
 * @fn      KmapServer_HandleFrame
 * @brief   处理一帧请求，组出应答帧
 * @param   cmd      命令码
 * @param   seq      序号（原样回填）
 * @param   payload  请求负载
 * @param   len      负载长度
 * @param   out      应答帧输出缓冲（至少 KMAP_MAX_FRAME 字节）
 * @return  应答帧字节数；0 = 不该回（理论上不会）
 * @note    所有分支都会给出应答（成功或错误），保证上位机不会等超时
 *********************************************************************/
uint16_t KmapServer_HandleFrame(uint8_t cmd, uint8_t seq,
                                const uint8_t *payload, uint16_t len,
                                uint8_t *out);

/* 统计（调试用） */
uint32_t KmapServer_RxFrames(void);
uint32_t KmapServer_ErrorFrames(void);

#ifdef __cplusplus
}
#endif

#endif /* __KMAP_SERVER_H__ */
