#include "read_psd.h"
#include "adc.h"      /* hadc1 */
#include "tim.h"      /* htim8 */
#include <stdlib.h>

/* psd_start가 필터 크기만큼 누적을 기다리는 최대 시간(ms). 변환 주기 * 필터 크기보다 커야 한다 */
#ifndef PSD_START_TIMEOUT_MS
#define PSD_START_TIMEOUT_MS   2000
#endif

/* 변환 순서 설정 시 사용할 샘플링 시간. CubeMX에서 정한 값에 맞춰 수정 */
#ifndef PSD_SAMPLING_TIME
#define PSD_SAMPLING_TIME   ADC_SAMPLETIME_480CYCLES
#endif

/* ------------------------------------------------------------------ */
/* 공개 변수                                                           */
/* ------------------------------------------------------------------ */
int              *adc = NULL;
volatile uint8_t  adc_updated_flag = 0;

/* ------------------------------------------------------------------ */
/* 내부 상태                                                           */
/* ------------------------------------------------------------------ */
static uint8_t  *s_channels = NULL;   /* 채널 목록 (동적 할당) */
static uint8_t   s_count    = 0;      /* 등록된 채널 수 */
static uint32_t *s_dma_buf  = NULL;   /* DMA가 변환값을 쓰는 버퍼 (동적 할당) */
static uint8_t   s_started  = 0;
static int     **s_hist     = NULL;   /* 이동평균 이력 [채널 개수][s_filter_size] (동적 할당) */
static uint16_t  s_filter_size = FILTER_SIZE;   /* 현재 필터 크기 (기본 FILTER_SIZE, 52 packet으로 변경) */
static volatile uint32_t s_sample_count = 0;   /* 변환 완료 횟수 (update_adc에서 증가) */
static int      *s_dest     = NULL;   /* set_addr로 지정한 저장 위치 (NULL이면 adc[] 사용) */

/* 채널 번호 -> HAL 채널 상수 */
static uint32_t to_hal_channel(uint8_t ch)
{
    switch (ch) {
    case 0:  return ADC_CHANNEL_0;
    case 1:  return ADC_CHANNEL_1;
    default: return ADC_CHANNEL_2;
    }
}

/* ------------------------------------------------------------------ */
/* 채널 등록                                                           */
/* ------------------------------------------------------------------ */
int psd_init(uint8_t channel)
{
    if (s_started)               return -1;   /* start 이후에는 추가 불가 */
    if (channel > PSD_MAX_CHANNEL) return -1;

    for (uint8_t i = 0; i < s_count; i++) {   /* 중복 등록 방지 */
        if (s_channels[i] == channel) return -1;
    }

    /* 채널 목록 확장 */
    uint8_t *new_ch = (uint8_t *)realloc(s_channels, (size_t)(s_count + 1) * sizeof(uint8_t));
    if (new_ch == NULL) return -1;
    s_channels = new_ch;

    /* 변환값 복사용 배열 확장 */
    int *new_adc = (int *)realloc(adc, (size_t)(s_count + 1) * sizeof(int));
    if (new_adc == NULL) return -1;
    adc = new_adc;

    /* 이동평균 이력 배열 확장: 행 포인터 추가 + 새 채널의 행(s_filter_size개) 할당 */
    int **new_hist = (int **)realloc(s_hist, (size_t)(s_count + 1) * sizeof(int *));
    if (new_hist == NULL) return -1;
    s_hist = new_hist;

    s_hist[s_count] = (int *)calloc(s_filter_size, sizeof(int));   /* 0으로 초기화 */
    if (s_hist[s_count] == NULL) return -1;

    s_channels[s_count] = channel;
    adc[s_count]        = 0;
    s_count++;

    return 0;
}

uint8_t psd_get_count(void)
{
    return s_count;
}

/* ------------------------------------------------------------------ */
/* 채널 삭제 + 동적 할당 메모리 해제                                      */
/* ------------------------------------------------------------------ */
void psd_clear(void)
{
    /* 1) 인터럽트(update_adc)가 더 이상 메모리를 쓰지 않도록 먼저 막고 변환 정지 */
    s_started = 0;
    HAL_ADC_Stop_DMA(&hadc1);
    HAL_TIM_Base_Stop_IT(&htim8);

    /* 2) 이동평균 이력 배열 해제: 각 채널의 행 -> 행 포인터 배열 */
    if (s_hist != NULL) {
        for (uint8_t i = 0; i < s_count; i++) {
            free(s_hist[i]);
        }
        free(s_hist);
        s_hist = NULL;
    }

    /* 3) 나머지 동적 할당 메모리 해제 */
    free(s_channels);   s_channels = NULL;
    free(s_dma_buf);    s_dma_buf  = NULL;
    free(adc);          adc        = NULL;

    /* 4) 상태 초기화 (이후 psd_init부터 다시 시작 가능) */
    s_count          = 0;
    s_sample_count   = 0;
    s_dest           = NULL;   /* set_addr 설정도 해제 (외부 배열 자체는 건드리지 않음) */
    s_filter_size    = FILTER_SIZE;   /* 필터 크기도 기본값으로 */
    adc_updated_flag = 0;
}

int maFilter(uint8_t ch, int value)
{
    int *h = s_hist[ch];
    int  n = (int)s_filter_size;

    /* 누적값 갱신: 한 칸씩 앞으로 밀고 새 값을 맨 뒤에 저장 */
    for (int i = 0; i < n - 1; i++) {
        h[i] = h[i + 1];
    }
    h[n - 1] = value;

    /* 총합 / 필터 크기 */
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += h[i];
    }
    return sum / n;
}

uint16_t psd_get_filter_size(void)
{
    return s_filter_size;
}

int psd_set_filter_size(uint16_t n)
{
    if (n == 0) return -1;                       /* 0은 무효 */
    if (n == s_filter_size) return 0;

    /* 등록된 채널이 없으면 값만 저장. 이후 psd_init이 이 크기로 이력을 할당한다 */
    if (s_count == 0) {
        s_filter_size = n;
        return 0;
    }

    /* 1) 인터럽트 밖에서 새 이력 배열을 만든다. 각 채널의 최근 값으로 채워 평균이 튀지 않게 한다 */
    int **new_hist = (int **)malloc((size_t)s_count * sizeof(int *));
    if (new_hist == NULL) return -1;

    for (uint8_t i = 0; i < s_count; i++) {
        new_hist[i] = (int *)malloc((size_t)n * sizeof(int));
        if (new_hist[i] == NULL) {               /* 실패: 지금까지 만든 것 해제, 기존 크기 유지 */
            for (uint8_t j = 0; j < i; j++) free(new_hist[j]);
            free(new_hist);
            return -1;
        }
        int latest = s_hist[i][s_filter_size - 1];   /* 가장 최근 샘플 */
        for (uint16_t k = 0; k < n; k++) new_hist[i][k] = latest;
    }

    /* 2) 인터럽트(update_adc)와 겹치지 않도록 교체는 인터럽트를 막은 채 한 번에 */
    int **old_hist = s_hist;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_hist        = new_hist;
    s_filter_size = n;
    __set_PRIMASK(primask);

    /* 3) 이전 배열 해제 (교체 후이므로 인터럽트가 더 이상 쓰지 않음) */
    for (uint8_t i = 0; i < s_count; i++) free(old_hist[i]);
    free(old_hist);

    return 0;
}

void set_addr(int addr[])
{
    s_dest = addr;
}

/* ------------------------------------------------------------------ */
/* 변환 시작: TIM8 트리거 + ADC1 DMA (circular)                          */
/* ------------------------------------------------------------------ */
int psd_start(void)
{
    if (s_started || s_count == 0) return -1;

    /* CubeMX의 Number Of Conversion이 등록한 채널 수와 같아야 한다 */
    if (hadc1.Init.NbrOfConversion != s_count) return -1;

    s_dma_buf = (uint32_t *)calloc(s_count, sizeof(uint32_t));
    if (s_dma_buf == NULL) return -1;

    /* 채널 목록 순서대로 Rank 1, 2, ... 설정 -> DMA 버퍼 인덱스와 adc[] 인덱스가 일치 */
    for (uint8_t i = 0; i < s_count; i++) {
        ADC_ChannelConfTypeDef cfg = {0};
        cfg.Channel      = to_hal_channel(s_channels[i]);
        cfg.Rank         = (uint32_t)(i + 1);
        cfg.SamplingTime = PSD_SAMPLING_TIME;

        if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) {
            free(s_dma_buf);
            s_dma_buf = NULL;
            return -1;
        }
    }

    /* 사진과 동일한 시작 순서: 타이머 -> ADC DMA */
    s_sample_count = 0;
    s_started = 1;                       /* 인터럽트의 update_adc가 동작하도록 먼저 설정 */

    HAL_TIM_Base_Start_IT(&htim8);
    if (HAL_ADC_Start_DMA(&hadc1, s_dma_buf, s_count) != HAL_OK) {
        s_started = 0;
        HAL_TIM_Base_Stop_IT(&htim8);
        free(s_dma_buf);
        s_dma_buf = NULL;
        return -1;
    }

    /* 필터 크기만큼 변환이 누적될 때까지 대기 후 return */
    uint32_t t0 = HAL_GetTick();
    while (s_sample_count < s_filter_size) {
        if ((HAL_GetTick() - t0) > PSD_START_TIMEOUT_MS) {   /* 트리거 미동작 등 */
            HAL_ADC_Stop_DMA(&hadc1);
            HAL_TIM_Base_Stop_IT(&htim8);
            s_started = 0;
            free(s_dma_buf);
            s_dma_buf = NULL;
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 변환 완료 시 DMA 버퍼 -> adc[] 복사                                    */
/* ------------------------------------------------------------------ */
void update_adc(void)
{
    if (!s_started) return;

    int *dest = (s_dest != NULL) ? s_dest : adc;   /* 저장 위치 선택 */

    for (uint8_t i = 0; i < s_count; i++) {
        dest[i] = maFilter(i, (int)s_dma_buf[i]);   /* 이동평균값 저장 */
    }
    s_sample_count++;
    adc_updated_flag = 1;
}

/* HAL의 weak 콜백을 재정의. DMA 변환(시퀀스 전체)이 끝나면 호출된다. */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        update_adc();
    }
}
