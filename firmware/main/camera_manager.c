#include "camera_manager.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "camera";
static bool        s_initialised = false;

/* -------------------------------------------------------------------------- */

esp_err_t camera_manager_init(void)
{
    camera_config_t config = {
        .pin_pwdn    = CAM_PIN_PWDN,
        .pin_reset   = CAM_PIN_RESET,
        .pin_xclk    = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7      = CAM_PIN_D7,
        .pin_d6      = CAM_PIN_D6,
        .pin_d5      = CAM_PIN_D5,
        .pin_d4      = CAM_PIN_D4,
        .pin_d3      = CAM_PIN_D3,
        .pin_d2      = CAM_PIN_D2,
        .pin_d1      = CAM_PIN_D1,
        .pin_d0      = CAM_PIN_D0,
        .pin_vsync   = CAM_PIN_VSYNC,
        .pin_href    = CAM_PIN_HREF,
        .pin_pclk    = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,   /* 640×480 — balanced quality/size */
        .jpeg_quality = 12,              /* 0–63, lower = better quality */
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Optimise sensor settings for indoor / handheld use */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);        /* auto */
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 0);
        s->set_gain_ctrl(s, 1);
        s->set_agc_gain(s, 0);
        s->set_gainceiling(s, GAINCEILING_2X);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
    }

    s_initialised = true;
    ESP_LOGI(TAG, "Camera ready (OV2640, VGA JPEG)");
    return ESP_OK;
}

esp_err_t camera_manager_capture_jpeg(uint8_t **jpeg_buf, size_t *jpeg_len)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        return ESP_FAIL;
    }

    *jpeg_buf = malloc(fb->len);
    if (!*jpeg_buf) {
        esp_camera_fb_return(fb);
        return ESP_ERR_NO_MEM;
    }
    memcpy(*jpeg_buf, fb->buf, fb->len);
    *jpeg_len = fb->len;
    esp_camera_fb_return(fb);

    ESP_LOGD(TAG, "Captured JPEG: %zu bytes", *jpeg_len);
    return ESP_OK;
}

void camera_manager_free_frame(uint8_t *buf)
{
    free(buf);
}

esp_err_t camera_manager_capture_base64(char **b64_out, size_t *b64_len)
{
    uint8_t *jpeg    = NULL;
    size_t   jpeg_sz = 0;

    esp_err_t err = camera_manager_capture_jpeg(&jpeg, &jpeg_sz);
    if (err != ESP_OK) return err;

    /* Calculate required base64 buffer size */
    size_t b64_sz = 0;
    mbedtls_base64_encode(NULL, 0, &b64_sz, jpeg, jpeg_sz);

    *b64_out = malloc(b64_sz + 1);
    if (!*b64_out) {
        free(jpeg);
        return ESP_ERR_NO_MEM;
    }

    err = mbedtls_base64_encode((unsigned char *)*b64_out, b64_sz + 1,
                                b64_len, jpeg, jpeg_sz);
    (*b64_out)[*b64_len] = '\0';
    free(jpeg);

    if (err != 0) {
        free(*b64_out);
        *b64_out = NULL;
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Base64 frame: %zu chars", *b64_len);
    return ESP_OK;
}

esp_err_t camera_manager_set_power(bool on)
{
#if CAM_PIN_PWDN >= 0
    gpio_set_level(CAM_PIN_PWDN, on ? 0 : 1);  /* PWDN is active-low */
#endif
    return ESP_OK;
}
