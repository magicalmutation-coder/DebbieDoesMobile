#pragma once
#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief  Initialise BLE stack and start the Debbie BLE server.
 *
 *  Provides:
 *  - GAP advertising as "Debbie" (or g_debbie_config.ble_device_name)
 *  - Nordic UART Service (NUS) for wireless config and data exchange
 *  - Device Information Service (DIS)
 *  - Battery Service
 *
 *  The NUS RX characteristic accepts JSON commands identical to the HTTP
 *  /configure endpoint so phones/tablets can configure Debbie over BLE
 *  without needing to join the WiFi AP.
 *
 * @return ESP_OK on success, ESP_FAIL if BLE is disabled in config.
 */
esp_err_t bluetooth_manager_init(void);

/**
 * @brief  Stop BLE advertising and deinitialise the BLE stack.
 */
esp_err_t bluetooth_manager_deinit(void);

/**
 * @brief  Send a UTF-8 notification string to connected BLE client via NUS TX.
 *         Silently succeeds if no client is connected.
 */
void bluetooth_manager_notify(const char *text);

/**
 * @brief  Return true when a BLE central is connected.
 */
bool bluetooth_manager_is_connected(void);
