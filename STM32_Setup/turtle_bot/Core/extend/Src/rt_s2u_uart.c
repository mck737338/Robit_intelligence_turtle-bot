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

/* 52 packet으로 받은 필터 누적 개수 (0 = 요청 없음) */
static volatile uint16_t s_filter_req = 0;

/* 에러 통지 상태 */
static volatile uint16_t s_err_pending   = 0;    /* err_table 인덱스별 대기 비트 */
static uint16_t          s_err_sent_once = 0;
static uint32_t          s_err_last_tick[16];
static volatile uint32_t s_last_rx_tick  = 0;    /* 마지막으로 데이터를 받은 시각 */

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
static void tx(uint8_t *packet, int length)
{
    transmit(s_usart, packet, length);
}

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
    case 0x50: case 0x51: case 0x52:            return 2;   /* 자동발행 주기 / 필터 누적 개수 설정 */
    case 0x60: case 0x61: case 0x62:            return 1;   /* 에러 통지 (STM32 -> 호스트 방향, 수신 시 무시) */
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

    tx(packet, 5);
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

    tx(packet, 7);
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

    tx(packet, 5);
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

    tx(packet, 9);
}

void send_status(uint8_t status)
{
    /* 프로토콜상 유효한 값은 시작(0x00), 종료(0xFF)뿐 */
    if (status != PROGRAM_START && status != PROGRAM_STOP) return;

    uint8_t packet[4] = {
        PKT_HEADER_0, PKT_HEADER_1,   /* header */
        PID_PROGRAM,                  /* packet id: 0x00 */
        status                        /* 0x00: 시작, 0xFF: 종료 */
    };

    transmit(s_usart, packet, 4);
}

/* ------------------------------------------------------------------ */
/* 에러 통지 (STM32 -> Ubuntu, AA 55 6x ERR)                             */
/* ------------------------------------------------------------------ */

/* 프로토콜 3.3에서 허용하는 (대상, 코드) 조합 */
static const struct { uint8_t target; uint8_t code; } err_table[] = {
    { ERR_TARGET_COMMON, ERR_UNKNOWN_ID   },   /* 60 01 */
    { ERR_TARGET_COMMON, ERR_INVALID_DATA },   /* 60 02 */
    { ERR_TARGET_COMMON, ERR_TIMEOUT      },   /* 60 03 */
    { ERR_TARGET_COMMON, ERR_NOT_STARTED  },   /* 60 04 */
    { ERR_TARGET_VEL,    ERR_INVALID_DATA },   /* 61 02 */
    { ERR_TARGET_PSD,    ERR_INVALID_DATA },   /* 62 02 */
    { ERR_TARGET_VEL,    ERR_SENSOR_FAULT },   /* 61 10 */
    { ERR_TARGET_PSD,    ERR_SENSOR_FAULT },   /* 62 10 */
    { ERR_TARGET_VEL,    ERR_MOTOR_FAULT  },   /* 61 11 */
};
#define ERR_COUNT  (sizeof(err_table) / sizeof(err_table[0]))

static int err_index(uint8_t target, uint8_t code)
{
    for (unsigned i = 0; i < ERR_COUNT; i++) {
        if (err_table[i].target == target && err_table[i].code == code) return (int)i;
    }
    return -1;
}

int send_error(uint8_t target, uint8_t code)
{
    if (s_usart == NULL || err_index(target, code) < 0) return -1;

    uint8_t packet[4] = {
        PKT_HEADER_0, PKT_HEADER_1,
        (uint8_t)(PID_ERROR + target),   /* packet id: 0x60 / 0x61 / 0x62 */
        code                             /* 에러 코드 */
    };

    transmit(s_usart, packet, 4);
    return 0;
}

void report_error(uint8_t target, uint8_t code)
{
    int idx = err_index(target, code);
    if (idx < 0) return;

    uint32_t primask = __get_PRIMASK();  /* 인터럽트 안에서도 안전하도록 상태 저장/복원 */
    __disable_irq();
    s_err_pending |= (uint16_t)(1U << idx);
    __set_PRIMASK(primask);
}

/* Header 이후 packet이 완성되지 않은 채 PROC_PARSE_TIMEOUT_MS가 지났으면 파서를 초기화하고 TIMEOUT 등록 */
static void check_parse_timeout(void)
{
    uint8_t expired = 0;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (s_parser.state != ST_HEADER0 &&
        (HAL_GetTick() - s_last_rx_tick) > PROC_PARSE_TIMEOUT_MS) {
        /* TIMEOUT 에러는 Header(AA 55)가 완성된 뒤(ID/Data 대기 중)에만 보낸다.
         * AA 하나만 받고 멈춘 경우는 에러 없이 파서만 초기화한다. */
        expired = (s_parser.state == ST_ID || s_parser.state == ST_DATA);
        s_parser.state = ST_HEADER0;
    }
    __set_PRIMASK(primask);

    if (expired) report_error(ERR_TARGET_COMMON, ERR_TIMEOUT);
}

void proc_send_errors(void)
{
    check_parse_timeout();

    uint16_t pending;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    pending = s_err_pending;
    s_err_pending = 0;
    __set_PRIMASK(primask);

    if (!pending) return;

    uint32_t now = HAL_GetTick();
    for (unsigned i = 0; i < ERR_COUNT; i++) {
        uint16_t bit = (uint16_t)(1U << i);
        if (!(pending & bit)) continue;

        /* 전송 간격 제한: 같은 에러가 너무 자주 발생하면 보내지 않고 버린다 */
        if ((s_err_sent_once & bit) && (now - s_err_last_tick[i]) < ERR_MIN_INTERVAL_MS) continue;

        s_err_sent_once    |= bit;
        s_err_last_tick[i]  = now;
        send_error(err_table[i].target, err_table[i].code);
    }
}

/* ------------------------------------------------------------------ */
/* 수신: 완성된 packet 1개 처리                                          */
/* ------------------------------------------------------------------ */
static void handle_packet(uint8_t id, const uint8_t *d)
{
    if (id >= PID_ERROR && id <= PID_ERROR + 2) return;   /* 에러 packet은 STM32 -> 호스트 방향이므로 무시 */

    /* 시작/종료(00)가 아닌 packet은 proc_running이 1일 때만 처리 */
    if (id != PID_PROGRAM && !proc_running) {
        report_error(ERR_TARGET_COMMON, ERR_NOT_STARTED);   /* 시작 전 명령 수신 */
        return;
    }

    switch (id) {
    case 0x00:                                       /* 프로그램 시작/종료 */
        if      (d[0] == PROGRAM_START) proc_running = 1;
        else if (d[0] == PROGRAM_STOP)  proc_running = 0;
        else report_error(ERR_TARGET_COMMON, ERR_INVALID_DATA);   /* 그 외 값은 폐기 */
        break;

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
    case 0x52: {                                     /* psd 필터 누적 개수 */
        uint16_t n = get_u16(d);
        if (n == 0) report_error(ERR_TARGET_PSD, ERR_INVALID_DATA);   /* 0은 무효: 폐기, 기존 설정 유지 */
        else        s_filter_req = n;                                 /* 적용은 메인 루프에서 */
        break;
    }

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
            report_error(ERR_TARGET_COMMON, ERR_UNKNOWN_ID);
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
    check_parse_timeout();                           /* 이전 packet이 미완성인 채 오래됐으면 TIMEOUT */

    for (uint16_t i = 0; i < length; i++) {
        parse_byte(data[i]);
    }

    s_last_rx_tick = HAL_GetTick();
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

void proc_init(USART_TypeDef *USART, int baud_rate)
{
    rt_s2u_init(USART, baud_rate);
}

uint8_t proc_take_requests(void)
{
    uint8_t pending;

    /* 인터럽트에서 세운 요청 비트를 원자적으로 가져오고 클리어 */
    __disable_irq();
    pending = s_pending;
    s_pending = 0;
    __enable_irq();

    return pending;
}

uint16_t proc_take_filter_size(void)
{
    uint16_t n;

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    n = s_filter_req;
    s_filter_req = 0;
    __set_PRIMASK(primask);

    return n;
}

void proc_process(void)
{
    proc_send_errors();
    uint8_t pending = proc_take_requests();
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
