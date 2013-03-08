#include <stdio.h>
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"

volatile uint32_t msTicks;                      /* counts 1ms timeTicks       */
volatile uint32_t msDelayCount;

/*----------------------------------------------------------------------------

  SysTick_Handler

 *----------------------------------------------------------------------------*/

void SysTick_Handler(void) {

  msTicks++;
	if (msDelayCount > 0) msDelayCount--;
}

#define ITM_PORT *(volatile unsigned *)0xE0000000

static void msDelay(uint32_t ms) {
	msDelayCount = ms;
  while (msDelayCount > 0);
}

void GPIO_Configuration()
{
  GPIO_InitTypeDef GPIO_InitStructure;

  RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);

  // LED1 -> PB0 , LED2 -> PB1 , LED3 -> PB14 , LED4 -> PB15
  GPIO_InitStructure.GPIO_Pin =  GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_14 | GPIO_Pin_15;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
  GPIO_Init(GPIOB, &GPIO_InitStructure);
}

void SendMsgViaITM(char* msg)
{
  int pos = 0;
	char ch;

	while (msg[pos] != '\0') {
		ch = msg[pos++];
    while (ITM_PORT == 0);       // Wait while Fifo full
		ITM->PORT[0].u8 = ch;
	}
}

int main(void)
{
  long i = 0;
	char buffer[100];

	// configure and enable the SystTick interrupt
	SysTick->LOAD = 0x000000C8*((8000000UL/8)/1000)-1;   // set reload register
  SysTick->CTRL = 0x00000006;                          // set clock source and Interrupt
  SysTick->VAL = 0;                                    // clear  the counter
  SysTick->CTRL |= ((unsigned long)0x00000001);        // enable the counter
	
	GPIO_Configuration();                                // configure the IO pins for the LEDs

  while (1) {
		GPIO_SetBits(GPIOB , GPIO_Pin_0);
		sprintf(&buffer[0], "Switched the LED on. Counter: %d\n", i);
		SendMsgViaITM(buffer);
//		msDelay(1);

		GPIO_ResetBits(GPIOB , GPIO_Pin_0);
		sprintf(&buffer[0], "Switched the LED off. Counter: %d\n", i);
		SendMsgViaITM(buffer);
//    msDelay(1);
    i++;
  }
}
