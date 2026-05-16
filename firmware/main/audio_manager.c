#include "audio_manager.h"
#include "settings.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio";

#define BUF_SAMPLES  ((AUDIO_SAMPLE_RATE * AUDIO_BUF_MS) / 1000)
#define BUF_BYTES    (BUF_SAMPLES * sizeof(int16_t))

static i2s_chan_handle_t   s_rx_chan = NULL;
static i2s_chan_handle_t   s_tx_chan = NULL;
static TaskHandle_t        s_cap_task = NULL;
static audio_capture_cb_t  s_cap_cb  = NULL;
static uint8_t             s_volume  = 75;
static bool                s_capturing = false;

/* ── Volume scaling ──────────────────────────────────────────────────────── */

static void scale_volume(int16_t *buf, size_t count)
{
    float gain = s_volume / 100.0f;
    for (size_t i = 0; i < count; i++) {
        int32_t v = (int32_t)buf[i] * (int32_t)(gain * 256) >> 8;
        buf[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

/* ── Capture task ────────────────────────────────────────────────────────── */

static void capture_task(void *pvParam)
{
    int16_t *buf = malloc(BUF_BYTES);
    if (!buf) { ESP_LOGE(TAG, "OOM in capture task"); vTaskDelete(NULL); return; }

    while (s_capturing) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, buf,
                                         BUF_BYTES, &bytes_read,
                                         pdMS_TO_TICKS(100));
        if (err == ESP_OK && bytes_read > 0 && s_cap_cb) {
            s_cap_cb(buf, bytes_read / sizeof(int16_t));
        }
    }
    free(buf);
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t audio_manager_init(void)
{
    /* ── Microphone (RX) channel ── */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_MIC_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_rx_chan));

    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK,
            .ws   = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_std));

    /* ── Speaker (TX) channel ── */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_SPK_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_tx_chan, NULL));

    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCK,
            .ws   = I2S_SPK_LRCK,
            .dout = I2S_SPK_DATA,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "Audio initialised — %d Hz, %d-bit", AUDIO_SAMPLE_RATE, AUDIO_BITS);
    return ESP_OK;
}

esp_err_t audio_manager_start_capture(audio_capture_cb_t callback)
{
    if (s_capturing) return ESP_OK;
    s_cap_cb    = callback;
    s_capturing = true;
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    xTaskCreatePinnedToCore(capture_task, "audio_cap", 4096, NULL,
                            configMAX_PRIORITIES - 2, &s_cap_task, 1);
    return ESP_OK;
}

esp_err_t audio_manager_stop_capture(void)
{
    s_capturing = false;
    if (s_cap_task) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_cap_task = NULL;
    }
    i2s_channel_disable(s_rx_chan);
    return ESP_OK;
}

esp_err_t audio_manager_play_pcm(const int16_t *samples, size_t count)
{
    if (!s_tx_chan || !samples || count == 0) return ESP_ERR_INVALID_ARG;

    /* Make a mutable copy to apply volume */
    int16_t *buf = malloc(count * sizeof(int16_t));
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, samples, count * sizeof(int16_t));
    scale_volume(buf, count);

    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, buf,
                                      count * sizeof(int16_t),
                                      &written, portMAX_DELAY);
    free(buf);
    return err;
}

esp_err_t audio_manager_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    size_t samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int16_t *buf = malloc(samples * sizeof(int16_t));
    if (!buf) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < samples; i++) {
        double t = (double)i / AUDIO_SAMPLE_RATE;
        buf[i] = (int16_t)(8000 * sin(2.0 * M_PI * freq_hz * t));
    }
    esp_err_t err = audio_manager_play_pcm(buf, samples);
    free(buf);
    return err;
}

esp_err_t audio_manager_set_volume(uint8_t volume)
{
    s_volume = volume > 100 ? 100 : volume;
    return ESP_OK;
}

bool audio_manager_vad(const int16_t *samples, size_t count)
{
    /* Simple RMS energy threshold — tune THRESHOLD for your environment */
    const int32_t THRESHOLD = 800;
    int64_t energy = 0;
    for (size_t i = 0; i < count; i++)
        energy += (int64_t)samples[i] * samples[i];
    int32_t rms = (int32_t)(energy / (int64_t)count);
    return (rms > THRESHOLD * THRESHOLD);
}
