/* ESP32_NTP.h */
#ifndef __ESP32_NTP_H
#define __ESP32_NTP_H

typedef enum {
    NTP_OK            = 0,
    NTP_ERR_TIMEOUT   = 1,   // ESP32没有在超时内回复
    NTP_ERR_PARSE     = 2,   // 回复格式解析失败
    NTP_ERR_ESP       = 3,   // ESP32返回了ERR,...报文
} ESP32_NTP_Result;

void             ESP32_NTP_Init(void);
ESP32_NTP_Result ESP32_NTP_Sync(void);

#endif
