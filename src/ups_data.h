#ifndef UPS_DATA_H_
#define UPS_DATA_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Report IDs used by the UPS HID report descriptor.
enum
{
    REPORT_ID_POWER_SUMMARY = 1,
    REPORT_ID_INPUT = 2,
    REPORT_ID_OUTPUT = 3,
    REPORT_ID_BATTERY = 4,
};

typedef struct
{
    bool ac_present;
    bool charging;
    bool discharging;
    bool fully_charged;
    bool need_replacement;
    bool below_remaining_capacity_limit;
    bool battery_present;
    bool overload;
    bool shutdown_imminent;
} ups_present_status_t;

// Battery System Info
typedef struct
{
    uint16_t battery_voltage;
    int16_t battery_current;
    uint16_t config_voltage;
    uint16_t run_time_to_empty_s;
    uint16_t remaining_time_limit_s;
    uint16_t temperature;
    uint16_t manufacturer_date;
    uint8_t remaining_capacity;
} ups_battery_t;

// Power Summary Unique Features
typedef struct
{
    bool rechargeable;
    uint8_t capacity_mode;
    uint8_t design_capacity;
    uint8_t full_charge_capacity;
    uint8_t warning_capacity_limit;
    uint8_t remaining_capacity_limit;
    uint8_t i_device_chemistry;
    uint8_t capacity_granularity_1;
    uint8_t capacity_granularity_2;
    uint8_t i_manufacturer_2bit;
    uint8_t i_product_2bit;
    uint8_t i_serial_number_2bit;
    uint8_t i_name_2bit;
} ups_summary_t;

// Input System
typedef struct
{
    uint16_t voltage;
    uint16_t frequency;
    uint16_t config_voltage;
    uint16_t low_voltage_transfer;
    uint16_t high_voltage_transfer;
} ups_input_t;

// Output System
typedef struct
{
    uint8_t percent_load;
    uint16_t config_active_power;
    uint16_t config_voltage;
    uint16_t voltage;
    int16_t current;
    uint16_t frequency;
} ups_output_t;

// Global UPS state (defined in src/main.c)
extern ups_present_status_t g_power_summary_present_status;
extern ups_summary_t g_power_summary;
extern ups_battery_t g_battery;
extern ups_input_t g_input;
extern ups_output_t g_output;

#ifdef __cplusplus
}
#endif

#endif // UPS_DATA_H_
