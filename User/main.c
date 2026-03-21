#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "MyRTC.h"
#include "USART.h"
#include "LED.h"

/****************** 闹钟配置 - 用户可以在这里修改 ******************/
/* 闹钟开始时间（24小时制） */
#define ALARM_START_HOUR    16      // 闹钟开始小时 (0-23)
#define ALARM_START_MINUTE  15      // 闹钟开始分钟 (0-59)

/* 闹钟维持时间（分钟） */
#define ALARM_DURATION_MINUTES  2  // 闹钟持续分钟数

/* 计算闹钟结束时间（自动处理分钟溢出和小时溢出） */
#define ALARM_TOTAL_MINUTES (ALARM_START_MINUTE + ALARM_DURATION_MINUTES)
#define ALARM_END_MINUTE    (ALARM_TOTAL_MINUTES % 60)
#define ALARM_EXTRA_HOURS   (ALARM_TOTAL_MINUTES / 60)
#define ALARM_END_HOUR      ((ALARM_START_HOUR + ALARM_EXTRA_HOURS) % 24)

/* 预计算：闹钟开始/结束的从午夜起的分钟数 */
#define ALARM_START_MINS_MIDNIGHT  (ALARM_START_HOUR * 60 + ALARM_START_MINUTE)
#define ALARM_END_MINS_MIDNIGHT    (ALARM_END_HOUR   * 60 + ALARM_END_MINUTE)

/****************** 函数声明 ******************/
uint8_t  IsInAlarmMaintainPeriod(void);
uint32_t GetNextAlarmStartTime(void);
void     UpdateAlarmDisplay(uint8_t in_alarm_period);

/**
  * 函    数：检查当前是否在闹钟维持期内
  * 参    数：无
  * 返 回 值：1表示在闹钟维持期内，0表示不在
  */
uint8_t IsInAlarmMaintainPeriod(void)
{
    MyRTC_ReadTime();  // 读取当前时间到MyRTC_Time数组（UTC+8）

    uint16_t current_minutes = (uint16_t)MyRTC_Time[3] * 60u + MyRTC_Time[4];

    // 如果结束时间 <= 开始时间，说明时间段跨天（或整整一天）
    if (ALARM_END_MINS_MIDNIGHT <= ALARM_START_MINS_MIDNIGHT)
    {
        // 跨天情况：在维持期内 ⟺ 当前 >= 开始 OR 当前 < 结束
        if (current_minutes >= ALARM_START_MINS_MIDNIGHT ||
            current_minutes <  ALARM_END_MINS_MIDNIGHT)
        {
            return 1;
        }
    }
    else
    {
        // 同一天情况：在维持期内 ⟺ 开始 <= 当前 < 结束
        if (current_minutes >= ALARM_START_MINS_MIDNIGHT &&
            current_minutes <  ALARM_END_MINS_MIDNIGHT)
        {
            return 1;
        }
    }

    return 0;
}

/**
  * 函    数：计算下一个闹钟开始时间（RTC秒计数器绝对值）
  * 参    数：无
  * 返 回 值：下一个闹钟开始时刻对应的RTC计数器值
  */
uint32_t GetNextAlarmStartTime(void)
{
    MyRTC_ReadTime();  // 读取当前时间到MyRTC_Time数组（UTC+8）

    uint32_t current_secs_since_midnight =
        (uint32_t)MyRTC_Time[3] * 3600u +
        (uint32_t)MyRTC_Time[4] * 60u   +
        MyRTC_Time[5];

    uint32_t alarm_secs_since_midnight =
        (uint32_t)ALARM_START_HOUR   * 3600u +
        (uint32_t)ALARM_START_MINUTE * 60u;

    uint32_t seconds_to_next_alarm;

    if (current_secs_since_midnight < alarm_secs_since_midnight)
    {
        // 还没到今天的闹钟时间
        seconds_to_next_alarm = alarm_secs_since_midnight - current_secs_since_midnight;
    }
    else
    {
        // 已过了今天的闹钟时间，等到明天
        seconds_to_next_alarm = (24u * 3600u - current_secs_since_midnight)
                                + alarm_secs_since_midnight;
    }

    uint32_t current_counter = RTC_GetCounter();

    return current_counter + seconds_to_next_alarm;
}

/**
  * 函    数：更新闹钟显示和RTC闹钟寄存器
  * 参    数：in_alarm_period - 当前是否在闹钟维持期内
  * 返 回 值：无
  */
void UpdateAlarmDisplay(uint8_t in_alarm_period)
{
    if (in_alarm_period)
    {
        // 在闹钟维持期内：设置闹钟为维持结束时刻
        MyRTC_ReadTime();
        uint32_t current_counter = RTC_GetCounter();

        uint16_t current_minutes = (uint16_t)MyRTC_Time[3] * 60u + MyRTC_Time[4];
        uint16_t current_seconds_in_minute = MyRTC_Time[5];

        int32_t remaining_seconds;

        if (ALARM_END_MINS_MIDNIGHT <= ALARM_START_MINS_MIDNIGHT)
        {
            // 跨天情况
            if (current_minutes >= ALARM_START_MINS_MIDNIGHT)
            {
                remaining_seconds = (int32_t)(24u * 60u - current_minutes + ALARM_END_MINS_MIDNIGHT) * 60
                                    - (int32_t)current_seconds_in_minute;
            }
            else
            {
                remaining_seconds = (int32_t)(ALARM_END_MINS_MIDNIGHT - current_minutes) * 60
                                    - (int32_t)current_seconds_in_minute;
            }
        }
        else
        {
            // 同一天情况
            remaining_seconds = (int32_t)(ALARM_END_MINS_MIDNIGHT - current_minutes) * 60
                                - (int32_t)current_seconds_in_minute;
        }

        if (remaining_seconds < 0)
        {
            remaining_seconds = 0;
        }

        uint32_t maintain_end_counter = current_counter + (uint32_t)remaining_seconds;

        RTC_SetAlarm(maintain_end_counter);
        RTC_WaitForLastTask();

        OLED_ShowString(4, 6, "ALARM ON");
    }
    else
    {
        // 不在闹钟维持期：设置下一个闹钟开始时刻
        uint32_t next_alarm = GetNextAlarmStartTime();

        RTC_SetAlarm(next_alarm);
        RTC_WaitForLastTask();

        OLED_ShowString(4, 6, "WAIT ALR");
    }
}

int main(void)
{
    /*======================================================
     * 安全启动窗口（防止反复进入待机模式导致无法下载程序）
     *======================================================*/
    #define SAFE_BOOT_WINDOW_MS  3000u

    /*模块初始化*/
    OLED_Init();        // OLED初始化
	LED_Init(GPIO_Pin_1);
    MyRTC_Init();       // RTC初始化
    USART1_Init();      // USART1初始化（用于与ESP32通信）
    Delay_ms(10);       // 等待USART外设稳定，防止第一个字节发送损坏

    /*开启PWR时钟（待机模式必须）*/
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);

    /*安全启动倒计时——显示在OLED上，方便确认*/
    #if SAFE_BOOT_WINDOW_MS > 0
    {
        uint32_t remaining = SAFE_BOOT_WINDOW_MS / 1000u;
        while (remaining > 0)
        {
            OLED_ShowString(1, 1, "BOOT WAIT:");
            OLED_ShowNum(1, 11, remaining, 1);
            OLED_ShowString(1, 12, "s ");
            Delay_ms(1000);
            remaining--;
        }
        OLED_Clear();
    }
    #endif

    /*NTP时间同步（通过ESP32获取网络时间）*/
    OLED_ShowString(1, 1, "NTP Syncing...  ");
    if (NTP_SyncTime())
    {
        OLED_ShowString(1, 1, "NTP OK!         ");
    }
    else
    {
        OLED_ShowString(1, 1, "NTP FAIL-RTC OK ");  // 同步失败，继续使用RTC现有时间
    }
    Delay_ms(1500);
    OLED_Clear();

    /*显示静态字符串*/
    OLED_ShowString(1, 1, "CNT:");
    OLED_ShowString(2, 1, "CFG:");
    OLED_ShowNum(2, 5, ALARM_START_HOUR,   2);
    OLED_ShowString(2, 7, ":");
    OLED_ShowNum(2, 8, ALARM_START_MINUTE, 2);
    OLED_ShowString(3, 1, "ALRF:");
    OLED_ShowString(4, 1, "MODE:");

    /*使能WKUP引脚（PA0上升沿唤醒待机模式）*/
    PWR_WakeUpPinCmd(ENABLE);

    /*明确读取一次时间，确保MyRTC_Time[]是最新值*/
    MyRTC_ReadTime();

    /*检查当前是否在闹钟维持期，并设置RTC闹钟*/
    uint8_t in_alarm_period = IsInAlarmMaintainPeriod();
    UpdateAlarmDisplay(in_alarm_period);
    LED_set(GPIOA, GPIO_Pin_1, in_alarm_period);  // 启动时立即同步LED状态

    while (1)
    {
        /*更新动态显示*/
        OLED_ShowNum(1, 5, RTC_GetCounter(), 10);               // CNT 秒计数器
        OLED_ShowNum(3, 6, RTC_GetFlagStatus(RTC_FLAG_ALR), 1); // ALRF 标志位

        /*检查闹钟标志*/
        if (RTC_GetFlagStatus(RTC_FLAG_ALR) == SET)
        {
            RTC_ClearFlag(RTC_FLAG_ALR);
            RTC_WaitForLastTask();

            in_alarm_period = IsInAlarmMaintainPeriod();
            UpdateAlarmDisplay(in_alarm_period);
            LED_set(GPIOA, GPIO_Pin_1, in_alarm_period);  // 闹钟状态切换时立即同步LED
        }

        /*Running 指示灯*/
        OLED_ShowString(3, 8, "*");
        Delay_ms(1000);
        OLED_ShowString(3, 8, " ");
        Delay_ms(1000);

        /*根据是否在闹钟期决定是否进入待机模式*/
        if (!in_alarm_period)
        {
            // 非闹钟期：短暂提示后进入待机
			LED_set(GPIOA, GPIO_Pin_1, 0);
            OLED_ShowString(4, 1, "MODE:STANDBY    ");
            Delay_ms(6000);

            OLED_Clear();           // 清屏（关闭显示，降低功耗）
            PWR_EnterSTANDBYMode(); // 进入待机模式；唤醒后从头开始运行
            /*--- 唤醒后程序重新从 main() 顶部执行 ---*/
        }
        else
        {
            // 闹钟期：保持运行，每秒刷新一次
			LED_set(GPIOA, GPIO_Pin_1, 1);
            Delay_ms(800);
        }
    }
}
