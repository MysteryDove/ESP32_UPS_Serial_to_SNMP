#ifndef SPM2K_H_
#define SPM2K_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "uart_engine.h"

// SPM2K protocol lookup tables.
//
// This module provides request LUTs describing which UPS values are queried:
// - "constant" (initialized once and typically stable), and
// - "dynamic" (telemetry values updated continuously).
//
// Each LUT item fully defines command bytes, response mode and parser callback.

// Lookup table: initialized/constant values.
extern const uart_engine_request_t g_spm2k_constant_lut[];
extern const size_t g_spm2k_constant_lut_count;

// Lookup table: dynamic/telemetry values.
extern const uart_engine_request_t g_spm2k_dynamic_lut[];
extern const size_t g_spm2k_dynamic_lut_count;

// Heartbeat definition for SPM2K sub-adapter.
// Expected response must fully match g_spm2k_constant_heartbeat_expect_return.
extern const uart_engine_request_t g_spm2k_constant_heartbeat;
extern const uint8_t g_spm2k_constant_heartbeat_expect_return[];
extern const size_t g_spm2k_constant_heartbeat_expect_return_len;

bool spm2k_process_string(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_rated_info(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_manufacturer_date(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_voltage(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_frequency(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_percent_load(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_runtime_minutes_to_seconds(uint16_t cmd,
                                              const uint8_t *rx,
                                              uint16_t rx_len,
                                              void *out_value);
bool spm2k_process_temperature_c_to_kelvin(uint16_t cmd,
                                           const uint8_t *rx,
                                           uint16_t rx_len,
                                           void *out_value);
bool spm2k_process_remaining_capacity(uint16_t cmd,
                                      const uint8_t *rx,
                                      uint16_t rx_len,
                                      void *out_value);
bool spm2k_process_status_flags(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_ac_present(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_bat_current(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);
bool spm2k_process_ac_current(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value);

#ifdef __cplusplus
}
#endif

#endif // SPM2K_H_
