#ifndef __MAIN_H
#define __MAIN_H
#include "stm32f1_host_stub.h"
/* Mock board GPIO/IRQ interfaces used by the actual key_port.c driver. */
#define key1_Pin LL_GPIO_PIN_12
#define key1_GPIO_Port GPIOB
#define led_Pin LL_GPIO_PIN_13
#define led_GPIO_Port GPIOC
#define LL_GPIO_MODE_INPUT 2U
#define LL_GPIO_PULL_UP 1U
#define LL_GPIO_SPEED_FREQ_LOW 0U
static inline uint32_t __get_PRIMASK(void) { return 0U; }
static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline uint32_t LL_GPIO_IsInputPinSet(GPIO_TypeDef *port, uint32_t pin) {
    return (port->IDR & pin) ? 1U : 0U;
}
static inline void LL_GPIO_TogglePin(GPIO_TypeDef *port, uint32_t pin) {
    port->ODR ^= pin;
}
#endif
