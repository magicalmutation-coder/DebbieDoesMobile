#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief  Initialise I2S microphone and speaker peripherals.
 */
esp_err_t audio_manager_init(void);

/**
 * @brief  Start continuous microphone capture.
 *         Captured PCM frames are delivered via @p callback.
 *
 * @param callback  Called from ISR-safe context with int16_t PCM samples.
 *                  @p samples points to AUDIO_BUF_MS worth of mono 24 kHz data.
 *                  The buffer is invalidated after the callback returns.
 */
typedef void (*audio_capture_cb_t)(const int16_t *samples, size_t count);
esp_err_t audio_manager_start_capture(audio_capture_cb_t callback);
esp_err_t audio_manager_stop_capture(void);

/**
 * @brief  Play raw PCM data (int16, mono, 24 kHz) through the speaker.
 *         Blocks until the buffer has been sent to the I2S DMA.
 */
esp_err_t audio_manager_play_pcm(const int16_t *samples, size_t count);

/**
 * @brief  Play a tone (helpful for status sounds).
 */
esp_err_t audio_manager_beep(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief  Set speaker volume 0–100.
 */
esp_err_t audio_manager_set_volume(uint8_t volume);

/**
 * @brief  Detect voice activity in a PCM frame (simple energy threshold).
 */
bool audio_manager_vad(const int16_t *samples, size_t count);
