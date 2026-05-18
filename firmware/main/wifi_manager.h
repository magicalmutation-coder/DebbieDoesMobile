#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief  Initialise WiFi in station + AP (dual) mode.
 *
 *  - If WiFi credentials are saved, try to connect immediately.
 *  - Always start the soft-AP "Debbie" for first-run configuration.
 *  - Blocks until the STA connection succeeds, or times out and
 *    leaves the device in AP-only setup mode.
 *
 * @return ESP_OK on successful STA connection, ESP_FAIL if setup required.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief  Return true when the device has an IP address on the STA interface.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief  Attempt to (re)connect using stored credentials.
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * @brief  Store new WiFi credentials and reconnect.
 */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/**
 * @brief  Get STA interface IP as dotted string.
 *
 * @param[out] buf Destination buffer.
 * @param[in]  len Destination length.
 * @return true if STA has a valid IP, false otherwise (buf set to "--").
 */
bool wifi_manager_get_sta_ip(char *buf, size_t len);

/**
 * @brief  Get AP interface IP as dotted string.
 *
 * @param[out] buf Destination buffer.
 * @param[in]  len Destination length.
 * @return true if AP IP is available, false otherwise (buf set to default AP IP).
 */
bool wifi_manager_get_ap_ip(char *buf, size_t len);
