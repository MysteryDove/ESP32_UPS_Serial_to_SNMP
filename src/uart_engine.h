#ifndef UART_ENGINE_H_
#define UART_ENGINE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef UART_ENGINE_MAX_ENDING_LEN
#define UART_ENGINE_MAX_ENDING_LEN 8U
#endif

#ifndef UART_ENGINE_INTERJOB_COOLDOWN_MS
#define UART_ENGINE_INTERJOB_COOLDOWN_MS 1U
#endif

// Non-blocking UART request engine.
//
// - Enqueue requests (cmd 8/16-bit, expected response length) paired with a
//   process callback.
// - Call uart_engine_tick() frequently from the main loop.
// - Engine uses UART2_* adapter functions (DMA TX, ring-buffer RX).

typedef enum
{
    UART_ENGINE_OK = 0,
    UART_ENGINE_ERR_QUEUE_FULL,
    UART_ENGINE_ERR_BAD_PARAM,
    UART_ENGINE_ERR_DISABLED,
} uart_engine_result_t;

typedef bool (*uart_engine_process_fn)(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);

// Request struct. See uart_engine_enqueue().
// 
typedef struct
{
    void *out_value;
    uint16_t cmd;
    uint8_t cmd_bits;      // 8 or 16. For 16-bit, bytes are sent MSB then LSB.
    uint16_t expected_len; // fixed mode: exact bytes; ending mode: max bytes before fail
    bool expected_ending;  // false: fixed-length mode, true: stop once expected_ending_bytes is seen
    uint8_t expected_ending_len; // 1..UART_ENGINE_MAX_ENDING_LEN when expected_ending=true, length is in bytes
    uint8_t expected_ending_bytes[UART_ENGINE_MAX_ENDING_LEN]; // terminator sequence
    uint32_t timeout_ms;   // overall RX timeout
    uint8_t max_retries;   // max retries after a failure (engine will attempt 1 + max_retries total)

    uart_engine_process_fn process_fn;
} uart_engine_request_t;

void uart_engine_init(void);

// Enable/disable the engine at runtime.
//
// When disabled:
// - uart_engine_tick() becomes a no-op
// - queued/active jobs are dropped
// - heartbeat scheduling is stopped
// - UART lock is released (so other code won't deadlock)
void uart_engine_set_enabled(bool enable);
bool uart_engine_is_enabled(void);
bool uart_engine_is_busy(void);

// Call frequently (e.g., each main loop iteration).
void uart_engine_tick(void);

// Enqueue a request that will call process_fn(cmd, rx, rx_len, out_value).
// If process_fn returns true, the value is considered successfully updated.
// Note: process_fn should only write to out_value on success.
//
// cmd_bits must be 8 or 16.
// Command framing/suffix bytes (e.g., CRLF) should be handled by caller-side
// protocol code, not by this engine.
//
// RX modes:
// - expected_ending=false: fixed-length mode (wait until expected_len bytes).
// - expected_ending=true: terminator mode (wait until expected_ending_bytes;
//   expected_len becomes the maximum capture length).

uart_engine_result_t uart_engine_enqueue(const uart_engine_request_t *req);

// Convenience for common usage.
static inline uart_engine_result_t uart_engine_enqueue_value(void *out_value,
                                                            uint16_t cmd,
                                                            uint8_t cmd_bits,
                                                            uint16_t expected_len,
                                                            uint32_t timeout_ms,
                                                            uint8_t max_retries,
                                                            uart_engine_process_fn process_fn)
{
    uart_engine_request_t req = {
        .out_value = out_value,
        .cmd = cmd,
        .cmd_bits = cmd_bits,
        .expected_len = expected_len,
        .expected_ending = false,
        .expected_ending_len = 0U,
        .timeout_ms = timeout_ms,
        .max_retries = max_retries,
        .process_fn = process_fn,
    };
    return uart_engine_enqueue(&req);
}

// Heartbeat monitor.
//
// The heartbeat is scheduled periodically by the engine.
// If the heartbeat request fails (after its internal retries) consecutively
// failure_threshold times (default 5), battery fields are forced to 0.

typedef struct
{
    uart_engine_request_t req;
    uint32_t interval_ms;
    uint8_t failure_threshold;
} uart_engine_heartbeat_cfg_t;

// Enable heartbeat scheduling. Pass NULL to disable.
void uart_engine_set_heartbeat(const uart_engine_heartbeat_cfg_t *cfg);

// Helper process function: exact match against expected bytes.
// out_value should point to a uart_engine_expect_bytes_t.

typedef struct
{
    const uint8_t *expected;
    uint16_t expected_len;
} uart_engine_expect_bytes_t;

bool uart_engine_process_expect_exact(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);

#ifdef __cplusplus
}
#endif

#endif // UART_ENGINE_H_
