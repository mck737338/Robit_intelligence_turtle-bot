#ifndef RT_S2U_UART_H
#define RT_S2U_UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "uart_communicator.h"
#include <stdint.h>

#define PROC_BAUD_RATE      1000000U
#define PROC_RX_BUF_SIZE    256

#define VEL_CH_COUNT        2
#define PSD_CH_COUNT        3

/* Header */
#define PKT_HEADER_0        0xAA
#define PKT_HEADER_1        0x55

/* Packet ID */
#define PID_PROGRAM         0x00
#define PID_VEL_BOTH        0x10   /* 0x11: velocity0, 0x12: velocity1 */
#define PID_VEL_REQ_BOTH    0x20   /* 0x21: velocity0, 0x22: velocity1 */
#define PID_PSD_ALL         0x30   /* 0x31: psd0, 0x32: psd1, 0x33: psd2 */
#define PID_PSD_REQ_ALL     0x40   /* 0x41: psd0, 0x42: psd1, 0x43: psd2 */
#define PID_VEL_PERIOD      0x50
#define PID_PSD_PERIOD      0x51

#define PROGRAM_START       0x00
#define PROGRAM_STOP        0xFF

/* ------------------------------------------------------------------ */
/* 다른 코드에서 참조하는 공개 변수                                       */
/* ------------------------------------------------------------------ */
extern int                 proc_velocity[VEL_CH_COUNT];  /* 수신한 속도 명령 / 요청 시 응답할 속도 */
extern int                 proc_psd[PSD_CH_COUNT];       /* 요청 시 응답할 PSD 값 (STM32에서 갱신) */
extern volatile uint8_t  proc_running;                 /* 1: 시작(00 00), 0: 종료(00 FF) */
extern volatile uint16_t proc_vel_period_ms;           /* velocity 자동발행 주기, 0 = 비활성화 */
extern volatile uint16_t proc_psd_period_ms;           /* psd 자동발행 주기, 0 = 비활성화 */

/**
 * @brief UART 초기화 + 수신(DMA/IDLE) 시작 + read_packet 콜백 등록
 */
void rt_s2u_init(USART_TypeDef *USART, int baud_rate);


/**
 * @brief 수신 데이터 해석 (uart_communicator의 IDLE 콜백, 인터럽트 컨텍스트에서 호출됨)
 *        packet이 여러 수신 구간에 걸쳐도 상태머신으로 이어서 해석한다.
 */
void read_packet(uint8_t *data, uint16_t length);

/**
 * @brief 메인 루프에서 호출. 수신한 요청(2x, 4x)에 대한 응답 packet을 송신한다.
 */
void proc_process(void);

/* 송신 (id: 채널 번호 0부터 시작) */
void send_velocity(int id, int velocity[]);     /* 11, 12 */
void send_velocities(int velocity[]);           /* 10 */
void send_psd(int id, int parameter[]);         /* 31, 32, 33 */
void send_psds(int parameter[]);                /* 30 */

#ifdef __cplusplus
}
#endif

#endif /* RT_S2U_UART_H */
