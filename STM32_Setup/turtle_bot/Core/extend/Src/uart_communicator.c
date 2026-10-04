#include "uart_communicator.h"

volatile uint32_t seq = 0;

/* ================================================================== */
/* UART 설정 테이블 - UART를 추가하려면 이 표에 한 줄만 추가하면 된다.     */
/* (STM32F446 DMA request mapping, RM0390 참고)                        */
/* ================================================================== */
typedef struct {
    USART_TypeDef *usart;
    uint8_t        apb;        /* USART가 연결된 APB 버스 (1 또는 2) */
    uint32_t       clk_mask;   /* LL_APBx_GRPx_PERIPH_USARTx */
    DMA_TypeDef   *dma;        /* RX DMA 컨트롤러 */
    uint32_t       stream;     /* RX DMA 스트림 */
    IRQn_Type      irq;
} UartPort_t;

static const UartPort_t port_table[] = {
    /* usart   apb  clock mask                      dma   stream             irq          */
    { USART3,  1,   LL_APB1_GRP1_PERIPH_USART3,     DMA1, LL_DMA_STREAM_1,   USART3_IRQn },
    { USART6,  2,   LL_APB2_GRP1_PERIPH_USART6,     DMA2, LL_DMA_STREAM_1,   USART6_IRQn },
};
#define PORT_COUNT (sizeof(port_table) / sizeof(port_table[0]))

/* 포트별 런타임 상태 (테이블과 같은 인덱스) */
typedef struct {
    uint8_t           *rx_buf;
    uint16_t           rx_len;
    uart_rx_callback_t cb;
} UartState_t;

static UartState_t state[PORT_COUNT];

static int find_port(USART_TypeDef *USARTx)
{
    for (unsigned i = 0; i < PORT_COUNT; i++) {
        if (port_table[i].usart == USARTx) return (int)i;
    }
    return -1;
}

/* 스트림의 모든 상태 플래그(FE, DME, TE, HT, TC) 클리어 */
static void dma_clear_all_flags(DMA_TypeDef *DMAx, uint32_t stream)
{
    static const uint8_t shift[4] = {0, 6, 16, 22};
    uint32_t mask = 0x3DUL << shift[stream & 3U];

    if (stream < 4U) DMAx->LIFCR = mask;
    else             DMAx->HIFCR = mask;
}

void uart_init(USART_TypeDef *USARTx, unsigned int baud_rate)
{
    int idx = find_port(USARTx);
    if (idx < 0) return;
    const UartPort_t *p = &port_table[idx];

    if (p->apb == 1) LL_APB1_GRP1_EnableClock(p->clk_mask);
    else             LL_APB2_GRP1_EnableClock(p->clk_mask);

    LL_USART_InitTypeDef cfg = {0};
    cfg.BaudRate            = baud_rate;
    cfg.DataWidth           = LL_USART_DATAWIDTH_8B;
    cfg.StopBits            = LL_USART_STOPBITS_1;
    cfg.Parity              = LL_USART_PARITY_NONE;
    cfg.TransferDirection   = LL_USART_DIRECTION_TX_RX;
    cfg.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    cfg.OverSampling        = LL_USART_OVERSAMPLING_16;

    LL_USART_Init(USARTx, &cfg);
    LL_USART_ConfigAsyncMode(USARTx);
    LL_USART_Enable(USARTx);

    NVIC_SetPriority(p->irq, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 0, 0));
    NVIC_EnableIRQ(p->irq);
}

void transmit(USART_TypeDef *USARTx, uint8_t packet[], int length)
{
    seq += 1;
    for (int i = 0; i < length; i++) {
        while (!LL_USART_IsActiveFlag_TXE(USARTx));
        LL_USART_TransmitData8(USARTx, packet[i]);
    }
    while (!LL_USART_IsActiveFlag_TC(USARTx));
}

void receive(USART_TypeDef *USARTx, uint8_t buffer[], int length)
{
    int idx = find_port(USARTx);
    if (idx < 0 || length <= 0) return;
    const UartPort_t *p = &port_table[idx];

    state[idx].rx_buf = buffer;
    state[idx].rx_len = (uint16_t)length;

    /* 스트림 정지 후 완전히 꺼질 때까지 대기 */
    LL_DMA_DisableStream(p->dma, p->stream);
    while (LL_DMA_IsEnabledStream(p->dma, p->stream));
    dma_clear_all_flags(p->dma, p->stream);

    LL_DMA_SetMemoryAddress(p->dma, p->stream, (uint32_t)buffer);
    LL_DMA_SetPeriphAddress(p->dma, p->stream, (uint32_t)&USARTx->DR);
    LL_DMA_SetDataLength(p->dma, p->stream, (uint32_t)length);
    LL_DMA_EnableStream(p->dma, p->stream);

    LL_USART_EnableDMAReq_RX(USARTx);
    LL_USART_EnableIT_IDLE(USARTx);
}

void uart_set_rx_callback(USART_TypeDef *USARTx, uart_rx_callback_t cb)
{
    int idx = find_port(USARTx);
    if (idx >= 0) state[idx].cb = cb;
}

void uart_irq_handler(USART_TypeDef *USARTx)
{
    int idx = find_port(USARTx);
    if (idx < 0) return;
    const UartPort_t *p = &port_table[idx];

    if (LL_USART_IsEnabledIT_IDLE(USARTx) && LL_USART_IsActiveFlag_IDLE(USARTx)) {
        LL_USART_ClearFlag_IDLE(USARTx);

        uint16_t received = state[idx].rx_len - (uint16_t)LL_DMA_GetDataLength(p->dma, p->stream);
        if (received > 0 && state[idx].cb) {
            state[idx].cb(state[idx].rx_buf, received);
        }

        /* 다음 패킷을 위해 DMA 재시작 (Circular 모드라면 이 호출은 불필요) */
        receive(USARTx, state[idx].rx_buf, state[idx].rx_len);
    }
}
