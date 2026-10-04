#ifndef DXL_MX64_H
#define DXL_MX64_H

#include "main.h"
#include <stdint.h>

#define DXL_UART                 USART3
#define DXL_BAUD                 1000000U   /* 모터 Baud Rate 설정과 맞출 것 */

#define DXL_ADDR_TORQUE_ENABLE   64
#define DXL_ADDR_GOAL_VELOCITY   104
#define DXL_ADDR_PRESENT_VEL     128
#define DXL_VELOCITY_LIMIT       285

/* 반환값 규약 (read 계열)
 *   0        : 정상
 *   0x01~0x7F: 모터가 보고한 에러 byte (Alert bit 제외)
 *  -1        : 응답 없음 / CRC 오류 */

int dxl_init(uint8_t id);
void dxl_clear_ids(void);
const uint8_t *dxl_get_ids(void);
uint8_t dxl_get_count(void);
int readVelAll(int32_t *vel, int *err);

void torqueOnDXL(void);                       /* broadcast(0xFE) */
void torqueOffDXL(void);                      /* broadcast(0xFE) */
void setVel(uint8_t id, int velocity);        /* Goal Velocity 설정 */
int  readVel(uint8_t id, int *vel);           /* Present Velocity 읽기 */
int setVels(int velocities[]);
int readVels(int velocities[]);

unsigned short update_crc(unsigned short crc_accum, unsigned char *data_blk_ptr, unsigned short data_blk_size);

#endif /* DXL_MX64_H */
