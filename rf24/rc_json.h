/**************************************************************************
 * rc_json.h —— 把主机侧的设备表导成 JSON
 *
 * 依据：规范 16.7 节、附录 F.2 的示例格式
 *
 * 输出示例：
 *   {"devices":[
 *     {"dev_id":1,"name":"tv_box","hw_ver":1,"sw_ver":16,
 *      "rssi":-38,"battery_mv":3950,"online":1,"config_ver":3,
 *      "keys":[
 *        {"id":1,"name":"Power","action":"TOGGLE","scene":0,"state":0},
 *        ...
 *      ]}
 *   ]}
 *
 * 用法（串口/屏上显示都行）：
 *     char buf[1024];
 *     uint16_t n = RcJson_Export(&h, buf, sizeof(buf));
 **************************************************************************/

#ifndef __RC_JSON_H__
#define __RC_JSON_H__

#include <stdint.h>
#include "proto_rc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * @fn      RcJson_Export
 * @brief   把主机设备表导成 JSON
 * @param   h    主机上下文
 * @param   buf  输出缓冲（建议 >= 1024 字节）
 * @param   cap  缓冲大小
 * @return  写进去的字符数（不含结尾 '\0'）；缓冲不够会尽量截断并保证 '\0'
 *********************************************************************/
uint16_t RcJson_Export(const rc_host_t *h, char *buf, uint16_t cap);

/*********************************************************************
 * @fn      RcJson_ActionName
 * @brief   行为枚举 -> 字符串（"TOGGLE" / "SCENE" ...）
 *********************************************************************/
const char *RcJson_ActionName(uint8_t action);

/* JSON 转义：把 " 和 \ 转成 \" \\ ；控制字符转成空格 */
uint16_t RcJson_Escape(const char *in, uint8_t len, char *out, uint16_t cap);

#ifdef __cplusplus
}
#endif

#endif /* __RC_JSON_H__ */
