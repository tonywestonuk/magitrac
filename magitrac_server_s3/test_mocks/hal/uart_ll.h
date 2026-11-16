#pragma once
// Test-only stub for hal/uart_ll.h.  Lets midi_player.cpp compile under
// UNIT_TEST.  The FIFO always reports "ready to write", so polling loops fall
// through immediately.  Written bytes are appended to _mockUartTx (until it
// fills) so a test can assert on the MIDI that went out; a test resets
// _mockUartTxLen to 0 to start a fresh capture.  The read side is empty unless
// a test queues MIDI-in bytes into _mockUartRx (set _mockUartRxLen, and
// _mockUartRxPos back to 0).
#include <stdint.h>
#include <stddef.h>

typedef struct uart_dev_s uart_dev_t;
static inline uart_dev_t* _mock_uart_ll_get_hw(int /*n*/) { return (uart_dev_t*)1; }
#define UART_LL_GET_HW(n) _mock_uart_ll_get_hw(n)

#define UART_INTR_RXFIFO_FULL  (1u << 0)
#define UART_INTR_RXFIFO_TOUT  (1u << 8)

static inline uint32_t uart_ll_get_txfifo_len(uart_dev_t*)                  { return 128; }
static uint8_t _mockUartTx[8192];
static size_t  _mockUartTxLen = 0;
static inline void     uart_ll_write_txfifo(uart_dev_t*, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n && _mockUartTxLen < sizeof(_mockUartTx); i++)
        _mockUartTx[_mockUartTxLen++] = b[i];
}
static uint8_t _mockUartRx[256];
static size_t  _mockUartRxLen = 0, _mockUartRxPos = 0;
static inline uint32_t uart_ll_get_rxfifo_len(uart_dev_t*) {
    return (uint32_t)(_mockUartRxLen - _mockUartRxPos);
}
static inline void     uart_ll_read_rxfifo(uart_dev_t*, uint8_t* b, size_t n) {
    for (size_t i = 0; i < n && _mockUartRxPos < _mockUartRxLen; i++)
        b[i] = _mockUartRx[_mockUartRxPos++];
}
static inline void     uart_ll_disable_intr_mask(uart_dev_t*, uint32_t)     {}
