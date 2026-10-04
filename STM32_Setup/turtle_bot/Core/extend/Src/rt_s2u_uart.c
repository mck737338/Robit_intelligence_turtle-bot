#include "rt_s2u_uart.h"

/* ------------------------------------------------------------------ */
/* 공개 변수                                                           */
/* ------------------------------------------------------------------ */
int               proc_velocity[VEL_CH_COUNT];
int               proc_psd[PSD_CH_COUNT];
volatile uint8_t  proc_running       = 0;
volatile uint16_t proc_vel_period_ms = 0;
volatile uint16_t proc_psd_period_ms = 0;

/* ------------------------------------------------------------------ */
/* 내부 상태                                                           */
/* ------------------------------------------------------------------ */
static USART_TypeDef *s_usart;
static uint8_t        s_rx_buf[PROC_RX_BUF_SIZE];

/* 요청 대기 비트: bit0~2 = 속도 요청(20,21,22), bit3~6 = PSD 요청(40,41,42,43) */
#define PEND_VEL_SHIFT  0
#define PEND_PSD_SHIFT  3
static volatile uint8_t s_pending = 0;

typedef enum {
    ST_HEADER0,
    ST_HEADER1,
    ST_ID,
    ST_DATA
} ParserState_t;

static struct {
    ParserState_t state;
    uint8_t       id;
    uint8_t       need;      /* 받아야 할 Data 길이 */
    uint8_t       idx;       /* 지금까지 받은 Data 길이 */
    uint8_t       data[6];   /* 최대 Data 길이 6 byte */
} s_parser = { ST_HEADER0, 0, 0, 0, {0} };

/* ------------------------------------------------------------------ */
/* 유틸                                                                */
/* ------------------------------------------------------------------ */

/* Packet ID -> Data 길이. 유효하지 않은 ID는 -1 */
static int data_length(uint8_t id)
{
    switch (id) {
    case 0x00:                                  return 1;   /* 시작/종료 */
    case 0x10:                                  return 4;   /* velocity0, 1 */
    case 0x11: case 0x12:                       return 2;   /* velocity0 / 1 */
    case 0x20: case 0x21: case 0x22:            return 0;   /* velocity 요청 */
    case 0x30:                                  return 6;   /* psd0, 1, 2 */
    case 0x31: case 0x32: case 0x33:            return 2;   /* psd0 / 1 / 2 */
    case 0x40: case 0x41: case 0x42: case 0x43: return 0;   /* psd 요청 */
    case 0x50: case 0x51:                       return 2;   /* 자동발행 주기 설정 */
    default:                                    return -1;
    }
}

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int      get_i16(const uint8_t *p) { return (int)(int16_t)get_u16(p); }

/* ------------------------------------------------------------------ */
/* 송신                                                                */
/* ------------------------------------------------------------------ */
void send_velocity(int id, int velocity[])
{
    uint16_t v = (uint16_t)velocity[id];

    uint8_t packet[5] = {
        PKT_HEADER_0, PKT_HEADER_1,                  /* header */
        (uint8_t)(PID_VEL_BOTH + id + 1),            /* packet id: 0x11 / 0x12 */
        (uint8_t)(v & 0xFF),                         /* v_L */
        (uint8_t)((v >> 8) & 0xFF)                   /* v_H */
    };

    transmit(s_usart, packet, 5);
}

void send_velocities(int velocity[])
{
    uint16_t v0 = (uint16_t)velocity[0];
    uint16_t v1 = (uint16_t)velocity[1];

    uint8_t packet[7] = {
        PKT_HEADER_0, PKT_HEADER_1,                  /* header */
        PID_VEL_BOTH,                                /* packet id: 0x10 */
        (uint8_t)(v0 & 0xFF),                        /* v0_L */
        (uint8_t)((v0 >> 8) & 0xFF),                 /* v0_H */
        (uint8_t)(v1 & 0xFF),                        /* v1_L */
        (uint8_t)((v1 >> 8) & 0xFF)                  /* v1_H */
    };

    transmit(s_usart, packet, 7);
}

void send_psd(int id, int parameter[])
{
    uint16_t p = (uint16_t)parameter[id];

    uint8_t packet[5] = {
        PKT_HEADER_0, PKT_HEADER_1,
        (uint8_t)(PID_PSD_ALL + id + 1),             /* packet id: 0x31 / 0x32 / 0x33 */
        (uint8_t)(p & 0xFF),
        (uint8_t)((p >> 8) & 0xFF)
    };

    transmit(s_usart, packet, 5);
}

void send_psds(int parameter[])
{
    uint16_t p0 = (uint16_t)parameter[0];
    uint16_t p1 = (uint16_t)parameter[1];
    uint16_t p2 = (uint16_t)parameter[2];

    uint8_t packet[9] = {
        PKT_HEADER_0, PKT_HEADER_1,
        PID_PSD_ALL,                                 /* packet id: 0x30 */
        (uint8_t)(p0 & 0xFF), (uint8_t)((p0 >> 8) & 0xFF),
        (uint8_t)(p1 & 0xFF), (uint8_t)((p1 >> 8) & 0xFF),
        (uint8_t)(p2 & 0xFF), (uint8_t)((p2 >> 8) & 0xFF)
    };

    transmit(s_usart, packet, 9);
}

/* ------------------------------------------------------------------ */
/* 수신: 완성된 packet 1개 처리                                          */
/* ------------------------------------------------------------------ */
static void handle_packet(uint8_t id, const uint8_t *d)
{
    switch (id) {
    case 0x00:                                       /* 프로그램 시작/종료 */
        if      (d[0] == PROGRAM_START) proc_running = 1;
        else if (d[0] == PROGRAM_STOP)  proc_running = 0;
        break;                                       /* 그 외 값은 폐기 */

    case 0x10:                                       /* velocity0, velocity1 */
        proc_velocity[0] = get_i16(&d[0]);
        proc_velocity[1] = get_i16(&d[2]);
        break;
    case 0x11:                                       /* velocity0 */
        proc_velocity[0] = get_i16(d);
        break;
    case 0x12:                                       /* velocity1 */
        proc_velocity[1] = get_i16(d);
        break;

    case 0x20: case 0x21: case 0x22:                 /* velocity 요청 */
        s_pending |= (uint8_t)(1U << (PEND_VEL_SHIFT + (id & 0x0F)));
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:      /* psd 요청 */
        s_pending |= (uint8_t)(1U << (PEND_PSD_SHIFT + (id & 0x0F)));
        break;

    case 0x50:                                       /* velocity 자동발행 주기 */
        proc_vel_period_ms = get_u16(d);
        break;
    case 0x51:                                       /* psd 자동발행 주기 */
        proc_psd_period_ms = get_u16(d);
        break;

    default:                                         /* 30~33: STM32 -> 호스트 방향이므로 무시 */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 수신: 바이트 단위 상태머신                                             */
/* ------------------------------------------------------------------ */
static void parse_byte(uint8_t b)
{
    switch (s_parser.state) {
    case ST_HEADER0:
        if (b == PKT_HEADER_0) s_parser.state = ST_HEADER1;
        break;

    case ST_HEADER1:
        if      (b == PKT_HEADER_1) s_parser.state = ST_ID;
        else if (b == PKT_HEADER_0) s_parser.state = ST_HEADER1;   /* AA AA 55 대응 */
        else                        s_parser.state = ST_HEADER0;
        break;

    case ST_ID: {
        int len = data_length(b);
        if (len < 0) {                               /* 잘못된 ID: 폐기 후 Header 재탐색 */
            s_parser.state = (b == PKT_HEADER_0) ? ST_HEADER1 : ST_HEADER0;
            break;
        }
        s_parser.id   = b;
        s_parser.need = (uint8_t)len;
        s_parser.idx  = 0;
        if (len == 0) {                              /* 요청 packet: Data 없음 -> 바로 완성 */
            handle_packet(b, s_parser.data);
            s_parser.state = ST_HEADER0;
        } else {
            s_parser.state = ST_DATA;
        }
        break;
    }

    case ST_DATA:
        s_parser.data[s_parser.idx++] = b;
        if (s_parser.idx >= s_parser.need) {
            handle_packet(s_parser.id, s_parser.data);
            s_parser.state = ST_HEADER0;
        }
        break;
    }
}

void read_packet(uint8_t *data, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        parse_byte(data[i]);
    }
}

/* ------------------------------------------------------------------ */
/* 초기화 / 요청 응답                                                    */
/* ------------------------------------------------------------------ */
void rt_s2u_init(USART_TypeDef *USART, int baud_rate)
{
    s_usart = USART;

    uart_init(USART, (unsigned int)baud_rate);
    uart_set_rx_callback(USART, read_packet);
    receive(USART, s_rx_buf, PROC_RX_BUF_SIZE);
}


void proc_process(void)
{
    uint8_t pending;

    /* 인터럽트에서 세운 요청 비트를 원자적으로 가져오고 클리어 */
    __disable_irq();
    pending = s_pending;
    s_pending = 0;
    __enable_irq();

    if (!pending) return;

    /* velocity 요청: 20 -> 10, 21 -> 11, 22 -> 12 */
    if (pending & (1U << (PEND_VEL_SHIFT + 0))) send_velocities(proc_velocity);
    if (pending & (1U << (PEND_VEL_SHIFT + 1))) send_velocity(0, proc_velocity);
    if (pending & (1U << (PEND_VEL_SHIFT + 2))) send_velocity(1, proc_velocity);

    /* psd 요청: 40 -> 30, 41 -> 31, 42 -> 32, 43 -> 33 */
    if (pending & (1U << (PEND_PSD_SHIFT + 0))) send_psds(proc_psd);
    if (pending & (1U << (PEND_PSD_SHIFT + 1))) send_psd(0, proc_psd);
    if (pending & (1U << (PEND_PSD_SHIFT + 2))) send_psd(1, proc_psd);
    if (pending & (1U << (PEND_PSD_SHIFT + 3))) send_psd(2, proc_psd);
}
