#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "MyRTC.h"
#include "USART.h"
#include "LED.h"
#include "KEY.h"

/* 
 * 多闹钟配置 - 用户只需修改这里
 * 每条记录：{ 开始小时, 开始分钟, 持续分钟数 }
 * 支持任意数量的闹钟，允许跨天（持续时间超过当天午夜）。
 * 闹钟时间段不应互相重叠，否则行为未定义。
 *  */
typedef struct {
    uint8_t start_hour;        /* 0-23 */
    uint8_t start_minute;      /* 0-59 */
    uint8_t duration_minutes;  /* 1-1439（跨天最多23h59m） */
} AlarmSlot;

static const AlarmSlot ALARMS[] = {
    {  7, 30, 2 },   /* 闹钟1：07:30 持续 2 分钟 */
    { 12,  0, 1 },   /* 闹钟2：12:00 持续 1 分钟 */
    { 22, 28, 2 },   /* 闹钟3：20:20 持续 2 分钟 */
	{ 23, 00, 1 },
	{ 23, 10, 1 },
	{ 23, 20, 1 },
	{ 23, 30, 1 }
};

#define ALARM_COUNT  (sizeof(ALARMS) / sizeof(ALARMS[0]))

/* 进入待机前在 IDLE 状态停留的秒数 */
#define IDLE_STANDBY_DELAY_SEC  8u

typedef enum {
    STATE_BOOT_CHECK,    /* 初始化、外设检测 */
    STATE_BOOT_DOWNLOAD, /* 下载模式：死循环 */
    STATE_BOOT_NTP,      /* NTP同步中（一次性） */
    STATE_IDLE,          /* 非闹钟期：显示时钟，倒计时后进入待机 */
    STATE_STANDBY,       /* 即将进入硬件待机 */
    STATE_ALARM,         /* 闹钟期内：LED亮，保持唤醒 */
} AppState;

/* 
 * 事件定义
 * 每个主循环 tick 开始时由 CollectEvents() 采样一次。
 *  */
typedef enum {
    EVT_NONE            = 0x00,
    EVT_RTC_ALARM       = 0x01,  /* RTC_FLAG_ALR 已置位 */
    EVT_IN_ALARM_PERIOD = 0x02,  /* 当前时刻落在某个闹钟窗口内 */
    EVT_STANDBY_TIMEOUT = 0x04,  /* IDLE 倒计时到零 */
} AppEvent;

/* 
 * 模块内部状态
 *  */
static AppState g_state      = STATE_BOOT_CHECK;
static uint32_t g_idle_ticks = 0;  /* IDLE 状态已经历的 tick 数 */
static uint8_t  g_heartbeat  = 0;  /* 0/1 交替，驱动 '*' 闪烁 */

/* 
 * 闹钟调度辅助函数
 *  */

/* 将 AlarmSlot[idx] 转换为从午夜起的 开始/结束 秒数。
 * end 可能 > 86400（跨天），调用方自行处理取模。 */
static void AlarmSlotToSecs(uint8_t idx,
                             uint32_t *out_start,
                             uint32_t *out_end)
{
    uint32_t start = ((uint32_t)ALARMS[idx].start_hour   * 60u
                    + (uint32_t)ALARMS[idx].start_minute) * 60u;
    *out_start = start;
    *out_end   = start + (uint32_t)ALARMS[idx].duration_minutes * 60u;
}

/* 返回当前落入的闹钟索引（0-based），不在任何闹钟期内则返回 -1。*/
static int8_t FindActiveAlarm(uint32_t current_secs)
{
    uint8_t i;
    for (i = 0; i < ALARM_COUNT; i++)
    {
        uint32_t s, e;
        AlarmSlotToSecs(i, &s, &e);

        if (e > 86400u)
        {
            /* 跨天：[s, 86400) ∪ [0, e-86400) */
            if (current_secs >= s || current_secs < (e - 86400u))
                return (int8_t)i;
        }
        else
        {
            if (current_secs >= s && current_secs < e)
                return (int8_t)i;
        }
    }
    return -1;
}

/* 计算距当前最近的未来闹钟边界（start 或 end），
 * 返回对应的 RTC 计数器绝对值，用于写入 RTC_ALR。*/
static uint32_t FindNextAlarmBoundary(uint32_t current_counter,
                                      uint32_t current_secs)
{
    uint32_t min_delta = 0xFFFFFFFFu;
    uint8_t  i;

    for (i = 0; i < ALARM_COUNT; i++)
    {
        uint32_t s, e;
        AlarmSlotToSecs(i, &s, &e);

        /* start 边界 */
        {
            uint32_t s_mod = s % 86400u;
            uint32_t delta = (s_mod > current_secs)
                           ? (s_mod - current_secs)
                           : (86400u - current_secs + s_mod);
            if (delta < min_delta) min_delta = delta;
        }

        /* end 边界 */
        {
            uint32_t delta;
            if (e > 86400u)
            {
                delta = (e > current_secs)
                      ? (e - current_secs)
                      : (86400u - current_secs + (e % 86400u));
            }
            else
            {
                uint32_t e_mod = e % 86400u;
                delta = (e_mod > current_secs)
                      ? (e_mod - current_secs)
                      : (86400u - current_secs + e_mod);
                if (delta == 0u) delta = 86400u;  /* 恰好相等，等到明天 */
            }
            if (delta < min_delta) min_delta = delta;
        }
    }

    if (min_delta == 0u) min_delta = 1u;  /* 保险：至少等1秒 */
    return current_counter + min_delta;
}

/* 将下一个边界写入 RTC_ALR。*/
static void ScheduleNextBoundary(void)
{
    uint32_t ctr  = RTC_GetCounter();
    uint32_t secs = MyRTC_CounterToSecsMidnight(ctr);
    RTC_SetAlarm(FindNextAlarmBoundary(ctr, secs));
    RTC_WaitForLastTask();
}

/* 
 * 事件采集
 * 在每个 tick 的最开始调用，返回本 tick 的事件位掩码。
 * RTC_FLAG_ALR 在此处清除，确保每次唤醒只处理一次。
 *  */
static AppEvent CollectEvents(void)
{
    AppEvent evt = EVT_NONE;

    /* 1. RTC 闹钟标志 */
    if (RTC_GetFlagStatus(RTC_FLAG_ALR) == SET)
    {
        RTC_ClearFlag(RTC_FLAG_ALR);
        RTC_WaitForLastTask();
        evt = (AppEvent)(evt | EVT_RTC_ALARM);
    }

    /* 2. 当前是否在某个闹钟期内 */
    uint32_t secs = MyRTC_CounterToSecsMidnight(RTC_GetCounter());
    if (FindActiveAlarm(secs) >= 0)
        evt = (AppEvent)(evt | EVT_IN_ALARM_PERIOD);

    /* 3. IDLE 超时 */
    if (g_idle_ticks >= IDLE_STANDBY_DELAY_SEC)
        evt = (AppEvent)(evt | EVT_STANDBY_TIMEOUT);

    return evt;
}

/* 
 * 状态入口动作（每次进入新状态时执行一次）
 *  */
static void OnEnter_Idle(void)
{
    g_idle_ticks = 0;
    LED_set(GPIOA, GPIO_Pin_1, 0);
    OLED_ShowString(4, 1, "MODE:WAIT ALR   ");
    ScheduleNextBoundary();
}

static void OnEnter_Alarm(void)
{
    LED_set(GPIOA, GPIO_Pin_1, 1);
    uint32_t secs = MyRTC_CounterToSecsMidnight(RTC_GetCounter());
    int8_t   idx  = FindActiveAlarm(secs);
    OLED_ShowString(4, 1, "MODE:ALM ");
    OLED_ShowNum   (4, 10, (uint32_t)(idx + 1), 1);
    OLED_ShowString(4, 11, " ON  ");
    ScheduleNextBoundary();
}

static void OnEnter_Standby(void)
{
    LED_set(GPIOA, GPIO_Pin_1, 0);
    OLED_ShowString(4, 1, "MODE:STANDBY    ");
    Delay_ms(500);
    OLED_Clear();
    PWR_EnterSTANDBYMode();
    /* 不会执行到这里：唤醒后从 main() 顶部重新开始 */
}

/* 
 * 状态转移
 * 根据当前状态和事件决定下一个状态；状态变化时自动执行入口动作。
 *  */
static void FSM_Transition(AppEvent evt)
{
    AppState next = g_state;

    switch (g_state)
    {
        case STATE_BOOT_CHECK:
            if (GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_11) == Bit_SET)
                next = STATE_BOOT_DOWNLOAD;
            else if (GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_1) == Bit_SET)
                next = STATE_BOOT_NTP;
            else
                next = STATE_IDLE;
            break;

        case STATE_BOOT_DOWNLOAD:
            break;  /* 死循环，永不转移 */

        case STATE_BOOT_NTP:
            /* NTP 同步是阻塞操作，完成后立刻决定去向 */
            next = (evt & EVT_IN_ALARM_PERIOD) ? STATE_ALARM : STATE_IDLE;
            break;

        case STATE_IDLE:
            if (evt & EVT_IN_ALARM_PERIOD)
                next = STATE_ALARM;
            else if (evt & EVT_STANDBY_TIMEOUT)
                next = STATE_STANDBY;
            break;

        case STATE_STANDBY:
            break;  /* OnEnter_Standby() 调用 PWR_EnterSTANDBYMode()，不会返回 */

        case STATE_ALARM:
            if (!(evt & EVT_IN_ALARM_PERIOD))
                next = STATE_IDLE;
            break;
    }

    /* 状态发生变化：执行入口动作 */
    if (next != g_state)
    {
        g_state = next;
        switch (g_state)
        {
            case STATE_IDLE:    OnEnter_Idle();    break;
            case STATE_ALARM:   OnEnter_Alarm();   break;
            case STATE_STANDBY: OnEnter_Standby(); break;
            default: break;
        }
    }
}

/* 
 * 状态内持续动作（每 tick 执行）
 *  */
static void FSM_Tick(void)
{
    switch (g_state)
    {
        case STATE_IDLE:
        case STATE_ALARM:
            OLED_ShowNum(1, 5, RTC_GetCounter(), 10);
            OLED_ShowNum(3, 6, RTC_GetFlagStatus(RTC_FLAG_ALR), 1);
            OLED_ShowString(3, 8, g_heartbeat ? "*" : " ");
            g_heartbeat ^= 1u;

            if (g_state == STATE_IDLE)
                g_idle_ticks++;
            else
                g_idle_ticks = 0;
            break;

        default:
            break;
    }
}

/* 
 * 启动阶段处理（阻塞式，只在 main() 开头执行一次）
 *  */
static void Boot_RunDownloadMode(void)
{
    OLED_ShowString(1, 1, "DOWNLOAD MODE   ");
    OLED_ShowString(2, 1, "PB11=HIGH       ");
    OLED_ShowString(3, 1, "Waiting...      ");
    OLED_ShowString(4, 1, "Pull PB11 LOW   ");
    while (1);
}

static void Boot_RunNTP(void)
{
    OLED_ShowString(1, 1, "NTP Syncing...  ");
    OLED_ShowString(1, 1, NTP_SyncTime() ? "NTP OK!         "
                                         : "NTP FAIL-RTC OK ");
    Delay_ms(1500);
    OLED_Clear();
}

static void Boot_SkipNTP(void)
{
    OLED_ShowString(1, 1, "NTP SKIP-RTC OK ");
    Delay_ms(1500);
    OLED_Clear();
}

/* 
 * main
 *  */
int main(void)
{
    /* ---- 外设初始化 ---- */
    OLED_Init();
    LED_Init(GPIO_Pin_1);
    MyRTC_Init();
    USART1_Init();
    Delay_ms(10);
    Key_Init();

    /* ---- 使能 WKUP 引脚（PA0 上升沿唤醒待机） ---- */
    PWR_WakeUpPinCmd(ENABLE);

    /* ---- 启动阶段：BOOT_CHECK → DOWNLOAD / NTP / IDLE ---- */
    g_state = STATE_BOOT_CHECK;
    FSM_Transition(EVT_NONE);

    if (g_state == STATE_BOOT_DOWNLOAD)
    {
        Boot_RunDownloadMode();   /* 不返回 */
    }
    else if (g_state == STATE_BOOT_NTP)
    {
        Boot_RunNTP();
        FSM_Transition(EVT_NONE); /* NTP → ALARM / IDLE，含入口动作 */
    }
    else
    {
        /* STATE_IDLE：直接跳过 NTP */
        Boot_SkipNTP();
        OnEnter_Idle();
    }

    /* ---- 静态 OLED 标签 ---- */
    OLED_ShowString(1, 1, "CNT:");
    OLED_ShowString(2, 1, "ALM:");
    OLED_ShowNum   (2, 5, ALARM_COUNT, 2);
    OLED_ShowString(3, 1, "ALRF:");
    OLED_ShowString(4, 1, "MODE:");

    /* ---- 主循环 ---- */
    while (1)
    {
        AppEvent evt = CollectEvents();   /* 1. 采集事件 */
        FSM_Transition(evt);              /* 2. 状态转移（含入口动作） */
        FSM_Tick();                       /* 3. 状态内持续动作 */
        Delay_ms(1000);                   /* 4. 固定 tick 周期：1 秒 */
    }
}
