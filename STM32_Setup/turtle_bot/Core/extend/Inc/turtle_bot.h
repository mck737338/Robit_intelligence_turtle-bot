#ifndef TURTLE_BOT_H
#define TURTLE_BOT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

#define TB_DXL_ID_0     0x01
#define TB_DXL_ID_1     0x02

#define TB_VEL_COUNT    2      /* written_velocity, read_velocity 배열 크기 */
#define TB_ADC_COUNT    3      /* read_adc 배열 크기 (psd0 ~ psd2) */

/* 자동발행 주기 (ms). 0이면 발행하지 않는다. 기본값 0 */
extern volatile uint16_t period_velocity;
extern volatile uint16_t period_psd;

/**
 * @brief 결과를 저장할 배열의 주소를 등록한다. (배열은 전역/static이어야 한다)
 * @param written_velocity [2] rt_s2u_uart로 수신한 velocity (호스트 명령)
 * @param read_velocity    [2] dxl_mx64(readVels)로 읽은 velocity
 * @param read_adc         [3] read_psd로 읽은 adc (이동평균 적용값)
 */
void tb_init(int written_velocity[], int read_velocity[], int read_adc[]);

/**
 * @brief turtle_bot 활성화. 메인 루프의 tb_process()가 호스트의 시작/종료 신호에 반응하도록 한다.
 *        이 함수는 dxl/psd를 설정하지 않는다. (설정은 호스트의 시작 신호를 받을 때 수행)
 *        rt_s2u_init(), tb_init() 이후에 호출한다.
 * @retval 0 성공, -1 실패 (tb_init 미호출)
 */
int tb_start(void);

/**
 * @brief 호스트의 시작 신호 처리 (실행 중이 아닐 때만 동작). tb_process가 자동 호출한다.
 *        dxl_init(0x01, 0x02) -> torqueOnDXL -> psd_init(0,1,2) -> psd_start
 *        -> 실행 상태 진입 -> send_status(시작)
 * @retval 0 성공, -1 실패 (실패 시 정리 후 다음 시작 신호를 기다림)
 */
int tb_host_start(void);

/**
 * @brief 호스트의 종료 신호 처리 (실행 중일 때만 동작). tb_process가 자동 호출한다.
 *        setVels(0,0) -> torqueOffDXL -> dxl_clear_ids -> psd_clear -> period 0
 *        -> send_status(종료). 이후 시작 신호를 다시 받을 때까지 일시 중지된다.
 */
void tb_stop(void);

/**
 * @brief 메인 루프에서 계속 호출한다. tb_start() 이후에만 동작한다.
 *        - 실행 중이 아닐 때: 호스트의 시작 신호를 받으면 tb_host_start
 *        - 실행 중일 때: 호스트의 종료 신호를 받으면 tb_stop
 *        - 실행 중일 때만: velocity 저장/모터 출력, 요청 응답, 주기 송신
 */
void tb_process(void);

#ifdef __cplusplus
}
#endif

#endif /* TURTLE_BOT_H */
