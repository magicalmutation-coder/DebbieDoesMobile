#pragma once
#include "esp_err.h"
#include "debbie.h"
#include <stdbool.h>

/**
 * @brief  Initialise the SPI LCD and LVGL graphics stack.
 *         Creates the LVGL timer task and renders the Debbie avatar UI.
 */
esp_err_t display_manager_init(void);

/**
 * @brief  Update the UI to reflect the new application state.
 *         Safe to call from any task — posts to the LVGL task internally.
 */
void display_manager_set_state(debbie_state_t state);

/**
 * @brief  Show a notification badge and brief overlay.
 */
void display_manager_show_notification(const debbie_notification_t *notif);

/**
 * @brief  Show a text bubble (AI transcript / message).
 */
void display_manager_show_text(const char *text);

/**
 * @brief  Show a camera preview frame (RGB565, LCD_WIDTH × LCD_HEIGHT).
 *         Pass NULL to hide the preview.
 */
void display_manager_show_camera_frame(const uint8_t *rgb565);

/**
 * @brief  Update the unread notification count badge.
 */
void display_manager_set_notif_count(int count);

/**
 * @brief  Legacy API kept for compatibility; Spotify footer visuals are disabled.
 */
void display_manager_set_spotify_track(const char *artist, const char *title,
                                       bool is_playing);

/**
 * @brief  Update battery percentage shown in status bar.
 */
void display_manager_set_battery(uint8_t percent);

/**
 * @brief  Update WiFi / AI connection icons in status bar.
 */
void display_manager_set_connection_status(bool wifi_ok, bool ai_ok);

/**
 * @brief  Show current network diagnostics text (AP/STA/AI state).
 */
void display_manager_set_network_info(const char *info);
