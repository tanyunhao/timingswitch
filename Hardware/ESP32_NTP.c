/* ESP32_NTP.c - STM32 USART1 driver to request NTP time from ESP32-C3 */
#include "stm32f10x.h"
#include "ESP32_NTP.h"
#include "MyRTC.h"
#include <string.h>
#include <stdlib.h>

#define NTP_UART_TIMEOUT_MS  5000u   // 等待ESP32回复的超时时间（ms）

/* ---- 内部：USART1初始化（PA9=TX, PA10=RX, 115200 8N1）---- */
static void USART1_Init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1 | RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitTypeDef GPIO_InitStructure;
    /* PA9 TX: 复用推挽输出 */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    /* PA10 RX: 浮空输入 */
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitTypeDef USART_InitStructure;
    USART_InitStructure.USART_BaudRate            = 115200;
    USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits            = USART_StopBits_1;
    USART_InitStructure.USART_Parity              = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &USART_InitStructure);

    USART_Cmd(USART1, ENABLE);
}

/* ---- 内部：发送单个字节 ---- */
static void USART1_SendByte(uint8_t byte)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    USART_SendData(USART1, byte);
}

/* ---- 内部：带超时地接收一行（'\n'结尾），返回实际收到的字符数 ---- */
static uint16_t USART1_ReceiveLine(char *buf, uint16_t maxLen, uint32_t timeoutMs)
{
    uint16_t idx = 0;
    uint32_t ticks = 0;

    while (idx < maxLen - 1)
    {
        if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) == SET)
        {
            char c = (char)USART_ReceiveData(USART1);
            buf[idx++] = c;
            ticks = 0;          // 收到数据就重置超时计数
            if (c == '\n') break;
        }
        else
        {
            /* 粗略的1ms计时：每次空循环约1ms（72MHz下近似） */
            volatile uint32_t i = 7200;
            while (i--);
            if (++ticks >= timeoutMs) break;
        }
    }
    buf[idx] = '\0';
    return idx;
}

/* ---- 内部：解析 "TIME,YYYY-MM-DD HH:MM:SS\n" ---- */
static uint8_t ParseTimeString(const char *str, uint16_t *timeArr)
{
    /* 期望格式：TIME,2025-03-21 14:30:05  */
    if (strncmp(str, "TIME,", 5) != 0) return 0;

    const char *p = str + 5;
    /* YYYY */
    timeArr[0] = (uint16_t)atoi(p);           p += 5;  /* skip "YYYY-" */
    /* MM   */
    timeArr[1] = (uint16_t)atoi(p);           p += 3;  /* skip "MM-"   */
    /* DD   */
    timeArr[2] = (uint16_t)atoi(p);           p += 3;  /* skip "DD "   */
    /* HH   */
    timeArr[3] = (uint16_t)atoi(p);           p += 3;  /* skip "HH:"   */
    /* MM   */
    timeArr[4] = (uint16_t)atoi(p);           p += 3;  /* skip "MM:"   */
    /* SS   */
    timeArr[5] = (uint16_t)atoi(p);

    /* 简单合法性检查 */
    if (timeArr[0] < 2020 || timeArr[0] > 2099) return 0;
    if (timeArr[1] < 1    || timeArr[1] > 12)   return 0;
    if (timeArr[2] < 1    || timeArr[2] > 31)   return 0;
    if (timeArr[3] > 23)                         return 0;
    if (timeArr[4] > 59)                         return 0;
    if (timeArr[5] > 59)                         return 0;

    return 1;
}

/* ==============================================================
 * 公开接口：初始化 + 从ESP32同步NTP时间到STM32 RTC
 * 返回值：NTP_OK / NTP_ERR_TIMEOUT / NTP_ERR_PARSE / NTP_ERR_ESP
 * ============================================================== */
void ESP32_NTP_Init(void)
{
    USART1_Init();
}

ESP32_NTP_Result ESP32_NTP_Sync(void)
{
    char rxBuf[48];

    /* 1. 清空残留接收数据 */
    while (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) == SET)
        (void)USART_ReceiveData(USART1);

    /* 2. 请求时间 */
    USART1_SendByte('T');

    /* 3. 等待回复 */
    uint16_t len = USART1_ReceiveLine(rxBuf, sizeof(rxBuf), NTP_UART_TIMEOUT_MS);

    if (len == 0) return NTP_ERR_TIMEOUT;

    /* 4. 检查是否是错误报文 */
    if (strncmp(rxBuf, "ERR,", 4) == 0) return NTP_ERR_ESP;

    /* 5. 解析时间 */
    uint16_t newTime[6];
    if (!ParseTimeString(rxBuf, newTime)) return NTP_ERR_PARSE;

    /* 6. 写入RTC */
    for (int i = 0; i < 6; i++)
        MyRTC_Time[i] = newTime[i];

    MyRTC_SetTime();   // 把 MyRTC_Time 数组刷新到RTC硬件

    return NTP_OK;
}
