#pragma once
#include "esp_err.h"

/**
 * @brief  Start the HTTP configuration server.
 *
 *  Serves the captive-portal setup page at http://192.168.4.1/ and provides
 *  REST endpoints for configuration, status, and device control.
 *
 *  Endpoints:
 *    GET  /          → Setup / status HTML page
 *    POST /configure → Save WiFi, API key, companion URL, persona
 *    GET  /status    → JSON device status
 *    POST /reset     → Factory reset
 *    GET  /snapshot  → JPEG camera snapshot (if camera enabled)
 */
esp_err_t web_server_start(void);

/**
 * @brief  Stop the HTTP server.
 */
esp_err_t web_server_stop(void);
