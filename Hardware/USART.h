#ifndef __USART_H
#define __USART_H

#include <stdint.h>

/*
 * 帧接收结果
 */
typedef enum {
    FRAME_OK    = 0,   /* 成功取到一帧，payload 已写入 buffer */
    FRAME_ERROR = 1,   /* 校验失败，帧已丢弃，NAK 已发出 */
    FRAME_NONE  = 2,   /* 暂无新帧 */
} USART_FrameResult;

void               USART1_Init(void);
void               USART_SendByte(uint8_t byte);
void               USART_SendString(const char *str);
USART_FrameResult  USART_ReceiveFrame(char *buffer, uint8_t buf_size, uint8_t *out_len);

#endif
