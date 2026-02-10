#include "spm2k.h"

#include "ups_data.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SPM2K_CMD_LINE_TIMEOUT_MS 500U
#define SPM2K_CMD_LINE_RETRIES 0U
#define SPM2K_LINE_MAX_LEN 40U

static bool spm2k_rx_has_crlf(const uint8_t *rx, uint16_t rx_len);
static bool spm2k_extract_text(const uint8_t *rx,
                               uint16_t rx_len,
                               bool require_crlf,
                               char *out,
                               size_t out_size);
static bool spm2k_parse_scaled_int(const char *text,
                                   int32_t scale,
                                   int32_t min_value,
                                   int32_t max_value,
                                   int32_t *out_value);
static bool spm2k_parse_hex_byte(const char *text, uint8_t *out_value);
static bool spm2k_get_csv_field(const char *csv,
                                uint8_t field_index,
                                char *out,
                                size_t out_size);
static bool spm2k_pack_date_mmddyy(const char *text, uint16_t *out_value);

const uart_engine_request_t g_spm2k_constant_lut[] = {
    { .out_value = &g_power_summary.i_product_2bit, .cmd = (uint16_t)0x01U, .cmd_bits = 8U, .expected_len = SPM2K_LINE_MAX_LEN, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_string },
    { .out_value = &g_power_summary.i_serial_number_2bit, .cmd = (uint16_t)0x6EU, .cmd_bits = 8U, .expected_len = SPM2K_LINE_MAX_LEN, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_string },

    { .out_value = NULL, .cmd = (uint16_t)0x9FD1U, .cmd_bits = 16U, .expected_len = SPM2K_LINE_MAX_LEN, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_rated_info },

    { .out_value = &g_battery.manufacturer_date, .cmd = (uint16_t)0x78U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_manufacturer_date },

    { .out_value = &g_input.low_voltage_transfer, .cmd = (uint16_t)0x6CU, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_voltage },
    { .out_value = &g_input.high_voltage_transfer, .cmd = (uint16_t)0x75U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_voltage },
};

const size_t g_spm2k_constant_lut_count = sizeof(g_spm2k_constant_lut) / sizeof(g_spm2k_constant_lut[0]);

const uart_engine_request_t g_spm2k_dynamic_lut[] = {
    { .out_value = NULL, .cmd = (uint16_t)0x59U, .cmd_bits = 8U, .expected_len = 4U, .expected_ending = false, .expected_ending_len = 0U, .expected_ending_bytes = {0}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = NULL },
    { .out_value = &g_battery.battery_voltage, .cmd = (uint16_t)0x42U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_voltage },
    { .out_value = &g_battery.battery_current, .cmd = (uint16_t)0x9FD4U, .cmd_bits = 16U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_bat_current },
    { .out_value = &g_battery.run_time_to_empty_s, .cmd = (uint16_t)0x6AU, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_runtime_minutes_to_seconds },
    { .out_value = &g_battery.temperature, .cmd = (uint16_t)0x43U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_temperature_c_to_kelvin },
    { .out_value = &g_battery.remaining_capacity, .cmd = (uint16_t)0x66U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_remaining_capacity },

    { .out_value = &g_power_summary_present_status.ac_present, .cmd = (uint16_t)0x39U, .cmd_bits = 8U, .expected_len = 2U, .expected_ending = false, .expected_ending_len = 0U, .expected_ending_bytes = {0}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_ac_present },
    { .out_value = NULL, .cmd = (uint16_t)0x51U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_status_flags },

    { .out_value = &g_input.voltage, .cmd = (uint16_t)0x4CU, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_voltage },
    { .out_value = &g_input.frequency, .cmd = (uint16_t)0x9FD3U, .cmd_bits = 16U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_frequency },

    { .out_value = &g_output.percent_load, .cmd = (uint16_t)0x5CU, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_percent_load },
    { .out_value = &g_output.voltage, .cmd = (uint16_t)0x4FU, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_voltage },
    { .out_value = &g_output.current, .cmd = (uint16_t)0x2FU, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_ac_current },
    { .out_value = &g_output.frequency, .cmd = (uint16_t)0x46U, .cmd_bits = 8U, .expected_len = 16U, .expected_ending = true, .expected_ending_len = 2U, .expected_ending_bytes = {0x0DU, 0x0AU}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = spm2k_process_frequency },
};

const uart_engine_request_t g_spm2k_constant_heartbeat =
    { .out_value = NULL, .cmd = (uint16_t)0x59U, .cmd_bits = 8U, .expected_len = 4U, .expected_ending = false, .expected_ending_len = 0U, .expected_ending_bytes = {0}, .timeout_ms = SPM2K_CMD_LINE_TIMEOUT_MS, .max_retries = SPM2K_CMD_LINE_RETRIES, .process_fn = NULL };

const uint8_t g_spm2k_constant_heartbeat_expect_return[] = {0x53U, 0x4DU, 0x0DU, 0x0AU}; // "SM\r\n"
const size_t g_spm2k_constant_heartbeat_expect_return_len = sizeof(g_spm2k_constant_heartbeat_expect_return);

const size_t g_spm2k_dynamic_lut_count = sizeof(g_spm2k_dynamic_lut) / sizeof(g_spm2k_dynamic_lut[0]);

static bool spm2k_rx_has_crlf(const uint8_t *rx, uint16_t rx_len)
{
    if ((rx == NULL) || (rx_len < 2U))
    {
        return false;
    }

    return (rx[rx_len - 2U] == 0x0DU) && (rx[rx_len - 1U] == 0x0AU);
}

static bool spm2k_extract_text(const uint8_t *rx,
                               uint16_t rx_len,
                               bool require_crlf,
                               char *out,
                               size_t out_size)
{
    if ((rx == NULL) || (out == NULL) || (out_size < 2U) || (rx_len == 0U))
    {
        return false;
    }

    size_t payload_len = (size_t)rx_len;
    if (require_crlf)
    {
        if (!spm2k_rx_has_crlf(rx, rx_len))
        {
            return false;
        }
        payload_len -= 2U;
    }

    if ((payload_len == 0U) || (payload_len >= out_size))
    {
        return false;
    }

    for (size_t i = 0U; i < payload_len; ++i)
    {
        if (!isprint((int)rx[i]))
        {
            return false;
        }
    }

    memcpy(out, rx, payload_len);
    out[payload_len] = '\0';
    return true;
}

static bool spm2k_parse_scaled_int(const char *text,
                                   int32_t scale,
                                   int32_t min_value,
                                   int32_t max_value,
                                   int32_t *out_value)
{
    if ((text == NULL) || (out_value == NULL) || (scale <= 0))
    {
        return false;
    }

    int32_t fraction_digits = 0;
    int32_t tmp_scale = scale;
    while ((tmp_scale > 1) && ((tmp_scale % 10) == 0))
    {
        tmp_scale /= 10;
        fraction_digits++;
    }
    if (tmp_scale != 1)
    {
        return false;
    }

    char const *cursor = text;
    int sign = 1;
    if (*cursor == '-')
    {
        sign = -1;
        cursor++;
    }
    else if (*cursor == '+')
    {
        cursor++;
    }

    if (!isdigit((int)*cursor))
    {
        return false;
    }

    int64_t integral = 0;
    while (isdigit((int)*cursor))
    {
        integral = (integral * 10) + (*cursor - '0');
        if (integral > (INT32_MAX / scale))
        {
            return false;
        }
        cursor++;
    }

    int64_t fraction = 0;
    int32_t captured_fraction_digits = 0;
    if (*cursor == '.')
    {
        cursor++;
        if (!isdigit((int)*cursor))
        {
            return false;
        }

        while (isdigit((int)*cursor))
        {
            if (captured_fraction_digits < fraction_digits)
            {
                fraction = (fraction * 10) + (*cursor - '0');
                captured_fraction_digits++;
            }
            cursor++;
        }
    }

    while (captured_fraction_digits < fraction_digits)
    {
        fraction *= 10;
        captured_fraction_digits++;
    }

    if (*cursor != '\0')
    {
        return false;
    }

    int64_t scaled = (integral * scale) + fraction;
    if (sign < 0)
    {
        scaled = -scaled;
    }

    if ((scaled < min_value) || (scaled > max_value))
    {
        return false;
    }

    *out_value = (int32_t)scaled;
    return true;
}

static bool spm2k_parse_hex_byte(const char *text, uint8_t *out_value)
{
    if ((text == NULL) || (out_value == NULL))
    {
        return false;
    }

    if ((text[0] == '\0') || (text[1] == '\0') || (text[2] != '\0'))
    {
        return false;
    }

    int hi = isdigit((int)text[0]) ? (text[0] - '0') : (tolower((int)text[0]) - 'a' + 10);
    int lo = isdigit((int)text[1]) ? (text[1] - '0') : (tolower((int)text[1]) - 'a' + 10);

    if ((hi < 0) || (hi > 15) || (lo < 0) || (lo > 15))
    {
        return false;
    }

    *out_value = (uint8_t)((hi << 4) | lo);
    return true;
}

static bool spm2k_get_csv_field(const char *csv,
                                uint8_t field_index,
                                char *out,
                                size_t out_size)
{
    if ((csv == NULL) || (out == NULL) || (out_size < 2U))
    {
        return false;
    }

    uint8_t current_field = 0U;
    char const *field_start = csv;

    for (char const *cursor = csv;; ++cursor)
    {
        if ((*cursor == ',') || (*cursor == '\0'))
        {
            if (current_field == field_index)
            {
                size_t len = (size_t)(cursor - field_start);
                if ((len == 0U) || (len >= out_size))
                {
                    return false;
                }

                memcpy(out, field_start, len);
                out[len] = '\0';
                return true;
            }

            if (*cursor == '\0')
            {
                return false;
            }

            current_field++;
            field_start = cursor + 1;
        }
    }
}

static bool spm2k_pack_date_mmddyy(const char *text, uint16_t *out_value)
{
    if ((text == NULL) || (out_value == NULL))
    {
        return false;
    }

    if ((strlen(text) != 8U) ||
        (text[2] != '/') ||
        (text[5] != '/') ||
        !isdigit((int)text[0]) ||
        !isdigit((int)text[1]) ||
        !isdigit((int)text[3]) ||
        !isdigit((int)text[4]) ||
        !isdigit((int)text[6]) ||
        !isdigit((int)text[7]))
    {
        return false;
    }

    uint8_t const mm = (uint8_t)(((uint8_t)(text[0] - '0') * 10U) + (uint8_t)(text[1] - '0'));
    uint8_t const dd = (uint8_t)(((uint8_t)(text[3] - '0') * 10U) + (uint8_t)(text[4] - '0'));
    uint8_t const yy = (uint8_t)(((uint8_t)(text[6] - '0') * 10U) + (uint8_t)(text[7] - '0'));

    if ((mm < 1U) || (mm > 12U) || (dd < 1U) || (dd > 31U))
    {
        return false;
    }

    uint16_t const year = (uint16_t)(2000U + yy);
    if ((year < 1980U) || (year > 2107U))
    {
        return false;
    }

    uint16_t const yoff = (uint16_t)(year - 1980U);
    *out_value = (uint16_t)((yoff << 9) | ((uint16_t)mm << 5) | (uint16_t)dd);
    return true;
}

bool spm2k_process_string(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)out_value;

    char text[33];
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    switch (cmd)
    {
    case 0x01U:
    case 0x6EU:
        return true;
    default:
        return false;
    }
}

bool spm2k_process_rated_info(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;
    (void)out_value;

    char text[48];
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    char token[16];
    int32_t parsed_config_active_power = 0;
    int32_t parsed_input_config_voltage = 0;
    int32_t parsed_output_config_voltage = 0;
    int32_t parsed_battery_config_voltage = 0;

    if (!spm2k_get_csv_field(text, 0U, token, sizeof(token)) ||
        !spm2k_parse_scaled_int(token, 1, 0, UINT16_MAX, &parsed_config_active_power))
    {
        return false;
    }

    if (!spm2k_get_csv_field(text, 1U, token, sizeof(token)) ||
        !spm2k_parse_scaled_int(token, 100, 0, UINT16_MAX, &parsed_input_config_voltage))
    {
        return false;
    }

    if (!spm2k_get_csv_field(text, 2U, token, sizeof(token)) ||
        !spm2k_parse_scaled_int(token, 100, 0, UINT16_MAX, &parsed_output_config_voltage))
    {
        return false;
    }

    if (!spm2k_get_csv_field(text, 5U, token, sizeof(token)) ||
        !spm2k_parse_scaled_int(token, 100, 0, UINT16_MAX, &parsed_battery_config_voltage))
    {
        return false;
    }

    g_output.config_active_power = (uint16_t)parsed_config_active_power;
    g_input.config_voltage = (uint16_t)parsed_input_config_voltage;
    g_output.config_voltage = (uint16_t)parsed_output_config_voltage;
    g_battery.config_voltage = (uint16_t)parsed_battery_config_voltage;

    return true;
}

bool spm2k_process_manufacturer_date(uint16_t cmd,
                                     const uint8_t *rx,
                                     uint16_t rx_len,
                                     void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    uint16_t packed_date = 0U;
    if (!spm2k_pack_date_mmddyy(text, &packed_date))
    {
        return false;
    }

    *(uint16_t *)out_value = packed_date;
    return true;
}

bool spm2k_process_voltage(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t parsed = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    if (!spm2k_parse_scaled_int(text, 100, 0, UINT16_MAX, &parsed))
    {
        return false;
    }

    *(uint16_t *)out_value = (uint16_t)parsed;
    return true;
}

bool spm2k_process_frequency(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t parsed = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    if (!spm2k_parse_scaled_int(text, 100, 0, UINT16_MAX, &parsed))
    {
        return false;
    }

    *(uint16_t *)out_value = (uint16_t)parsed;
    return true;
}

bool spm2k_process_percent_load(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t parsed_x100 = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)) ||
        !spm2k_parse_scaled_int(text, 100, 0, 10000, &parsed_x100))
    {
        return false;
    }

    uint8_t percent = (uint8_t)(parsed_x100 / 100);
    *(uint8_t *)out_value = percent;
    return true;
}

bool spm2k_process_runtime_minutes_to_seconds(uint16_t cmd,
                                              const uint8_t *rx,
                                              uint16_t rx_len,
                                              void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    char *colon = strchr(text, ':');
    if (colon == NULL)
    {
        return false;
    }
    *colon = '\0';

    int32_t minutes = 0;
    if (!spm2k_parse_scaled_int(text, 1, 0, (INT32_MAX / 60), &minutes))
    {
        return false;
    }

    int32_t seconds = minutes * 60;
    if (seconds > UINT16_MAX)
    {
        seconds = UINT16_MAX;
    }

    *(uint16_t *)out_value = (uint16_t)seconds;
    return true;
}

bool spm2k_process_temperature_c_to_kelvin(uint16_t cmd,
                                           const uint8_t *rx,
                                           uint16_t rx_len,
                                           void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t celsius_x10 = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)) ||
        !spm2k_parse_scaled_int(text, 10, -2731, 5000, &celsius_x10))
    {
        return false;
    }

    int32_t kelvin_x10 = celsius_x10 + 2731;
    if (kelvin_x10 < 0)
    {
        kelvin_x10 = 0;
    }
    if (kelvin_x10 > UINT16_MAX)
    {
        kelvin_x10 = UINT16_MAX;
    }

    *(uint16_t *)out_value = (uint16_t)kelvin_x10;
    return true;
}

bool spm2k_process_remaining_capacity(uint16_t cmd,
                                      const uint8_t *rx,
                                      uint16_t rx_len,
                                      void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t capacity_x10 = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)) ||
        !spm2k_parse_scaled_int(text, 10, 0, 1000, &capacity_x10))
    {
        return false;
    }

    uint8_t const capacity_percent = (uint8_t)(capacity_x10 / 10);
    *(uint8_t *)out_value = capacity_percent;

    g_power_summary_present_status.fully_charged = (capacity_percent >= 100U);
    return true;
}

bool spm2k_process_status_flags(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;
    (void)out_value;

    char text[8];
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    uint8_t flags = 0U;
    if (!spm2k_parse_hex_byte(text, &flags))
    {
        return false;
    }

    bool const on_line = ((flags & (1U << 3)) != 0U);
    bool const on_battery = ((flags & (1U << 4)) != 0U);
    bool const overload = ((flags & (1U << 5)) != 0U);
    bool const battery_low = ((flags & (1U << 6)) != 0U);
    bool const replace_battery = ((flags & (1U << 7)) != 0U);

    g_power_summary_present_status.ac_present = on_line && !on_battery;
    g_power_summary_present_status.charging = on_line && !on_battery && (g_battery.remaining_capacity < 100U);
    g_power_summary_present_status.discharging = on_battery;
    g_power_summary_present_status.overload = overload;
    g_power_summary_present_status.below_remaining_capacity_limit = battery_low;
    g_power_summary_present_status.shutdown_imminent = battery_low;
    g_power_summary_present_status.need_replacement = replace_battery;
    g_power_summary_present_status.battery_present = true;

    return true;
}

bool spm2k_process_ac_present(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    if ((out_value == NULL) || (rx == NULL) || (rx_len != 2U))
    {
        return false;
    }

    char text[3] = {(char)rx[0], (char)rx[1], '\0'};
    bool is_ff = ((text[0] == 'F') || (text[0] == 'f')) && ((text[1] == 'F') || (text[1] == 'f'));
    bool is_00 = (text[0] == '0') && (text[1] == '0');

    if (!is_ff && !is_00)
    {
        return false;
    }

    *(bool *)out_value = is_ff;
    return true;
}

bool spm2k_process_bat_current(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t parsed = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)))
    {
        return false;
    }

    if (!spm2k_parse_scaled_int(text, 100, INT16_MIN, INT16_MAX, &parsed))
    {
        return false;
    }

    *(int16_t *)out_value = (int16_t)parsed;

    if (parsed < 0)
    {
        g_power_summary_present_status.charging = false;
        g_power_summary_present_status.discharging = true;
    }
    else if (parsed > 0)
    {
        g_power_summary_present_status.charging = true;
        g_power_summary_present_status.discharging = false;
    }

    return true;
}

bool spm2k_process_ac_current(uint16_t cmd, const uint8_t *rx, uint16_t rx_len, void *out_value)
{
    (void)cmd;

    if (out_value == NULL)
    {
        return false;
    }

    char text[16];
    int32_t parsed = 0;
    if (!spm2k_extract_text(rx, rx_len, true, text, sizeof(text)) ||
        !spm2k_parse_scaled_int(text, 100, INT16_MIN, INT16_MAX, &parsed))
    {
        return false;
    }

    *(int16_t *)out_value = (int16_t)parsed;
    return true;
}
