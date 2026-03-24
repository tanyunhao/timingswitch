#ifndef __MYRTC_H
#define __MYRTC_H
#include "stm32f10x.h"

extern uint16_t MyRTC_Time[];

void     MyRTC_Init(void);
void     MyRTC_SetTime(void);
void     MyRTC_ReadTime(void);
uint8_t  NTP_SyncTime(void);
uint32_t MyRTC_CounterToSecsMidnight(uint32_t counter);

#endif
