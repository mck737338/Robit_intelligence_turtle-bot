#ifndef READ_PSD_H
#define READ_PSD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

#define PSD_MAX_CHANNEL     2      /* 사용 가능한 ADC1 채널: IN0 ~ IN2 */

#ifndef FILTER_SIZE
#define FILTER_SIZE         10     /* 이동평균 필터 기본 길이. 52 packet(psd_set_filter_size)으로 실행 중 변경 가능 */
#endif

/* ------------------------------------------------------------------ */
/* 외부 코드에서 참조하는 전역 변수                                       */
/* ------------------------------------------------------------------ */

/**
 * 변환 완료된 값(0~4095)이 복사되는 배열. DMA가 쓰는 버퍼와는 별개다.
 * adc[i] = psd_init을 i+1번째로 호출한 채널의 값.
 * 크기는 psd_init 호출 횟수와 같고, psd_start() 이후에는 변하지 않는다.
 */
extern int *adc;

/* update_adc()가 값을 갱신할 때마다 1로 설정. 사용하는 쪽에서 0으로 지운다. */
extern volatile uint8_t adc_updated_flag;

/**
 * @brief  ADC 채널을 채널 목록에 추가 (동적 할당)
 * @param  channel ADC1 채널 번호 (0 ~ 2)
 * @retval 0 성공, -1 실패 (범위 밖, 중복, 이미 start됨, 메모리 부족)
 * @note   호출한 순서가 곧 변환 순서(Rank)이자 adc[] 인덱스이다.
 */
int psd_init(uint8_t channel);

/**
 * @brief  채널 목록대로 ADC 순서를 설정하고 TIM8 + ADC1 DMA 변환 시작.
 *         변환이 필터 크기(기본 FILTER_SIZE)만큼 누적될 때까지 기다린 뒤 return한다. (블로킹)
 *         psd_init을 모두 호출한 뒤 한 번만 호출한다.
 * @retval 0 성공(이동평균 이력이 실제 변환값으로 채워진 상태), -1 실패 또는 시간 초과
 * @note   저장 위치를 바꾸려면 set_addr()를 psd_start 전에 호출한다.
 */
int psd_start(void);

/**
 * @brief  이동평균 필터: 채널 ch의 이력 배열을 한 칸씩 밀고 새 값을 맨 뒤에 넣은 뒤
 *         이력 전체의 합 / 필터 크기를 반환한다.
 * @param  ch    채널 목록 인덱스 (psd_init 호출 순서, 0부터)
 * @param  value 새로 변환된 값
 * @note   psd_start가 필터 크기만큼 먼저 누적하므로 psd_start 이후에는 이력이 모두 유효하다.
 */
int maFilter(uint8_t ch, int value);

/**
 * @brief  이동평균 필터 크기 변경 (모든 채널에 동일하게 적용). 메인 루프에서 호출한다.
 *         채널이 등록되어 있으면 이력 배열을 새 크기로 다시 할당하고, 각 채널의 최근 값으로 채운다.
 *         (평균이 0에서 다시 올라가는 일을 막기 위함)
 * @param  n 필터 크기 (1 이상). 1이면 필터를 적용하지 않는 것과 같다.
 * @retval 0 성공, -1 실패 (n이 0이거나 메모리 부족. 실패 시 기존 크기 유지)
 * @note   psd_clear()를 호출하면 필터 크기는 기본값(FILTER_SIZE)으로 돌아간다.
 */
int psd_set_filter_size(uint16_t n);

/* 현재 필터 크기 */
uint16_t psd_get_filter_size(void);

/**
 * @brief  변환값 저장 위치를 외부 배열로 변경
 * @param  addr 변환값을 저장할 배열 (크기 >= psd_get_count(), 전역/static 등 수명이 유지되는 배열)
 *              NULL을 넘기면 기본 저장 위치(adc[])로 되돌린다.
 * @note   설정한 뒤에는 update_adc()가 adc[]가 아닌 addr[]에만 값을 쓴다.
 *         addr[i]의 인덱스 i는 psd_init 호출 순서와 같다.
 */
void set_addr(int addr[]);

/**
 * @brief  DMA 버퍼의 변환값을 이동평균 필터에 누적하고, 평균값을 저장 위치
 *         (기본 adc[], set_addr 설정 시 해당 배열)에 저장
 *         HAL_ADC_ConvCpltCallback에서 호출된다.
 */
void update_adc(void);

/**
 * @brief  psd_init으로 등록한 모든 채널을 삭제하고 동적 할당 메모리를 해제한다.
 *         변환이 동작 중이면 ADC DMA와 TIM8을 먼저 정지한다.
 * @note   호출 후 adc는 NULL이 되므로 참조하면 안 된다. set_addr 설정도 해제된다.
 *         이후 psd_init -> psd_start로 다시 시작할 수 있다.
 */
void psd_clear(void);

/* 등록된 채널 수 */
uint8_t psd_get_count(void);

#ifdef __cplusplus
}
#endif

#endif /* READ_PSD_H */
