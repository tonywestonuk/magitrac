#pragma once
// Test-only stub for hal/uart_ll.h.  Lets midi_player.cpp compile under
// UNIT_TEST — the sequencer logic doesn't care whether bytes actually leave
// the (mock) FIFO.  All functions are no-ops; FIFO always reports "ready to
// write" and "empty to read" so polling loops fall through immediately.
#include <stdint.h>
#include <stddef.h>

typedef struct uart_dev_s uart_dev_t;
static inline uart_dev_t* _mock_uart_ll_get_hw(int /*n*/) { return (uart_dev_t*)1; }
#define UART_LL_GET_HW(n) _mock_uart_ll_get_hw(n)

#define UART_INTR_RXFIFO_FULL  (1u << 0)
#define UART_INTR_RXFIFO_TOUT  (1u << 8)

static inline uint32_t uart_ll_get_txfifo_len(uart_dev_t*)                  { return 128; }
static inline void     uart_ll_write_txfifo(uart_dev_t*, const uint8_t*, size_t) {}
static inline uint32_t uart_ll_get_rxfifo_len(uart_dev_t*)                  { return 0; }
static inline void     uart_ll_read_rxfifo(uart_dev_t*, uint8_t*, size_t)   {}
static inline void     uart_ll_disable_intr_mask(uart_dev_t*, uint32_t)     {}
