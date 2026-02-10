#include "main.h"

#include "spm2k.h"
#include "snmp_agent.h"
#include "uart_engine.h"
#include "wifi_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "ups_main";

static inline void ups_loop_delay_safe(uint32_t delay_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    if (ticks == 0)
    {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

#ifndef UPS_DYNAMIC_UPDATE_PERIOD_S
#define UPS_DYNAMIC_UPDATE_PERIOD_S 10U
#endif

#ifndef UPS_INIT_RETRY_PERIOD_S
#define UPS_INIT_RETRY_PERIOD_S 5U
#endif

#ifndef UPS_DEBUG_STATUS_PRINT_ENABLED
#define UPS_DEBUG_STATUS_PRINT_ENABLED 1
#endif

#ifndef UPS_DEBUG_STATUS_PRINT_PERIOD_MS
#define UPS_DEBUG_STATUS_PRINT_PERIOD_MS 10000U
#endif

#ifndef UPS_BOOTSTRAP_HEARTBEAT_RX_BUF_SIZE
#define UPS_BOOTSTRAP_HEARTBEAT_RX_BUF_SIZE 16U
#endif

#ifndef UPS_MAIN_LOOP_DELAY_MS
#define UPS_MAIN_LOOP_DELAY_MS 1U
#endif

#ifndef UPS_ENQUEUE_BURST_PER_TICK
#define UPS_ENQUEUE_BURST_PER_TICK 8U
#endif

#define UPS_DYNAMIC_UPDATE_PERIOD_MS ((uint32_t)(UPS_DYNAMIC_UPDATE_PERIOD_S) * 1000U)
#define UPS_INIT_RETRY_PERIOD_MS ((uint32_t)(UPS_INIT_RETRY_PERIOD_S) * 1000U)

#if (UPS_DEBUG_STATUS_PRINT_ENABLED != 0)
#define UPS_DEBUG_PRINTF(...) printf(__VA_ARGS__)
const bool g_ups_debug_status_print_enabled = true;
#else
#define UPS_DEBUG_PRINTF(...)
const bool g_ups_debug_status_print_enabled = false;
#endif

typedef enum
{
    UPS_SUB_ADAPTER_SPM2K = 0,
} ups_sub_adapter_t;

#ifndef UPS_ACTIVE_SUB_ADAPTER
#define UPS_ACTIVE_SUB_ADAPTER UPS_SUB_ADAPTER_SPM2K
#endif

typedef enum
{
    UPS_BOOTSTRAP_ENQUEUE_HEARTBEAT = 0,
    UPS_BOOTSTRAP_WAIT_HEARTBEAT_DRAIN,
    UPS_BOOTSTRAP_HEARTBEAT_VERIFY,
    UPS_BOOTSTRAP_WAIT_RETRY,
    UPS_BOOTSTRAP_ENQUEUE_CONSTANT,
    UPS_BOOTSTRAP_ENQUEUE_DYNAMIC,
    UPS_BOOTSTRAP_WAIT_DRAIN,
    UPS_BOOTSTRAP_SANITY_CHECK,
    UPS_BOOTSTRAP_DONE,
} ups_bootstrap_state_t;

static const uart_engine_request_t *g_sub_adapter_constant_lut = NULL;
static size_t g_sub_adapter_constant_lut_count = 0U;
static const uart_engine_request_t *g_sub_adapter_dynamic_lut = NULL;
static size_t g_sub_adapter_dynamic_lut_count = 0U;
static const uart_engine_request_t *g_sub_adapter_constant_heartbeat = NULL;
static const uint8_t *g_sub_adapter_constant_heartbeat_expect_return = NULL;
static size_t g_sub_adapter_constant_heartbeat_expect_return_len = 0U;

static ups_bootstrap_state_t s_ups_bootstrap_state = UPS_BOOTSTRAP_ENQUEUE_HEARTBEAT;
static size_t s_bootstrap_constant_idx = 0U;
static size_t s_bootstrap_dynamic_idx = 0U;
static uint32_t s_init_retry_not_before_ms = 0U;
static uint32_t s_init_bootstrap_start_ms = 0U;
static bool s_init_bootstrap_started = false;
static uint32_t s_last_dynamic_cycle_start_ms = 0U;

static uint8_t s_bootstrap_heartbeat_rx[UPS_BOOTSTRAP_HEARTBEAT_RX_BUF_SIZE];
static uint16_t s_bootstrap_heartbeat_rx_len = 0U;
static bool s_bootstrap_heartbeat_done = false;

static bool s_dynamic_update_cycle_active = false;
static size_t s_dynamic_update_idx = 0U;
static uint32_t s_next_dynamic_update_ms = 0U;

#ifndef UART_ENGINE_DEFAULT_ENABLED
#define UART_ENGINE_DEFAULT_ENABLED 1
#endif

static bool s_uart_engine_enabled = (UART_ENGINE_DEFAULT_ENABLED != 0);

ups_present_status_t g_power_summary_present_status = {
    .ac_present = false,
    .charging = false,
    .discharging = false,
    .fully_charged = false,
    .need_replacement = false,
    .below_remaining_capacity_limit = false,
    .battery_present = false,
    .overload = false,
    .shutdown_imminent = false,
};

ups_summary_t g_power_summary = {
    .rechargeable = true,
    .capacity_mode = 2U,
    .design_capacity = 100U,
    .full_charge_capacity = 100U,
    .warning_capacity_limit = 20U,
    .remaining_capacity_limit = 10U,
    .i_device_chemistry = 0x05U,
    .capacity_granularity_1 = 1U,
    .capacity_granularity_2 = 1U,
    .i_manufacturer_2bit = 1U,
    .i_product_2bit = 2U,
    .i_serial_number_2bit = 3U,
    .i_name_2bit = 2U,
};

ups_battery_t g_battery = {
    .battery_voltage = 0,
    .battery_current = 0,
    .config_voltage = 0,
    .run_time_to_empty_s = 0,
    .remaining_time_limit_s = 0,
    .temperature = 0,
    .manufacturer_date = 0,
    .remaining_capacity = 0,
};

ups_input_t g_input = {
    .voltage = 0,
    .frequency = 0,
    .config_voltage = 0,
    .low_voltage_transfer = 0,
    .high_voltage_transfer = 0,
};

ups_output_t g_output = {
    .percent_load = 0,
    .config_active_power = 0,
    .config_voltage = 0,
    .voltage = 0,
    .current = 0,
    .frequency = 0,
};

void UPS_DebugPrintTxCommand(const uint8_t *data, uint16_t len)
{
#if (UPS_DEBUG_STATUS_PRINT_ENABLED != 0)
    if ((data == NULL) || (len < 1U))
    {
        return;
    }

    printf("UART_TX cmd len=%u data=", (unsigned int)len);
    for (uint16_t i = 0U; i < len; i++)
    {
        printf("%02X", data[i]);
        if ((uint16_t)(i + 1U) < len)
        {
            printf(" ");
        }
    }
    printf("\r\n");
#else
    (void)data;
    (void)len;
#endif
}

static void ups_sub_adapter_select(void)
{
    switch ((ups_sub_adapter_t)UPS_ACTIVE_SUB_ADAPTER)
    {
    case UPS_SUB_ADAPTER_SPM2K:
        g_sub_adapter_constant_lut = g_spm2k_constant_lut;
        g_sub_adapter_constant_lut_count = g_spm2k_constant_lut_count;
        g_sub_adapter_dynamic_lut = g_spm2k_dynamic_lut;
        g_sub_adapter_dynamic_lut_count = g_spm2k_dynamic_lut_count;
        g_sub_adapter_constant_heartbeat = &g_spm2k_constant_heartbeat;
        g_sub_adapter_constant_heartbeat_expect_return = g_spm2k_constant_heartbeat_expect_return;
        g_sub_adapter_constant_heartbeat_expect_return_len = g_spm2k_constant_heartbeat_expect_return_len;
        break;
    default:
        g_sub_adapter_constant_lut = NULL;
        g_sub_adapter_constant_lut_count = 0U;
        g_sub_adapter_dynamic_lut = NULL;
        g_sub_adapter_dynamic_lut_count = 0U;
        g_sub_adapter_constant_heartbeat = NULL;
        g_sub_adapter_constant_heartbeat_expect_return = NULL;
        g_sub_adapter_constant_heartbeat_expect_return_len = 0U;
        break;
    }
}

static bool ups_bootstrap_heartbeat_capture(uint16_t cmd,
                                            const uint8_t *rx,
                                            uint16_t rx_len,
                                            void *out_value)
{
    (void)cmd;
    (void)out_value;

    s_bootstrap_heartbeat_done = false;
    s_bootstrap_heartbeat_rx_len = 0U;

    if (rx == NULL)
    {
        return false;
    }

    if (rx_len > (uint16_t)sizeof(s_bootstrap_heartbeat_rx))
    {
        return false;
    }

    memcpy(s_bootstrap_heartbeat_rx, rx, rx_len);
    s_bootstrap_heartbeat_rx_len = rx_len;
    s_bootstrap_heartbeat_done = true;
    return true;
}

static bool ups_bootstrap_heartbeat_matches_expected(void)
{
    if (!s_bootstrap_heartbeat_done ||
        (g_sub_adapter_constant_heartbeat_expect_return == NULL) ||
        (g_sub_adapter_constant_heartbeat_expect_return_len == 0U))
    {
        return false;
    }

    if (s_bootstrap_heartbeat_rx_len != g_sub_adapter_constant_heartbeat_expect_return_len)
    {
        return false;
    }

    return (memcmp(s_bootstrap_heartbeat_rx,
                   g_sub_adapter_constant_heartbeat_expect_return,
                   s_bootstrap_heartbeat_rx_len) == 0);
}

static void ups_bootstrap_reset_for_retry(uint32_t now_ms)
{
    s_bootstrap_constant_idx = 0U;
    s_bootstrap_dynamic_idx = 0U;
    s_bootstrap_heartbeat_rx_len = 0U;
    s_bootstrap_heartbeat_done = false;
    s_init_retry_not_before_ms = now_ms + UPS_INIT_RETRY_PERIOD_MS;
    s_ups_bootstrap_state = UPS_BOOTSTRAP_WAIT_RETRY;
}

static void ups_enqueue_full_lut_step(const uart_engine_request_t *lut,
                                      size_t lut_count,
                                      size_t *inout_index)
{
    if ((lut == NULL) || (inout_index == NULL))
    {
        return;
    }

    if (*inout_index >= lut_count)
    {
        return;
    }

    uint8_t burst = 0U;
    while ((*inout_index < lut_count) && (burst < UPS_ENQUEUE_BURST_PER_TICK))
    {
        uart_engine_result_t const result = uart_engine_enqueue(&lut[*inout_index]);
        if (result != UART_ENGINE_OK)
        {
            break;
        }

        (*inout_index)++;
        burst++;
    }
}

static void ups_bootstrap_task(void)
{
    uint32_t const now_ms = ups_tick_ms();

    if (!s_init_bootstrap_started)
    {
        s_init_bootstrap_started = true;
        s_init_bootstrap_start_ms = now_ms;
    }

    switch (s_ups_bootstrap_state)
    {
    case UPS_BOOTSTRAP_ENQUEUE_HEARTBEAT:
    {
        if (g_sub_adapter_constant_heartbeat == NULL)
        {
            ups_bootstrap_reset_for_retry(now_ms);
            break;
        }

        uart_engine_request_t hb_req = *g_sub_adapter_constant_heartbeat;
        hb_req.out_value = NULL;
        hb_req.process_fn = ups_bootstrap_heartbeat_capture;

        uart_engine_result_t const result = uart_engine_enqueue(&hb_req);
        if (result == UART_ENGINE_OK)
        {
            s_bootstrap_heartbeat_done = false;
            s_ups_bootstrap_state = UPS_BOOTSTRAP_WAIT_HEARTBEAT_DRAIN;
        }
        break;
    }

    case UPS_BOOTSTRAP_WAIT_HEARTBEAT_DRAIN:
        if (!uart_engine_is_busy())
        {
            s_ups_bootstrap_state = UPS_BOOTSTRAP_HEARTBEAT_VERIFY;
        }
        break;

    case UPS_BOOTSTRAP_HEARTBEAT_VERIFY:
        if (ups_bootstrap_heartbeat_matches_expected())
        {
            s_ups_bootstrap_state = UPS_BOOTSTRAP_ENQUEUE_CONSTANT;
        }
        else
        {
            UPS_DEBUG_PRINTF("INIT heartbeat failed, retry in %lu ms\r\n",
                             (unsigned long)UPS_INIT_RETRY_PERIOD_MS);
            ups_bootstrap_reset_for_retry(now_ms);
        }
        break;

    case UPS_BOOTSTRAP_WAIT_RETRY:
        if ((int32_t)(now_ms - s_init_retry_not_before_ms) >= 0)
        {
            s_ups_bootstrap_state = UPS_BOOTSTRAP_ENQUEUE_HEARTBEAT;
        }
        break;

    case UPS_BOOTSTRAP_ENQUEUE_CONSTANT:
        ups_enqueue_full_lut_step(g_sub_adapter_constant_lut,
                                  g_sub_adapter_constant_lut_count,
                                  &s_bootstrap_constant_idx);
        if (s_bootstrap_constant_idx >= g_sub_adapter_constant_lut_count)
        {
            s_ups_bootstrap_state = UPS_BOOTSTRAP_ENQUEUE_DYNAMIC;
        }
        break;

    case UPS_BOOTSTRAP_ENQUEUE_DYNAMIC:
        ups_enqueue_full_lut_step(g_sub_adapter_dynamic_lut,
                                  g_sub_adapter_dynamic_lut_count,
                                  &s_bootstrap_dynamic_idx);
        if (s_bootstrap_dynamic_idx >= g_sub_adapter_dynamic_lut_count)
        {
            s_ups_bootstrap_state = UPS_BOOTSTRAP_WAIT_DRAIN;
        }
        break;

    case UPS_BOOTSTRAP_WAIT_DRAIN:
        if (!uart_engine_is_busy())
        {
            s_ups_bootstrap_state = UPS_BOOTSTRAP_SANITY_CHECK;
        }
        break;

    case UPS_BOOTSTRAP_SANITY_CHECK:
        if (g_battery.remaining_capacity > 0U)
        {
            s_next_dynamic_update_ms = ups_tick_ms() + UPS_DYNAMIC_UPDATE_PERIOD_MS;
            s_ups_bootstrap_state = UPS_BOOTSTRAP_DONE;
            UPS_DEBUG_PRINTF("INIT full bootstrap done in %lu ms\r\n",
                             (unsigned long)(now_ms - s_init_bootstrap_start_ms));
        }
        else
        {
            UPS_DEBUG_PRINTF("INIT sanity failed (remaining_capacity=0), retry in %lu ms\r\n",
                             (unsigned long)UPS_INIT_RETRY_PERIOD_MS);
            ups_bootstrap_reset_for_retry(now_ms);
        }
        break;

    case UPS_BOOTSTRAP_DONE:
    default:
        break;
    }
}

static void ups_dynamic_update_task(void)
{
    if (s_ups_bootstrap_state != UPS_BOOTSTRAP_DONE)
    {
        return;
    }

    uint32_t const now_ms = ups_tick_ms();
    if (!s_dynamic_update_cycle_active)
    {
        if ((int32_t)(now_ms - s_next_dynamic_update_ms) < 0)
        {
            return;
        }

        s_dynamic_update_cycle_active = true;
        s_dynamic_update_idx = 0U;
        s_last_dynamic_cycle_start_ms = now_ms;
    }

    if (s_dynamic_update_idx < g_sub_adapter_dynamic_lut_count)
    {
        ups_enqueue_full_lut_step(g_sub_adapter_dynamic_lut,
                                  g_sub_adapter_dynamic_lut_count,
                                  &s_dynamic_update_idx);
        return;
    }

    if (uart_engine_is_busy())
    {
        return;
    }

    s_dynamic_update_cycle_active = false;
    s_next_dynamic_update_ms = now_ms + UPS_DYNAMIC_UPDATE_PERIOD_MS;
    UPS_DEBUG_PRINTF("DYN refresh done in %lu ms\r\n",
                     (unsigned long)(now_ms - s_last_dynamic_cycle_start_ms));
}

static void ups_debug_status_print_task(void)
{
#if (UPS_DEBUG_STATUS_PRINT_ENABLED != 0)
    static uint32_t next_print_ms = 0U;
    uint32_t const now_ms = ups_tick_ms();
    if ((int32_t)(now_ms - next_print_ms) < 0)
    {
        return;
    }

    next_print_ms = now_ms + UPS_DEBUG_STATUS_PRINT_PERIOD_MS;

    printf("PS: ac=%u chg=%u dis=%u full=%u repl=%u low=%u bpres=%u ovl=%u shut=%u\r\n",
           (unsigned)g_power_summary_present_status.ac_present,
           (unsigned)g_power_summary_present_status.charging,
           (unsigned)g_power_summary_present_status.discharging,
           (unsigned)g_power_summary_present_status.fully_charged,
           (unsigned)g_power_summary_present_status.need_replacement,
           (unsigned)g_power_summary_present_status.below_remaining_capacity_limit,
           (unsigned)g_power_summary_present_status.battery_present,
           (unsigned)g_power_summary_present_status.overload,
           (unsigned)g_power_summary_present_status.shutdown_imminent);

    printf("SUM: rech=%u mode=%u des=%u full=%u warn=%u rem=%u chem=%u g1=%u g2=%u iM=%u iP=%u iS=%u iN=%u\r\n",
           (unsigned)g_power_summary.rechargeable,
           (unsigned)g_power_summary.capacity_mode,
           (unsigned)g_power_summary.design_capacity,
           (unsigned)g_power_summary.full_charge_capacity,
           (unsigned)g_power_summary.warning_capacity_limit,
           (unsigned)g_power_summary.remaining_capacity_limit,
           (unsigned)g_power_summary.i_device_chemistry,
           (unsigned)g_power_summary.capacity_granularity_1,
           (unsigned)g_power_summary.capacity_granularity_2,
           (unsigned)g_power_summary.i_manufacturer_2bit,
           (unsigned)g_power_summary.i_product_2bit,
           (unsigned)g_power_summary.i_serial_number_2bit,
           (unsigned)g_power_summary.i_name_2bit);

    printf("BAT: cap=%u rt=%u rtl=%u vb=%u ib=%d cfgv=%u temp=%u mfg=%u\r\n",
           (unsigned)g_battery.remaining_capacity,
           (unsigned)g_battery.run_time_to_empty_s,
           (unsigned)g_battery.remaining_time_limit_s,
           (unsigned)g_battery.battery_voltage,
           (int)g_battery.battery_current,
           (unsigned)g_battery.config_voltage,
           (unsigned)g_battery.temperature,
           (unsigned)g_battery.manufacturer_date);

    printf("IN: v=%u f=%u cfgv=%u low=%u high=%u\r\n",
           (unsigned)g_input.voltage,
           (unsigned)g_input.frequency,
           (unsigned)g_input.config_voltage,
           (unsigned)g_input.low_voltage_transfer,
           (unsigned)g_input.high_voltage_transfer);

    printf("OUT: load=%u cfgp=%u cfgv=%u v=%u i=%d f=%u\r\n",
           (unsigned)g_output.percent_load,
           (unsigned)g_output.config_active_power,
           (unsigned)g_output.config_voltage,
           (unsigned)g_output.voltage,
           (int)g_output.current,
           (unsigned)g_output.frequency);
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "Starting UPS UART bridge: UART%u tx=%d rx=%d baud=%d",
             (unsigned int)UPS_UART_PORT,
             UPS_UART_TX_GPIO,
             UPS_UART_RX_GPIO,
             UPS_UART_BAUDRATE);

    esp_err_t const wifi_err = wifi_client_start();
    if (wifi_err != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi start failed (%s), SNMP agent disabled", esp_err_to_name(wifi_err));
    }
    else
    {
        esp_err_t const snmp_err = snmp_agent_start();
        if (snmp_err != ESP_OK)
        {
            ESP_LOGW(TAG, "SNMP agent start failed (%s)", esp_err_to_name(snmp_err));
        }
    }

    UART2_RxStartIT();
    uart_engine_init();
    uart_engine_set_enabled(s_uart_engine_enabled);
    ups_sub_adapter_select();

    while (1)
    {
        ups_bootstrap_task();
        ups_dynamic_update_task();
        ups_debug_status_print_task();
        uart_engine_tick();

        ups_loop_delay_safe(UPS_MAIN_LOOP_DELAY_MS);
    }
}
