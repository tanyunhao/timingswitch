#include "stm32f10x.h"                  // Device header
#include "MyRTC.h"
#include "USART.h"
#include "Delay.h"
#include <time.h>
#include <string.h>

/*
 * 设计说明：时区处理策略
 * ─────────────────────────────────────────────────────────────
 * RTC硬件计数器中存储的是 UTC 时间对应的 Unix 时间戳（秒数）。
 *
 * MyRTC_SetTime()  : MyRTC_Time[] 存放 UTC+8 本地时间
 *                    → 写入RTC前减去 8h，转为 UTC 存储
 * MyRTC_ReadTime() : 从RTC读出 UTC 时间戳
 *                    → 加上 8h 转为 UTC+8，存入 MyRTC_Time[]
 * NTP_SyncTime()   : PC 通过 USB-TTL 主动推送 UTC+8 本地时间
 *                    → 帧格式：@TIME,YYYY-MM-DD HH:MM:SS#<xor校验字节>
 *                    → 校验通过后填入 MyRTC_Time[]，调用 MyRTC_SetTime()
 *                    → 成功回复 "ACK\n"，失败由 USART 层或本函数回复 "NAK\n"
 *
 * 之前出现 ~17,740,000,000 溢出的根本原因：
 *   mktime() 收到了未减1900的年份（tm_year=2026而非126），
 *   计算出遥远未来的时间戳，转为uint32_t后溢出。
 *   本版本所有 struct tm 赋值均严格执行 -1900 / -1 偏移。
 * ─────────────────────────────────────────────────────────────
 */

#define TIMEZONE_OFFSET_SEC  (8 * 60 * 60)   /* UTC+8，单位：秒 */

uint16_t MyRTC_Time[] = {2023, 1, 1, 23, 59, 55};  /* 年,月,日,时,分,秒（本地UTC+8） */

void MyRTC_SetTime(void);            /* 函数声明 */
static uint32_t CounterToSecsMidnight(uint32_t counter); /* 内部辅助函数声明 */

/**
  * 函    数：RTC初始化
  */
void MyRTC_Init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_BKP, ENABLE);

    PWR_BackupAccessCmd(ENABLE);

    if (BKP_ReadBackupRegister(BKP_DR1) != 0xA5A5)
    {
        RCC_LSEConfig(RCC_LSE_ON);
        while (RCC_GetFlagStatus(RCC_FLAG_LSERDY) != SET);

        RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
        RCC_RTCCLKCmd(ENABLE);

        RTC_WaitForSynchro();
        RTC_WaitForLastTask();

        RTC_SetPrescaler(32768 - 1);
        RTC_WaitForLastTask();

        MyRTC_SetTime();

        BKP_WriteBackupRegister(BKP_DR1, 0xA5A5);
    }
    else
    {
        RTC_WaitForSynchro();
        RTC_WaitForLastTask();
    }
}

/**
  * 函    数：将RTC计数器值转换为本地时间（UTC+8）的午夜秒数
  * 参    数：counter - RTC计数器原始值（UTC Unix时间戳）
  * 返 回 值：从当天午夜起经过的秒数（0 ~ 86399）
  * 说    明：供 main.c 中的闹钟计算使用，
  *           使 main.c 无需直接依赖 <time.h>。
  */
static uint32_t CounterToSecsMidnight(uint32_t counter)
{
    time_t local = (time_t)counter + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm *t = localtime(&local);
    return (uint32_t)t->tm_hour * 3600u +
           (uint32_t)t->tm_min  * 60u   +
           (uint32_t)t->tm_sec;
}

uint32_t MyRTC_CounterToSecsMidnight(uint32_t counter)
{
    return CounterToSecsMidnight(counter);
}

/**
  * 函    数：RTC设置时间
  * 说    明：MyRTC_Time[] 中存放 UTC+8 本地时间，
  *           本函数减去8h后以UTC存入RTC计数器。
  */
void MyRTC_SetTime(void)
{
    struct tm t;
    t.tm_year  = (int)(MyRTC_Time[0]) - 1900;
    t.tm_mon   = (int)(MyRTC_Time[1]) - 1;
    t.tm_mday  = (int)(MyRTC_Time[2]);
    t.tm_hour  = (int)(MyRTC_Time[3]);
    t.tm_min   = (int)(MyRTC_Time[4]);
    t.tm_sec   = (int)(MyRTC_Time[5]);
    t.tm_isdst = 0;

    time_t utc = mktime(&t) - (time_t)TIMEZONE_OFFSET_SEC;

    RTC_SetCounter((uint32_t)utc);
    RTC_WaitForLastTask();
}

/**
  * 函    数：RTC读取时间
  * 说    明：从RTC读出UTC时间戳，加8h后转为本地时间存入MyRTC_Time[]。
  */
void MyRTC_ReadTime(void)
{
    time_t utc = (time_t)RTC_GetCounter() + (time_t)TIMEZONE_OFFSET_SEC;

    struct tm *t = localtime(&utc);

    MyRTC_Time[0] = (uint16_t)(t->tm_year + 1900);
    MyRTC_Time[1] = (uint16_t)(t->tm_mon  + 1);
    MyRTC_Time[2] = (uint16_t)(t->tm_mday);
    MyRTC_Time[3] = (uint16_t)(t->tm_hour);
    MyRTC_Time[4] = (uint16_t)(t->tm_min);
    MyRTC_Time[5] = (uint16_t)(t->tm_sec);
}

/**
  * 函    数：通过PC（USB-TTL）获取时间并同步到RTC
  * 返 回 值：1=同步成功，0=超时或解析失败
  * 说    明：被动接收模式。PC端脚本在串口连接后立即主动推送时间帧。
  *           帧格式：@TIME,YYYY-MM-DD HH:MM:SS#<xor校验字节>
  *           XOR校验失败时底层自动回复 "NAK\n" 并继续等待下一帧。
  *           内容非法时本函数回复 "NAK\n"。
  *           成功时回复 "ACK\n"。超时30秒后回退到RTC现有时间。
  */
uint8_t NTP_SyncTime(void)
{
    char     buffer[64];
    uint32_t waited = 0;

    /* 等待 USART 层组装出一帧（最多30秒） */
    while (1)
    {
        USART_FrameResult result = USART_ReceiveFrame(buffer, sizeof(buffer), NULL);

        if (result == FRAME_OK)    break;
        /* FRAME_ERROR: XOR校验失败，NAK已由底层发出，继续等待下一帧重传 */

        Delay_ms(10);
        waited += 10;
        if (waited >= 30000) return 0;
    }

    /* ── 校验帧头 ── */
    /* 期望 payload: "TIME,YYYY-MM-DD HH:MM:SS" */
    if (strncmp(buffer, "TIME,", 5) != 0)
    {
        USART_SendString("NAK\n");
        return 0;
    }

    char *p = buffer + 5;
    if (strlen(p) < 19)
    {
        USART_SendString("NAK\n");
        return 0;
    }

    /* ── 解析 ──
     * p: "YYYY-MM-DD HH:MM:SS"
     *     0123456789012345678   */
    uint16_t year   = (uint16_t)((p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0'));
    uint16_t month  = (uint16_t)((p[5]-'0')*10   + (p[6]-'0'));
    uint16_t day    = (uint16_t)((p[8]-'0')*10   + (p[9]-'0'));
    uint16_t hour   = (uint16_t)((p[11]-'0')*10  + (p[12]-'0'));
    uint16_t minute = (uint16_t)((p[14]-'0')*10  + (p[15]-'0'));
    uint16_t second = (uint16_t)((p[17]-'0')*10  + (p[18]-'0'));

    /* ── 范围校验 ── */
    if (year < 2000 || year > 2099 ||
        month < 1   || month > 12  ||
        day   < 1   || day   > 31  ||
        hour  > 23  || minute > 59 || second > 59)
    {
        USART_SendString("NAK\n");
        return 0;
    }

    /* ── 写入 MyRTC_Time[]（UTC+8本地时间）── */
    MyRTC_Time[0] = year;
    MyRTC_Time[1] = month;
    MyRTC_Time[2] = day;
    MyRTC_Time[3] = hour;
    MyRTC_Time[4] = minute;
    MyRTC_Time[5] = second;

    /* ── 写入RTC硬件 ── */
    MyRTC_SetTime();

    /* ── 通知PC已成功接受 ── */
    USART_SendString("ACK\n");

    return 1;
}
