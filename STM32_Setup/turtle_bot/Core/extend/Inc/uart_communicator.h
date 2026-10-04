#ifndef UART_COMMUNICATOR_H
#define UART_COMMUNICATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

/* 송신 패킷 카운터 (transmit 호출 시마다 +1) */
extern volatile uint32_t seq;

/* IDLE 인터럽트 발생 시 호출되는 콜백: (수신 버퍼, 수신된 바이트 수) */
typedef void (*uart_rx_callback_t)(uint8_t *data, uint16_t length);

void uart_init(USART_TypeDef *USARTx, unsigned int baud_rate);
void transmit(USART_TypeDef *USARTx, uint8_t packet[], int length);
void receive(USART_TypeDef *USARTx, uint8_t buffer[], int length);

/* 수신 완료(IDLE) 시 호출할 함수 등록 */
void uart_set_rx_callback(USART_TypeDef *USARTx, uart_rx_callback_t cb);

/* USARTx_IRQHandler 안에서 호출 */
void uart_irq_handler(USART_TypeDef *USARTx);

#ifdef __cplusplus
}
#endif

#endif /* UART_COMMUNICATOR_H */
