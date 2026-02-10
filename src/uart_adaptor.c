#include "main.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ups_uart";

static SemaphoreHandle_t s_uart_lock;
static bool s_uart_ready;
static bool s_tx_inflight;

static void ups_uart_init_if_needed(void)
{
    if (s_uart_ready)
    {
        return;
    }

    uart_config_t const config = {
        .baud_rate = UPS_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_XTAL,
    };

    esp_err_t err = uart_driver_install(UPS_UART_PORT,
                                        UPS_UART_BUFFER_SIZE,
                                        UPS_UART_BUFFER_SIZE,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return;
    }

    err = uart_param_config(UPS_UART_PORT, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return;
    }

    err = uart_set_pin(UPS_UART_PORT,
                       UPS_UART_TX_GPIO,
                       UPS_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return;
    }

#if (UPS_UART_TX_INVERT != 0) || (UPS_UART_RX_INVERT != 0)
    uint32_t inverse_mask = 0U;
#if (UPS_UART_TX_INVERT != 0)
    inverse_mask |= UART_SIGNAL_TXD_INV;
#endif
#if (UPS_UART_RX_INVERT != 0)
    inverse_mask |= UART_SIGNAL_RXD_INV;
#endif
    err = uart_set_line_inverse(UPS_UART_PORT, inverse_mask);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_set_line_inverse failed: %s", esp_err_to_name(err));
        return;
    }
#endif

    s_uart_lock = xSemaphoreCreateMutex();
    if (s_uart_lock == NULL)
    {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return;
    }

    uint32_t real_baud = 0U;
    (void)uart_get_baudrate(UPS_UART_PORT, &real_baud);
    ESP_LOGI(TAG,
             "ready: uart=%u tx=%d rx=%d baud=%lu",
             (unsigned int)UPS_UART_PORT,
             UPS_UART_TX_GPIO,
             UPS_UART_RX_GPIO,
             (unsigned long)real_baud);

    s_tx_inflight = false;
    s_uart_ready = true;
}

uint32_t ups_tick_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

void UART2_RxStartIT(void)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready)
    {
        return;
    }

    (void)uart_flush_input(UPS_UART_PORT);
}

bool UART2_TryLock(void)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready || (s_uart_lock == NULL))
    {
        return false;
    }

    return (xSemaphoreTake(s_uart_lock, 0) == pdTRUE);
}

void UART2_Unlock(void)
{
    if (s_uart_lock == NULL)
    {
        return;
    }

    (void)xSemaphoreGive(s_uart_lock);
}

esp_err_t UART2_SendBytes(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if ((data == NULL) || (len == 0U))
    {
        return ESP_OK;
    }

    int const written = uart_write_bytes(UPS_UART_PORT, data, len);
    if (written != (int)len)
    {
        return ESP_FAIL;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if ((timeout_ms > 0U) && (wait_ticks == 0))
    {
        wait_ticks = 1;
    }

    if (uart_wait_tx_done(UPS_UART_PORT, wait_ticks) != ESP_OK)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_tx_inflight = false;
    return ESP_OK;
}

esp_err_t UART2_SendBytesDMA(const uint8_t *data, uint16_t len)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if ((data == NULL) || (len == 0U))
    {
        return ESP_OK;
    }

    int const written = uart_write_bytes(UPS_UART_PORT, data, len);
    if (written != (int)len)
    {
        ESP_LOGE(TAG,
                 "uart_write_bytes short write: wrote=%d need=%u",
                 written,
                 (unsigned int)len);
        s_tx_inflight = false;
        return ESP_FAIL;
    }

    s_tx_inflight = true;
    return ESP_OK;
}

bool UART2_TxDone(void)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready)
    {
        return true;
    }

    if (!s_tx_inflight)
    {
        return true;
    }

    if (uart_wait_tx_done(UPS_UART_PORT, 0) == ESP_OK)
    {
        s_tx_inflight = false;
        return true;
    }

    return false;
}

void UART2_TxDoneClear(void)
{
    s_tx_inflight = false;
}

uint16_t UART2_Available(void)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready)
    {
        return 0U;
    }

    size_t buffered = 0U;
    if (uart_get_buffered_data_len(UPS_UART_PORT, &buffered) != ESP_OK)
    {
        return 0U;
    }

    if (buffered > UINT16_MAX)
    {
        buffered = UINT16_MAX;
    }

    return (uint16_t)buffered;
}

int UART2_ReadByte(uint8_t *out)
{
    if (out == NULL)
    {
        return 0;
    }

    return (UART2_Read(out, 1U) == 1U) ? 1 : 0;
}

uint16_t UART2_Read(uint8_t *dst, uint16_t len)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready || (dst == NULL) || (len == 0U))
    {
        return 0U;
    }

    int const got = uart_read_bytes(UPS_UART_PORT, dst, len, 0);
    if (got <= 0)
    {
        return 0U;
    }

    return (uint16_t)got;
}

void UART2_DiscardBuffered(void)
{
    ups_uart_init_if_needed();
    if (!s_uart_ready)
    {
        return;
    }

    (void)uart_flush_input(UPS_UART_PORT);
}

bool UART2_ReadExactTimeout(uint8_t *dst, uint16_t len, uint32_t timeout_ms)
{
    if ((dst == NULL) || (len == 0U))
    {
        return true;
    }

    uint32_t const start_ms = ups_tick_ms();
    uint16_t got = 0U;

    while (got < len)
    {
        got += UART2_Read(&dst[got], (uint16_t)(len - got));
        if (got >= len)
        {
            return true;
        }

        if ((ups_tick_ms() - start_ms) >= timeout_ms)
        {
            return false;
        }

        vTaskDelay(1);
    }

    return true;
}
