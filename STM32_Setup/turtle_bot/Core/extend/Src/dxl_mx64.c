#include "dxl_mx64.h"
#include "uart_communicator.h"
#include <stdlib.h>


#define DXL_RX_BUF_LEN      64
#define DXL_RX_TIMEOUT_MS   15

static uint8_t *dxl_ids = NULL;        /* 등록된 ID (오름차순, 동적할당) */
static uint8_t  dxl_id_count = 0;      /* 등록된 모터 수 */
static uint8_t  dxl_uart_ready = 0;    /* UART/DMA 초기화 여부 */

static uint8_t          dxl_dma_buf[DXL_RX_BUF_LEN];  /* DMA가 채우는 버퍼 */
static volatile uint8_t rx_acc[DXL_RX_BUF_LEN];       /* 콜백이 누적하는 버퍼 */
static volatile int     rx_n;

static int32_t clampVelocity(int32_t vel);

/* IDLE 인터럽트 컨텍스트에서 호출됨: 수신 바이트를 누적만 한다 */
static void dxlRxCallback(uint8_t *buf, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++){
		if (rx_n < DXL_RX_BUF_LEN) rx_acc[rx_n++] = buf[i];
	}
}

int dxl_init(uint8_t id)
{
	if (id > 252) return -1;

	if (!dxl_uart_ready){
		uart_init(DXL_UART, DXL_BAUD);
		uart_set_rx_callback(DXL_UART, dxlRxCallback);
		receive(DXL_UART, dxl_dma_buf, DXL_RX_BUF_LEN);
		dxl_uart_ready = 1;
	}

	/* 삽입 위치 탐색 + 중복 검사 */
	uint8_t pos = 0;
	while (pos < dxl_id_count && dxl_ids[pos] < id) pos++;
	if (pos < dxl_id_count && dxl_ids[pos] == id) return 1;

	/* 1칸 확장. 실패하면 기존 배열은 그대로 유지된다 */
	uint8_t *p = (uint8_t *)realloc(dxl_ids, (size_t)dxl_id_count + 1);
	if (p == NULL) return -1;
	dxl_ids = p;

	/* 뒤로 한 칸씩 밀고 삽입 */
	for (uint8_t i = dxl_id_count; i > pos; i--) dxl_ids[i] = dxl_ids[i - 1];
	dxl_ids[pos] = id;
	dxl_id_count++;
	return 0;
}

/* 등록된 ID 해제 (재설정이 필요할 때) */
void dxl_clear_ids(void)
{
	free(dxl_ids);
	dxl_ids = NULL;
	dxl_id_count = 0;
}

/* 등록된 ID 목록 (오름차순, 읽기 전용). dxl_init 호출 후에는 주소가 바뀔 수 있다 */
const uint8_t *dxl_get_ids(void)   { return dxl_ids; }

/* 등록된 모터 수 */
uint8_t dxl_get_count(void)        { return dxl_id_count; }

/* 누적 버퍼에서 Status Packet 탐색.
 * 성공 시 에러 byte(0~0x7F), 아직 없으면 -1 */
static int parseStatus(uint8_t id, uint16_t len, uint8_t *out)
{
	uint16_t plen  = len + 4;             // INST + ERR + param(len) + CRC(2)
	int      total = plen + 7;
	int      n     = rx_n;

	uint8_t tmp[DXL_RX_BUF_LEN];
	for (int i = 0; i < n; i++) tmp[i] = rx_acc[i];

	for (int i = 0; i + total <= n; i++){
		if (tmp[i] == 0xFF && tmp[i+1] == 0xFF && tmp[i+2] == 0xFD &&
		    tmp[i+3] == 0x00 && tmp[i+4] == id &&
		    tmp[i+5] == (plen & 0xFF) && tmp[i+6] == (plen >> 8) &&
		    tmp[i+7] == 0x55){
			uint16_t rc = tmp[i+total-2] | (tmp[i+total-1] << 8);
			if (rc != update_crc(0, &tmp[i], total - 2)) continue;
			for (uint16_t k = 0; k < len; k++) out[k] = tmp[i + 9 + k];
			return tmp[i + 8] & 0x7F;     // 에러 번호 (Alert bit 제외)
		}
	}
	return -1;
}

/* Read(0x02) 전송 후 Status Packet 수신 */
static int readDXL(uint8_t id, uint16_t addr, uint16_t len, uint8_t *out)
{
	uint8_t p[14] = {
		0xFF, 0xFF, 0xFD, 0x00, id,
		0x07, 0x00,                       // length = param(4) + 3
		0x02,                             // Read
		(uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8),
		(uint8_t)(len & 0xFF),  (uint8_t)(len >> 8),
		0x00, 0x00
	};
	uint16_t c = update_crc(0, p, 12);
	p[12] = c & 0xFF;
	p[13] = c >> 8;

	rx_n = 0;                             // 이전 잔여 데이터 정리
	transmit(DXL_UART, p, 14);            // 에코가 와도 콜백이 같이 누적

	uint32_t t0 = HAL_GetTick();
	while (HAL_GetTick() - t0 < DXL_RX_TIMEOUT_MS){
		int e = parseStatus(id, len, out);
		if (e >= 0) return e;
	}
	return -1;
}

void torqueOnDXL(void){
	uint8_t packet[] = {
			0xFF, 0xFF, 0xFD, 0x00,	//header
			0xFE,	                //id (broadcast)
			0x06, 0x00,	        //length
			0x03,	                //instruction
			(uint8_t)(DXL_ADDR_TORQUE_ENABLE & 0xFF), 0x00,
			0x01,
			0x00, 0x00
	};

	uint16_t crc = update_crc(0, packet, sizeof(packet) - 2);
	packet[11] = crc & 0xFF;
	packet[12] = (crc >> 8) & 0xFF;
	transmit(DXL_UART, packet, sizeof(packet));
}

void torqueOffDXL(void){
	uint8_t packet[] = {
			0xFF, 0xFF, 0xFD, 0x00,	//header
			0xFE,	                //id (broadcast)
			0x06, 0x00,	        //length
			0x03,	                //instruction
			(uint8_t)(DXL_ADDR_TORQUE_ENABLE & 0xFF), 0x00,
			0x00,                   //torque disable
			0x00, 0x00
	};

	uint16_t crc = update_crc(0, packet, sizeof(packet) - 2);
	packet[11] = crc & 0xFF;
	packet[12] = (crc >> 8) & 0xFF;
	transmit(DXL_UART, packet, sizeof(packet));
}

void setVel(uint8_t id, int velocity){
	uint32_t v = (uint32_t)clampVelocity(velocity);

	uint8_t packet[] = {
			0xFF, 0xFF, 0xFD, 0x00,	//header
			id,
			0x09, 0x00,	        //length
			0x03,	                //instruction
			(uint8_t)(DXL_ADDR_GOAL_VELOCITY & 0xFF),
			(uint8_t)((DXL_ADDR_GOAL_VELOCITY >> 8) & 0xFF),
			(uint8_t)(v & 0xFF),
			(uint8_t)((v >> 8) & 0xFF),
			(uint8_t)((v >> 16) & 0xFF),
			(uint8_t)((v >> 24) & 0xFF),
			0x00, 0x00              //crc
	};

	uint16_t crc = update_crc(0, packet, sizeof(packet) - 2);
	packet[14] = crc & 0xFF;
	packet[15] = (crc >> 8) & 0xFF;

	transmit(DXL_UART, packet, sizeof(packet));
}

/* Read(0x02)로 Present Velocity 1개 읽기
 * 반환: 0=정상, 1~0x7F=모터 에러 byte, -1=응답 없음/CRC 오류 */
int readVel(uint8_t id, int *vel)
{
	uint8_t b[4] = {0, 0, 0, 0};
	int ret = readDXL(id, DXL_ADDR_PRESENT_VEL, 4, b);
	if (ret != 0) return ret;             // 실패 시 *vel은 건드리지 않음

	*vel = (int32_t)((uint32_t)b[0]       | ((uint32_t)b[1] << 8) |
	                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
	return 0;
}

/* Sync Write(0x83)로 등록된 모든 모터의 Goal Velocity를 한 번에 쓴다.
 *  velocities[i] -> dxl_ids[i]  (DXL_VELOCITY_LIMIT로 clamp됨)
 * 반환: 0=전송 완료, -1=등록된 ID 없음 또는 메모리 부족
 * (Broadcast 쓰기는 Status Packet이 없으므로 성공 여부는 확인 불가) */
int setVels(int velocities[])
{
	uint8_t n = dxl_id_count;
	if (n == 0 || velocities == NULL) return -1;

	uint16_t L = 5 * n + 7;                       /* param(4 + 5n) + 3 */
	uint8_t *p = (uint8_t *)malloc(14 + 5 * n);
	if (p == NULL) return -1;

	p[0] = 0xFF; p[1] = 0xFF; p[2] = 0xFD; p[3] = 0x00;
	p[4] = 0xFE;                                  /* Broadcast ID */
	p[5] = L & 0xFF; p[6] = L >> 8;
	p[7] = 0x83;                                  /* Sync Write */
	p[8]  = DXL_ADDR_GOAL_VELOCITY & 0xFF;
	p[9]  = (DXL_ADDR_GOAL_VELOCITY >> 8) & 0xFF;
	p[10] = 4; p[11] = 0;                         /* 모터당 데이터 길이 */

	uint16_t k = 12;
	for (uint8_t i = 0; i < n; i++){
		uint32_t v = (uint32_t)clampVelocity(velocities[i]);
		p[k++] = dxl_ids[i];
		p[k++] = v & 0xFF;
		p[k++] = (v >> 8) & 0xFF;
		p[k++] = (v >> 16) & 0xFF;
		p[k++] = (v >> 24) & 0xFF;
	}

	uint16_t crc = update_crc(0, p, k);
	p[k++] = crc & 0xFF;
	p[k++] = crc >> 8;

	transmit(DXL_UART, p, k);
	free(p);
	return 0;
}

/* Sync Read(0x82)로 등록된 모든 모터의 Present Velocity를 한 번에 읽는다.
 *  velocities[i] <- dxl_ids[i]  (응답 성공한 모터만 갱신)
 * 반환: 정상적으로 읽은 모터 수, 인자/버퍼 한계 오류면 -1 */
int readVels(int velocities[])
{
	uint8_t n = dxl_id_count;
	if (n == 0 || velocities == NULL) return -1;

	/* 수신 버퍼 한계: 에코(n+14) + 응답(15n) <= DXL_RX_BUF_LEN */
	if ((n + 14) + 15 * n > DXL_RX_BUF_LEN) return -1;

	uint16_t L = n + 7;                           /* param(4 + n) + 3 */
	uint8_t *p = (uint8_t *)malloc(14 + n);
	uint8_t *done = (uint8_t *)calloc(n, 1);
	if (p == NULL || done == NULL){ free(p); free(done); return -1; }

	p[0] = 0xFF; p[1] = 0xFF; p[2] = 0xFD; p[3] = 0x00;
	p[4] = 0xFE;
	p[5] = L & 0xFF; p[6] = L >> 8;
	p[7] = 0x82;                                  /* Sync Read */
	p[8]  = DXL_ADDR_PRESENT_VEL & 0xFF;
	p[9]  = (DXL_ADDR_PRESENT_VEL >> 8) & 0xFF;
	p[10] = 4; p[11] = 0;
	for (uint8_t i = 0; i < n; i++) p[12 + i] = dxl_ids[i];

	uint16_t crc = update_crc(0, p, 12 + n);
	p[12 + n] = crc & 0xFF;
	p[13 + n] = crc >> 8;

	rx_n = 0;
	transmit(DXL_UART, p, 14 + n);

	int ok = 0, finished = 0;
	uint32_t t0 = HAL_GetTick();
	while (finished < n && HAL_GetTick() - t0 < DXL_RX_TIMEOUT_MS){
		for (uint8_t i = 0; i < n; i++){
			if (done[i]) continue;
			uint8_t b[4] = {0, 0, 0, 0};
			int e = parseStatus(dxl_ids[i], 4, b);   /* 기존 parseStatus 사용 */
			if (e < 0) continue;                      /* 아직 없음 */
			done[i] = 1; finished++;
			if (e == 0){
				velocities[i] = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
				                          ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
				ok++;
			}
		}
	}

	free(p);
	free(done);
	return ok;
}

static int32_t clampVelocity(int32_t vel)
{
    if (vel >  DXL_VELOCITY_LIMIT) return  DXL_VELOCITY_LIMIT;
    if (vel < -DXL_VELOCITY_LIMIT) return -DXL_VELOCITY_LIMIT;
    return vel;
}

unsigned short update_crc(unsigned short crc_accum, unsigned char *data_blk_ptr, unsigned short data_blk_size)
{
    unsigned short i, j;
    static const unsigned short crc_table[256] = {
        0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
        0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
        0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
        0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
        0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
        0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
        0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
        0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
        0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
        0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
        0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
        0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
        0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
        0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
        0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
        0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
        0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
        0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
        0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
        0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
        0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
        0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
        0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
        0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
        0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
        0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
        0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
        0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
        0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
        0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
        0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
        0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202
    };

    for(j = 0; j < data_blk_size; j++)
    {
        i = ((unsigned short)(crc_accum >> 8) ^ data_blk_ptr[j]) & 0xFF;
        crc_accum = (crc_accum << 8) ^ crc_table[i];
    }

    return crc_accum;
}
