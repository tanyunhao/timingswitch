#ifndef __USART_H
#define __USART_H

#include <stdint.h>

extern volatile char rx_buffer[64];
extern volatile uint8_t rx_index;
extern volatile uint8_t rx_complete;

void USART1_Init(void);
void USART_SendByte(uint8_t byte);
uint8_t USART_ReceiveString(char *buffer, uint32_t size);

#endif
