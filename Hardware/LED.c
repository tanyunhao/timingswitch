#include "stm32f10x.h"


/**
 * @brief  初始化LED（GPIOA）
 * @note   可根据需要扩展多路LED
 */
void LED_Init(uint16_t GPIO_Pin) {
	// 使能GPIOA时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	// 默认熄灭LED
	GPIO_ResetBits(GPIOA, GPIO_Pin);
}

/**
 * @brief  控制LED状态
 * @param  GPIOx: GPIO端口
 * @param  GPIO_Pin: 引脚
 * @param  state: 1-亮, 0-灭
 */
void LED_set(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, int state) {
	if (!GPIOx) return;
	if (state) {
		GPIO_SetBits(GPIOx, GPIO_Pin);
	} else {
		GPIO_ResetBits(GPIOx, GPIO_Pin);
	}
}
