#include "turtle_bot.h"
#include "rt_s2u_uart.h"
#include "read_psd.h"
#include "dxl_mx64.h"

/* ------------------------------------------------------------------ */
/* 호스트(Ubuntu)와 통신할 UART 설정                                      */
/* (dxl_mx64는 USART3을 별도로 사용: dxl_init()에서 초기화)               */
/* ------------------------------------------------------------------ */
#ifndef TB_HOST_USART
#define TB_HOST_USART      USART6
#endif
#ifndef TB_HOST_BAUD_RATE
#define TB_HOST_BAUD_RATE  1000000
#endif

/* ================================================================== */
/* dxl_mx64 어댑터 (dxl_mx64.c의 실제 함수에 맞춤)                         */
/*   int  dxl_init(uint8_t id)      0 추가, 1 이미 등록됨, -1 실패          */
/*   void torqueOnDXL(void), torqueOffDXL(void)                           */
/*   void dxl_clear_ids(void)       등록된 ID 해제                          */
/*   int  setVels(int velocities[]) velocities[i] -> i번째(오름차순) ID     */
/*   int  readVels(int velocities[]) 정상적으로 읽은 모터 수 반환            */
/* ================================================================== */
static int  dxl_add(uint8_t id)             { return dxl_init(id); }
static void dxl_torque_on(void)             { torqueOnDXL(); }
static void dxl_torque_off(void)            { torqueOffDXL(); }
static void dxl_release(void)               { dxl_clear_ids(); }
static void dxl_set_vels(int velocities[])  { setVels(velocities); }
static void dxl_read_vels(int velocities[]) { readVels(velocities); }
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* 공개 변수                                                           */
/* ------------------------------------------------------------------ */
volatile uint16_t period_velocity = 0;
volatile uint16_t period_psd      = 0;

/* ------------------------------------------------------------------ */
/* 내부 상태                                                           */
/* ------------------------------------------------------------------ */
static int     *s_written_velocity = NULL;
static int     *s_read_velocity    = NULL;
static int     *s_read_adc         = NULL;

static uint8_t  s_enabled = 0;                       /* tb_start 호출됨 (tb_process 동작 허용) */
static uint8_t  s_running = 0;                       /* 호스트 시작 신호 처리 완료 ~ 종료 신호 */
static int      s_applied_vel[TB_VEL_COUNT];         /* 모터에 마지막으로 출력한 값 */
static uint32_t s_last_vel_tick = 0;
static uint32_t s_last_psd_tick = 0;

/* ------------------------------------------------------------------ */
/* 공개 API                                                            */
/* ------------------------------------------------------------------ */
void tb_init(int written_velocity[], int read_velocity[], int read_adc[])
{
    s_written_velocity = written_velocity;
    s_read_velocity    = read_velocity;
    s_read_adc         = read_adc;

    /* 호스트 통신 초기화: USART6 + DMA2 Stream1 + IDLE 인터럽트 수신 시작 */
    proc_init(TB_HOST_USART, TB_HOST_BAUD_RATE);
}

/* 활성화: tb_process가 호스트 신호에 반응하도록 허용만 한다 */
int tb_start(void)
{
    if (s_written_velocity == NULL || s_read_velocity == NULL || s_read_adc == NULL) return -1;

    s_enabled = 1;
    return 0;
}

/* 호스트의 시작 신호 처리: dxl/psd 설정 후 실행 상태로 진입 */
int tb_host_start(void)
{
    if (s_running) return 0;                         /* 이미 실행 중이면 무시 */
    if (s_written_velocity == NULL || s_read_velocity == NULL || s_read_adc == NULL) {
        proc_running = 0;
        return -1;
    }

    /* --- dxl_mx64 설정 --- */
    if (dxl_add(TB_DXL_ID_0) < 0) goto fail;         /* 1(이미 등록됨)은 정상으로 취급 */
    if (dxl_add(TB_DXL_ID_1) < 0) goto fail;
    dxl_torque_on();

    /* --- read_psd 설정 --- */
    for (uint8_t ch = 0; ch < TB_ADC_COUNT; ch++) {
        if (psd_init(ch) != 0) goto fail;
    }
    set_addr(s_read_adc);                            /* 변환값을 read_adc에 저장 (psd_start 전에 지정) */
    if (psd_start() != 0) goto fail;                 /* 필터 크기(기본 10)만큼 누적 후 return */

    /* --- 상태 초기화 (설정 중에 쌓인 명령/요청 제거) --- */
    for (int i = 0; i < TB_VEL_COUNT; i++) {
        proc_velocity[i]      = 0;
        s_written_velocity[i] = 0;
        s_applied_vel[i]      = 0;
    }
    period_velocity    = 0;
    period_psd         = 0;
    proc_vel_period_ms = 0;
    proc_psd_period_ms = 0;
    proc_take_requests();
    proc_take_filter_size();                         /* 시작 전에 쌓인 필터 변경 요청 버리기 */
    s_last_vel_tick = HAL_GetTick();
    s_last_psd_tick = s_last_vel_tick;

    proc_running = 1;                                /* 시작/종료 외 packet 처리 허용 */
    s_running    = 1;

    send_status(PROGRAM_START);                      /* 시작 신호 송신 */
    return 0;

fail:
    psd_clear();
    dxl_torque_off();
    dxl_release();
    proc_running = 0;                                /* 실패: 다음 시작 신호를 기다림 */
    return -1;
}

/* 호스트의 종료 신호 처리: 정리 후 일시 중지 (시작 신호를 다시 받을 때까지) */
void tb_stop(void)
{
    if (!s_running) return;                          /* 실행 중일 때만 동작 */

    proc_running = 0;                                /* 종료 처리 중 새 명령이 처리되지 않게 */
    s_running    = 0;

    int zero[TB_VEL_COUNT] = {0, 0};
    dxl_set_vels(zero);                              /* 속도 0, 0 출력 */
    dxl_torque_off();
    dxl_release();
    psd_clear();                                     /* 필터 크기도 기본값(10)으로 복귀 */

    period_velocity    = 0;
    period_psd         = 0;
    proc_vel_period_ms = 0;
    proc_psd_period_ms = 0;

    send_status(PROGRAM_STOP);                       /* 종료 신호 송신 */
}

void tb_process(void)
{
    proc_send_errors();                              /* 에러 packet 송신, 파싱 시간 초과 검사 (실행 여부와 무관) */

    if (!s_enabled) return;                          /* tb_start 호출 전 */

    /* 실행 중이 아닐 때: 호스트의 시작 신호(proc_running = 1)를 기다림 */
    if (!s_running) {
        if (proc_running) tb_host_start();
        return;
    }

    /* 실행 중일 때: 호스트의 종료 신호(proc_running = 0) 처리 */
    if (!proc_running) {
        tb_stop();
        return;
    }

    /* ---- 이하는 실행 중일 때만 동작 ---- */

    /* 1) 수신한 velocity 저장, 값이 바뀌었을 때만 모터에 출력 */
    int changed = 0;
    for (int i = 0; i < TB_VEL_COUNT; i++) {
        s_written_velocity[i] = proc_velocity[i];
        if (s_written_velocity[i] != s_applied_vel[i]) changed = 1;
    }
    if (changed) {
        dxl_set_vels(s_written_velocity);
        s_applied_vel[0] = s_written_velocity[0];
        s_applied_vel[1] = s_written_velocity[1];
    }

    /* 2) 자동발행 주기 반영 (50, 51 packet으로 설정된 값) */
    period_velocity = proc_vel_period_ms;
    period_psd      = proc_psd_period_ms;

    /* 3) psd 필터 누적 개수 변경 요청 반영 (52 packet) */
    uint16_t fs = proc_take_filter_size();
    if (fs != 0 && psd_set_filter_size(fs) != 0) {
        report_error(ERR_TARGET_PSD, ERR_INVALID_DATA);   /* 메모리 부족 등으로 변경 실패, 기존 값 유지 */
    }

    /* 4) 호스트 요청 응답 (velocity 응답은 항상 직전에 readVels) */
    uint8_t req = proc_take_requests();
    if (req) {
        if (req & (REQ_VEL_ALL | REQ_VEL_CH0 | REQ_VEL_CH1)) {
            dxl_read_vels(s_read_velocity);
        }
        if (req & REQ_VEL_ALL) send_velocities(s_read_velocity);
        if (req & REQ_VEL_CH0) send_velocity(0, s_read_velocity);
        if (req & REQ_VEL_CH1) send_velocity(1, s_read_velocity);

        if (req & REQ_PSD_ALL) send_psds(s_read_adc);
        if (req & REQ_PSD_CH0) send_psd(0, s_read_adc);
        if (req & REQ_PSD_CH1) send_psd(1, s_read_adc);
        if (req & REQ_PSD_CH2) send_psd(2, s_read_adc);
    }

    /* 5) 주기 발행: 주기가 0이면 보내지 않는다. 겹치면 velocity -> psd 순서 */
    uint32_t now = HAL_GetTick();
    uint8_t vel_due = 0, psd_due = 0;

    if (period_velocity != 0 && (now - s_last_vel_tick) >= period_velocity) {
        s_last_vel_tick = now;
        vel_due = 1;
    }
    if (period_psd != 0 && (now - s_last_psd_tick) >= period_psd) {
        s_last_psd_tick = now;
        psd_due = 1;
    }

    if (vel_due) {
        dxl_read_vels(s_read_velocity);              /* 직전에 readVels */
        send_velocities(s_read_velocity);
    }
    if (psd_due) {
        send_psds(s_read_adc);
    }
}
