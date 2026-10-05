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
#define PID_VEL_PERIOD      0x50   /* velocity 자동발행 주기 설정 */
#define PID_PSD_PERIOD      0x51   /* psd 자동발행 주기 설정 */
#define PID_FILTER_SIZE     0x52   /* psd 필터 누적 개수 설정 */
#define PID_ERROR           0x60   /* 0x60: 공통, 0x61: velocity, 0x62: psd (STM32 -> Ubuntu) */

#define PROGRAM_START       0x00
#define PROGRAM_STOP        0xFF

/* 에러 대상 (6x 하위 nibble) */
#define ERR_TARGET_COMMON   0      /* 60: packet 파싱 등 공통 */
#define ERR_TARGET_VEL      1      /* 61: velocity */
#define ERR_TARGET_PSD      2      /* 62: psd */

/* 에러 코드 */
#define ERR_UNKNOWN_ID      0x01   /* 60 */
#define ERR_INVALID_DATA    0x02   /* 60, 61, 62 */
#define ERR_TIMEOUT         0x03   /* 60 */
#define ERR_NOT_STARTED     0x04   /* 60 */
#define ERR_SENSOR_FAULT    0x10   /* 61, 62 */
#define ERR_MOTOR_FAULT     0x11   /* 61 */

#define PROC_PARSE_TIMEOUT_MS   50   /* Header 이후 packet이 완성되지 않아도 기다리는 최대 시간 */
#define ERR_MIN_INTERVAL_MS     100  /* 같은 (대상, 코드) 에러 packet의 최소 전송 간격 */

/* ------------------------------------------------------------------ */
/* 다른 코드에서 참조하는 공개 변수                                       */
/* ------------------------------------------------------------------ */
extern int                 proc_velocity[VEL_CH_COUNT];  /* 수신한 속도 명령 / 요청 시 응답할 속도 */
extern int                 proc_psd[PSD_CH_COUNT];       /* 요청 시 응답할 PSD 값 (STM32에서 갱신) */
extern volatile uint8_t  proc_running;                 /* 1: 시작(00 00), 0: 종료(00 FF). 1일 때만 그 외 packet을 처리 */
extern volatile uint16_t proc_vel_period_ms;           /* velocity 자동발행 주기, 0 = 비활성화 */
extern volatile uint16_t proc_psd_period_ms;           /* psd 자동발행 주기, 0 = 비활성화 */

/* proc_take_requests()가 반환하는 요청 비트 */
#define REQ_VEL_ALL         (1U << 0)   /* 20 */
#define REQ_VEL_CH0         (1U << 1)   /* 21 */
#define REQ_VEL_CH1         (1U << 2)   /* 22 */
#define REQ_PSD_ALL         (1U << 3)   /* 40 */
#define REQ_PSD_CH0         (1U << 4)   /* 41 */
#define REQ_PSD_CH1         (1U << 5)   /* 42 */
#define REQ_PSD_CH2         (1U << 6)   /* 43 */

/**
 * @brief UART 초기화 + 수신(DMA/IDLE) 시작 + read_packet 콜백 등록
 */
void rt_s2u_init(USART_TypeDef *USART, int baud_rate);

/* rt_s2u_init의 별칭 (기존 코드 호환용) */
void proc_init(USART_TypeDef *USART, int baud_rate);

/**
 * @brief 수신 데이터 해석 (uart_communicator의 IDLE 콜백, 인터럽트 컨텍스트에서 호출됨)
 *        packet이 여러 수신 구간에 걸쳐도 상태머신으로 이어서 해석한다.
 */
void read_packet(uint8_t *data, uint16_t length);

/**
 * @brief 메인 루프에서 호출. 수신한 요청(2x, 4x)에 대한 응답 packet을 송신한다.
 */
void proc_process(void);

/**
 * @brief 수신한 요청 비트(REQ_*)를 가져오고 클리어한다. 응답은 호출한 쪽이 직접 송신한다.
 */
uint8_t proc_take_requests(void);

/**
 * @brief 52 packet으로 받은 psd 필터 누적 개수를 가져오고 클리어한다. (메인 루프에서 호출)
 *        0이 유효하지 않은 값이므로 반환값 0은 "변경 요청 없음"을 뜻한다.
 *        가져온 값은 psd_set_filter_size()로 적용한다.
 */
uint16_t proc_take_filter_size(void);

/* 송신 (id: 채널 번호 0부터 시작) */
void send_velocity(int id, int velocity[]);     /* 11, 12 */
void send_velocities(int velocity[]);           /* 10 */
void send_psd(int id, int parameter[]);         /* 31, 32, 33 */
void send_psds(int parameter[]);                /* 30 */

/**
 * @brief 시작/종료 신호 송신
 * @param status PROGRAM_START(0x00) -> AA 55 00 00, PROGRAM_STOP(0xFF) -> AA 55 00 FF
 */
void send_status(uint8_t status);

/**
 * @brief 에러 packet 즉시 송신 (AA 55 6x ERR). 대상/코드 조합이 프로토콜 3.3에 없으면 보내지 않는다.
 * @param target ERR_TARGET_COMMON / ERR_TARGET_VEL / ERR_TARGET_PSD
 * @param code   ERR_* 코드
 * @return 0 송신, -1 허용되지 않은 조합 또는 초기화 전
 * @note   송신 중인 다른 packet과 섞이지 않도록 메인 루프에서만 호출한다. 인터럽트에서는 report_error를 쓴다.
 */
int send_error(uint8_t target, uint8_t code);

/**
 * @brief 에러 발생 등록 (인터럽트/메인 어디서나 호출 가능). 실제 송신은 proc_send_errors가 한다.
 *        센서/모터 에러는 이 함수로 알린다. 예) report_error(ERR_TARGET_VEL, ERR_MOTOR_FAULT);
 */
void report_error(uint8_t target, uint8_t code);

/**
 * @brief 메인 루프에서 계속 호출. 파싱 시간 초과를 검사하고, 등록된 에러를 송신한다.
 *        같은 에러는 ERR_MIN_INTERVAL_MS 안에 반복되면 보내지 않는다.
 */
void proc_send_errors(void);

#ifdef __cplusplus
}
#endif

#endif /* RT_S2U_UART_H */
