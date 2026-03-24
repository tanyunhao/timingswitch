#include "stm32f10x.h"
#include "USART.h"
#include <string.h>

/*
 * 通信协议说明
 * ─────────────────────────────────────────────────────────────
 * 帧格式（PC → STM32）：
 *   @<payload>#<XOR校验字节>
 *   示例：@TIME,2026-03-23 14:30:05#<xor>
 *   校验字节 = payload 中所有字节的逐位异或值（不含 @ # 定界符）
 *
 * 应答帧（STM32 → PC）：
 *   "ACK\n"  —— 帧接收且校验通过，已处理
 *   "NAK\n"  —— 帧接收但校验失败或内容非法，已丢弃
 *
 * 接收状态机（在ISR中运行）：
 *   IDLE        → 收到 '@' → RECEIVING
 *   RECEIVING   → 收到 '#' → CHECKSUM（保存payload，等待校验字节）
 *   RECEIVING   → 缓冲区满 → IDLE（溢出保护，重置状态）
 *   CHECKSUM    → 收到校验字节 → 验证，置 rx_complete / rx_error，→ IDLE
 * ─────────────────────────────────────────────────────────────
 */

/* ---- 接收缓冲（仅在本文件内部使用，不对外暴露） ---- */
#define RX_BUF_SIZE  64u

static volatile char    rx_buffer[RX_BUF_SIZE];
static volatile uint8_t rx_len      = 0;   /* payload 有效字节数 */
static volatile uint8_t rx_complete = 0;   /* 1 = 一帧已就绪，等待上层取走 */
static volatile uint8_t rx_error    = 0;   /* 1 = 校验失败 */

/* 接收状态机状态 */
typedef enum { STATE_IDLE, STATE_RECEIVING, STATE_CHECKSUM } RxState;
static volatile RxState rx_state = STATE_IDLE;
static volatile uint8_t rx_xor   = 0;      /* 实时累积的 XOR 值 */

/* ================================================================
 * USART1初始化（PB6=TX, PB7=RX, 重映射, 115200 8N1, 接收中断）
 * ================================================================ */
void USART1_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef  NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(
        RCC_APB2Periph_GPIOB |
        RCC_APB2Periph_AFIO  |
        RCC_APB2Periph_USART1,
        ENABLE
    );

    GPIO_PinRemapConfig(GPIO_Remap_USART1, ENABLE);

    /* PB6: TX 复用推挽 */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* PB7: RX 浮空输入 */
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate            = 115200;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(USART1, &USART_InitStructure);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel                   = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(USART1, ENABLE);
}

/* ================================================================
 * 发送单个字节（等待 TXE，不等 TC——全双工无需确认总线空闲）
 * ================================================================ */
void USART_SendByte(uint8_t byte)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    USART_SendData(USART1, (uint16_t)byte);
}

/* ================================================================
 * 发送以 '\0' 结尾的字符串
 * ================================================================ */
void USART_SendString(const char *str)
{
    while (*str)
    {
        USART_SendByte((uint8_t)*str++);
    }
}

/* ================================================================
 * 尝试取走一帧已接收的 payload
 * 返回值：
 *   FRAME_OK    — 成功，buffer 已填充，长度写入 *out_len（可为NULL）
 *   FRAME_ERROR — 校验失败（已自动发送 NAK）
 *   FRAME_NONE  — 没有新帧
 * ================================================================ */
USART_FrameResult USART_ReceiveFrame(char *buffer, uint8_t buf_size, uint8_t *out_len)
{
    if (!rx_complete && !rx_error)
    {
        return FRAME_NONE;
    }

    if (rx_error)
    {
        rx_error    = 0;
        rx_complete = 0;
        USART_SendString("NAK\n");
        return FRAME_ERROR;
    }

    /* rx_complete == 1, rx_error == 0 */
    uint8_t len = (rx_len < (uint8_t)(buf_size - 1)) ? rx_len : (uint8_t)(buf_size - 1);
    memcpy(buffer, (const void *)rx_buffer, len);
    buffer[len] = '\0';

    if (out_len) *out_len = len;

    rx_complete = 0;
    rx_len      = 0;

    return FRAME_OK;
}

/* ================================================================
 * USART1 接收中断服务程序
 * 实现带 XOR 校验的状态机帧解析
 * ================================================================ */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) == RESET) return;

    uint8_t ch = (uint8_t)USART_ReceiveData(USART1);

    switch (rx_state)
    {
        case STATE_IDLE:
            if (ch == '@')
            {
                rx_len   = 0;
                rx_xor   = 0;
                rx_state = STATE_RECEIVING;
            }
            /* 其他字节：静默丢弃 */
            break;

        case STATE_RECEIVING:
            if (ch == '#')
            {
                /* payload 结束，等待紧跟的校验字节 */
                rx_state = STATE_CHECKSUM;
            }
            else if (rx_len >= RX_BUF_SIZE - 1u)
            {
                /* 缓冲区溢出保护：丢弃本帧，重置到 IDLE */
                rx_state = STATE_IDLE;
            }
            else
            {
                rx_buffer[rx_len++] = (char)ch;
                rx_xor ^= ch;   /* 累积 XOR */
            }
            break;

        case STATE_CHECKSUM:
            if (ch == rx_xor)
            {
                /* 校验通过 */
                rx_buffer[rx_len] = '\0';
                rx_complete = 1;
            }
            else
            {
                /* 校验失败 */
                rx_error = 1;
            }
            rx_state = STATE_IDLE;
            break;
    }

    USART_ClearITPendingBit(USART1, USART_IT_RXNE);
}
