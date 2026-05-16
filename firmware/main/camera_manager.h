#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief  Initialise camera hardware.
 */
esp_err_t camera_manager_init(void);

/**
 * @brief  Capture a JPEG snapshot.
 *         The returned buffer MUST be freed with camera_manager_free_frame().
 *
 * @param[out] jpeg_buf  Pointer to JPEG data.
 * @param[out] jpeg_len  Length of JPEG data in bytes.
 */
esp_err_t camera_manager_capture_jpeg(uint8_t **jpeg_buf, size_t *jpeg_len);

/**
 * @brief  Free a frame previously returned by camera_manager_capture_jpeg().
 */
void camera_manager_free_frame(uint8_t *buf);

/**
 * @brief  Capture a JPEG, encode to base64, and return as a null-terminated
 *         string.  Caller must free() the returned pointer.
 *
 * @param[out] b64_out    Base64 string (malloc'd by this function).
 * @param[out] b64_len    Length of base64 string (excluding null terminator).
 */
esp_err_t camera_manager_capture_base64(char **b64_out, size_t *b64_len);

/**
 * @brief  Enable or disable camera power (if PWDN pin is wired).
 */
esp_err_t camera_manager_set_power(bool on);
