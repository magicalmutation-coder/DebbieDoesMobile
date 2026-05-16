#pragma once
#include "esp_err.h"

/**
 * @brief  Initialise NVS flash and load saved configuration.
 *         If no config exists the defaults in settings.h are used.
 */
esp_err_t storage_init(void);

/**
 * @brief  Persist the current g_debbie_config to NVS.
 */
esp_err_t storage_save_config(void);

/**
 * @brief  Erase all Debbie NVS keys (factory reset).
 */
esp_err_t storage_factory_reset(void);
