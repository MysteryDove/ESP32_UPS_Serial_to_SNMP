#ifndef WIFI_CLIENT_H_
#define WIFI_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include <stdbool.h>

esp_err_t wifi_client_start(void);
bool wifi_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_CLIENT_H_
