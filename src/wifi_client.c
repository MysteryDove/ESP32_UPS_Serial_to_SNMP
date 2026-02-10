#include "wifi_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "wifi_client";

#ifndef UPS_WIFI_STA_SSID
#define UPS_WIFI_STA_SSID ""
#endif

#ifndef UPS_WIFI_STA_PASSWORD
#define UPS_WIFI_STA_PASSWORD ""
#endif

#ifndef UPS_WIFI_CONNECT_TIMEOUT_MS
#define UPS_WIFI_CONNECT_TIMEOUT_MS 10000U
#endif

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_wifi_started = false;

enum
{
    WIFI_CONNECTED_BIT = BIT0,
};

static void wifi_client_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)arg;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START))
    {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
        return;
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED))
    {
        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        return;
    }

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP))
    {
        ip_event_got_ip_t const *got_ip = (const ip_event_got_ip_t *)event_data;
        if (got_ip != NULL)
        {
            ESP_LOGI(TAG, "Got IPv4 address: 0x%08lx", (unsigned long)got_ip->ip_info.ip.addr);
        }

        if (s_wifi_event_group != NULL)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

bool wifi_client_is_connected(void)
{
    if (s_wifi_event_group == NULL)
    {
        return false;
    }

    EventBits_t const bits = xEventGroupGetBits(s_wifi_event_group);
    return ((bits & WIFI_CONNECTED_BIT) != 0);
}

esp_err_t wifi_client_start(void)
{
    if (s_wifi_started)
    {
        return ESP_OK;
    }

    size_t const ssid_len = strlen(UPS_WIFI_STA_SSID);
    if ((ssid_len == 0U) || (ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid)))
    {
        ESP_LOGW(TAG,
                 "UPS_WIFI_STA_SSID is empty/invalid (set via build flag), skipping WiFi start");
        return ESP_ERR_INVALID_ARG;
    }

    size_t const password_len = strlen(UPS_WIFI_STA_PASSWORD);
    if (password_len >= sizeof(((wifi_config_t *)0)->sta.password))
    {
        ESP_LOGE(TAG, "UPS_WIFI_STA_PASSWORD too long");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_client_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_client_event_handler,
                                               NULL));

    if (s_wifi_event_group == NULL)
    {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL)
        {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    memcpy(wifi_config.sta.ssid, UPS_WIFI_STA_SSID, ssid_len);
    memcpy(wifi_config.sta.password, UPS_WIFI_STA_PASSWORD, password_len);

    wifi_config.sta.threshold.authmode =
        (password_len > 0U) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_started = true;

    EventBits_t const bits = xEventGroupWaitBits(s_wifi_event_group,
                                                 WIFI_CONNECTED_BIT,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(UPS_WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) != 0)
    {
        ESP_LOGI(TAG, "WiFi connected to SSID: %s", UPS_WIFI_STA_SSID);
    }
    else
    {
        ESP_LOGW(TAG,
                 "WiFi not connected yet (timeout=%u ms), background reconnect remains active",
                 (unsigned int)UPS_WIFI_CONNECT_TIMEOUT_MS);
    }

    return ESP_OK;
}
