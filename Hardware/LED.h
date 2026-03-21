#ifndef __LED_H
#define __LED_H
#include "stm32f10x.h" 
/**
 * @brief  初始化LED（GPIOA）
 */
void LED_Init(uint16_t GPIO_Pin) ;

/**
 * @brief  控制LED状态
 * @param  GPIOx: GPIO端口
 * @param  GPIO_Pin: 引脚
 * @param  state: 1-亮, 0-灭
 */
void LED_set(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, int state);

#endif
