#include "stm32f10x.h"
#include "USART.h"
#include <string.h>

/* 接收缓冲 */
volatile char rx_buffer[64];
volatile uint8_t rx_index = 0;
volatile uint8_t rx_complete = 0;

/* 帧状态 */
static uint8_t frame_started = 0;

void USART1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(
        RCC_APB2Periph_GPIOB |
        RCC_APB2Periph_AFIO |
        RCC_APB2Periph_USART1,
        ENABLE
    );

    GPIO_PinRemapConfig(GPIO_Remap_USART1, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStructure);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART1, ENABLE);
}

/**
  * 函    数：USART1发送一个字节
  * 参    数：byte 要发送的字节
  * 返 回 值：无
  */
void USART_SendByte(uint8_t byte)
{
    /* 等待发送寄存器为空 */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    USART_SendData(USART1, (uint16_t)byte);
    /* 等待发送完全结束，确保字节已从总线上完整发出 */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
}

uint8_t USART_ReceiveString(char *buffer, uint32_t size)
{
    if (rx_complete) {
        uint32_t len = (rx_index < size - 1) ? rx_index : size - 1;
        memcpy(buffer, (void *)rx_buffer, len);
        buffer[len] = '\0';

        rx_index = 0;
        rx_complete = 0;

        return 1;
    }
    return 0;
}

/* ================= USART1 中断 ================= */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        char ch = USART_ReceiveData(USART1);

        if (ch == '@') {
            rx_index = 0;
            frame_started = 1;
        }
        else if (ch == '#' && frame_started) {
            rx_buffer[rx_index] = '\0';
            rx_complete = 1;
            frame_started = 0;
        }
        else if (frame_started) {
            if (rx_index < sizeof(rx_buffer) - 1) {
                rx_buffer[rx_index++] = ch;
            }
        }
        /* 其他所有数据：直接丢弃 */

        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}
