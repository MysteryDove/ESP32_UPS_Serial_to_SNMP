#ifndef MAIN_H_
#define MAIN_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/uart.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ups_data.h"

#ifndef UPS_UART_PORT
#define UPS_UART_PORT UART_NUM_1
#endif

#ifndef UPS_UART_BAUDRATE
#define UPS_UART_BAUDRATE 2400
#endif

#ifndef UPS_UART_TX_GPIO
#define UPS_UART_TX_GPIO 0
#endif

#ifndef UPS_UART_RX_GPIO
#define UPS_UART_RX_GPIO 1
#endif

#ifndef UPS_UART_BUFFER_SIZE
#define UPS_UART_BUFFER_SIZE 512
#endif

#ifndef UPS_UART_TX_INVERT
#define UPS_UART_TX_INVERT 0
#endif

#ifndef UPS_UART_RX_INVERT
#define UPS_UART_RX_INVERT 0
#endif

uint32_t ups_tick_ms(void);

void UART2_RxStartIT(void);
esp_err_t UART2_SendBytes(const uint8_t *data, uint16_t len, uint32_t timeout_ms);
// Async TX using ESP-IDF UART driver TX ring-buffer + ISR path.
esp_err_t UART2_SendBytesDMA(const uint8_t *data, uint16_t len);
bool UART2_TxDone(void);
void UART2_TxDoneClear(void);
uint16_t UART2_Available(void);
int UART2_ReadByte(uint8_t *out);
uint16_t UART2_Read(uint8_t *dst, uint16_t len);
void UART2_DiscardBuffered(void);
bool UART2_ReadExactTimeout(uint8_t *dst, uint16_t len, uint32_t timeout_ms);
bool UART2_TryLock(void);
void UART2_Unlock(void);

extern const bool g_ups_debug_status_print_enabled;
void UPS_DebugPrintTxCommand(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif // MAIN_H_
