/*
 * display_manager.c — Debbie's LVGL UI
 *
 * Layout (3.5" 480×320):
 *  ┌─────────────────────────────────────────────┐
 *  │  ◉ WiFi  ◉ AI  ║  Debbie  ║  🔋 85%  🔔 3  │  ← status bar  (32 px)
 *  ├────────────────────┬────────────────────────┤
 *  │                    │                        │
 *  │   Debbie avatar    │   Chat / notification  │
 *  │    (animated)      │        panel           │
 *  │                    │                        │
 *  ├────────────────────┴────────────────────────┤
 *  │  🎵  Artist — Song title         [▶ || ⏭]  │  ← Spotify bar (36 px)
 *  └─────────────────────────────────────────────┘
 *
 * For the 1.14" (240×135) version the avatar fills the screen with a
 * simplified overlay.
 */

#include "display_manager.h"
#include "settings.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdlib.h>

/* Conditional include for display driver */
/* Use the ST7789 panel header available in ESP-IDF v6. For some
 * 3.5" displays (ST7796) a compatible driver/header may not be
 * present in this IDF version; default to the ST7789 panel which
 * is commonly supported. */
#include "esp_lcd_panel_st7789.h"
#define LCD_CMD_BITS   8
#define LCD_PARAM_BITS 8

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "display";

/* Debug helper: set to 1 to skip heavy display init and avoid boot-loops
 * while iterating on LVGL/PSRAM issues. Remove or set to 0 for normal
 * operation once the root cause is fixed. */
#define DEBUG_SKIP_DISPLAY_INIT 0

/* Quick test mode: build a minimal LVGL screen with a single "Hello" label
 * to validate display hardware without heavy drawing (shadows/animations).
 * Set to 1 for flashing tests, revert to 0 to restore full UI. */
#define DEBUG_MINIMAL_UI 0
/* Orientation probe cycles multiple MADCTL combinations and is useful for
 * bring-up, but it can leave the panel in a test configuration. Keep off
 * during normal use. */
#define DEBUG_ORIENTATION_PROBE 0
/* Disable boot-time panel pattern tests for normal runs to avoid visual
 * confusion while validating LVGL text orientation. */
#define DEBUG_PANEL_PATTERN_TEST 0
/* LVGL is configured to flush RGB565 to the LCD. */
#define PANEL_RGB565_BYTES_PER_PIXEL 2

/* ── LVGL globals ─────────────────────────────────────────────────────────── */
static lv_disp_t            *s_disp     = NULL;
static SemaphoreHandle_t     s_lvgl_mux = NULL;
static esp_lcd_panel_handle_t s_panel   = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
/* LVGL input device (keypad / 5-way nav) */
static lv_indev_t           *s_indev_keypad = NULL;

/* Small DMA buffer and semaphore for chunked, async LVGL flushes */
#define LVGL_DMA_BUFFER_SIZE (8 * 1024)
static SemaphoreHandle_t s_trans_done_sem = NULL;
static uint8_t *s_dma_buf = NULL;

/* ── UI widget handles ───────────────────────────────────────────────────── */
/* Status bar */
static lv_obj_t *s_status_bar   = NULL;
static lv_obj_t *s_lbl_name     = NULL;
static lv_obj_t *s_lbl_wifi     = NULL;
static lv_obj_t *s_lbl_ai       = NULL;
static lv_obj_t *s_lbl_battery  = NULL;
static lv_obj_t *s_lbl_notif    = NULL;

/* Avatar panel */
static lv_obj_t *s_avatar_cont  = NULL;
static lv_obj_t *s_face_bg      = NULL;   /* circle */
static lv_obj_t *s_eye_left     = NULL;
static lv_obj_t *s_eye_right    = NULL;
static lv_obj_t *s_mouth        = NULL;
static lv_obj_t *s_state_lbl    = NULL;

/* Chat / notification panel */
static lv_obj_t *s_chat_cont    = NULL;
static lv_obj_t *s_chat_label   = NULL;
static lv_obj_t *s_notif_cont   = NULL;
static lv_obj_t *s_notif_label  = NULL;

/* Spotify bar */
static lv_obj_t *s_spotify_bar  = NULL;
static lv_obj_t *s_spotify_lbl  = NULL;
static lv_obj_t *s_netinfo_lbl  = NULL;

/* Camera preview */
static lv_obj_t *s_cam_canvas   = NULL;

/* ── Colour palette ─────────────────────────────────────────────────────── */
#define CLR_BG_IDLE         lv_color_hex(0x1A1A2E)   /* deep navy */
#define CLR_BG_LISTENING    lv_color_hex(0x16213E)   /* darker blue */
#define CLR_BG_THINKING     lv_color_hex(0x0F3460)   /* mid blue */
#define CLR_BG_SPEAKING     lv_color_hex(0x533483)   /* purple */
#define CLR_BG_NOTIF        lv_color_hex(0x2C3E50)   /* slate */
#define CLR_FACE            lv_color_hex(0xFFD6A5)   /* warm peach */
#define CLR_EYE_OPEN        lv_color_hex(0x5E548E)   /* violet */
#define CLR_EYE_CLOSED      lv_color_hex(0x9E8FB2)
#define CLR_MOUTH           lv_color_hex(0xFF6B6B)   /* coral */
#define CLR_ACCENT          lv_color_hex(0x06D6A0)   /* teal */
#define CLR_TEXT_PRIMARY    lv_color_hex(0xEEEEEE)
#define CLR_TEXT_SECONDARY  lv_color_hex(0xAAAAAA)
#define CLR_SPOTIFY         lv_color_hex(0x1DB954)   /* Spotify green */
#define CLR_WHATSAPP        lv_color_hex(0x25D366)
#define CLR_EMAIL           lv_color_hex(0x4285F4)
#define CLR_NOTIF_BADGE     lv_color_hex(0xFF4444)

/* ST7796 vendor init (used for some 3.x" Freenove panels). This mirrors
 * the DeepSeek / MediaKit initialization sequence and is applied when
 * DEBBIE_DISPLAY_35 is enabled in settings.h so the ST7789 driver can
 * be configured for ST7796-compatible panels. */
#if DEBBIE_DISPLAY_35
typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} st7796_lcd_init_cmd_t;

typedef struct {
    const st7796_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} st7796_vendor_config_t;

static const st7796_lcd_init_cmd_t st7796_lcd_init_cmds[] = {
    {0x11, (uint8_t []){ 0x00 }, 0, 120},
    {0x3A, (uint8_t []){ 0x05 }, 1, 0},
    {0xF0, (uint8_t []){ 0xC3 }, 1, 0},
    {0xF0, (uint8_t []){ 0x96 }, 1, 0},
    {0xB4, (uint8_t []){ 0x01 }, 1, 0},
    {0xB7, (uint8_t []){ 0xC6 }, 1, 0},
    {0xC0, (uint8_t []){ 0x80, 0x45 }, 2, 0},
    {0xC1, (uint8_t []){ 0x13 }, 1, 0},
    {0xC2, (uint8_t []){ 0xA7 }, 1, 0},
    {0xC5, (uint8_t []){ 0x0A }, 1, 0},
    {0xE8, (uint8_t []){ 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, (uint8_t []){ 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14, 0},
    {0xE1, (uint8_t []){ 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32},14, 0},
    {0xF0, (uint8_t []){ 0x3C }, 1, 0},
    {0xF0, (uint8_t []){ 0x69 }, 1, 120},
    {0x21, (uint8_t []){ 0x00 }, 0, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},
};
#endif

/* ── Eye blink animation ────────────────────────────────────────────────── */
static lv_anim_t s_blink_anim;
static int32_t   s_eye_height_px = 24;  /* set from eye object after creation */

static void eye_height_cb(void *obj, int32_t val)
{
    if (s_eye_left)  lv_obj_set_height(s_eye_left,  val);
    if (s_eye_right) lv_obj_set_height(s_eye_right, val);
}

/* Some board variants define nav pins that overlap display or UART pins.
 * Using those as keypad inputs corrupts LCD transfers and serial output. */
static int nav_pin_is_usable(int pin)
{
    if (pin < 0) {
        return 0;
    }
    if (pin == LCD_PIN_MOSI || pin == LCD_PIN_CLK || pin == LCD_PIN_CS ||
        pin == LCD_PIN_DC   || pin == LCD_PIN_RST || pin == LCD_PIN_BL) {
        return 0;
    }
    /* ESP32-S3 default console UART pins in this project. */
    if (pin == 43 || pin == 44) {
        return 0;
    }
    return 1;
}

static void start_blink_animation(void)
{
    lv_anim_init(&s_blink_anim);
    lv_anim_set_exec_cb(&s_blink_anim, eye_height_cb);
    lv_anim_set_var(&s_blink_anim, s_eye_left);
    lv_anim_set_values(&s_blink_anim, s_eye_height_px, 2);
    lv_anim_set_time(&s_blink_anim, 120);
    lv_anim_set_playback_time(&s_blink_anim, 120);
    lv_anim_set_delay(&s_blink_anim, 4000);
    lv_anim_set_repeat_count(&s_blink_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&s_blink_anim, 4500);
    lv_anim_start(&s_blink_anim);
}

/* ── LCD flush callback ─────────────────────────────────────────────────── */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    /* Signal that a chunk transfer completed so the flush loop can continue */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    /* prefer user_ctx semaphore if provided (registered as user_ctx)
       fall back to global semaphore for compatibility */
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    if (!sem) sem = s_trans_done_sem;
    if (sem) {
        xSemaphoreGiveFromISR(sem, &xHigherPriorityTaskWoken);
    }
    /* We handled the event — don't free the user_ctx here */
    return false;
}

/* Simple keypad reader for the 5-way navigation switch defined in `settings.h`.
 * Buttons are assumed active-low (pressed == 0). The read callback reports
 * LV_KEY_ENTER for center, and arrow keys for directions.
 */
/* LVGL input read callback — signature differs between LVGL v8 and v9 */
#if LVGL_VERSION_MAJOR >= 9
static void my_keypad_read(lv_indev_t *drv, lv_indev_data_t *data)
#else
static void my_keypad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
#endif
{
    static int last_key = 0;
    int act_key = 0;
    int pressed = 0;

    /* Center has highest priority */
#ifdef NAV_CENTER
    if (nav_pin_is_usable(NAV_CENTER) && gpio_get_level(NAV_CENTER) == 0) {
        act_key = LV_KEY_ENTER;
        pressed = 1;
    }
#endif
#ifdef NAV_UP
    if (!pressed && nav_pin_is_usable(NAV_UP) && gpio_get_level(NAV_UP) == 0) {
        act_key = LV_KEY_UP;
        pressed = 1;
    }
#endif
#ifdef NAV_DOWN
    if (!pressed && nav_pin_is_usable(NAV_DOWN) && gpio_get_level(NAV_DOWN) == 0) {
        act_key = LV_KEY_DOWN;
        pressed = 1;
    }
#endif
#ifdef NAV_LEFT
    if (!pressed && nav_pin_is_usable(NAV_LEFT) && gpio_get_level(NAV_LEFT) == 0) {
        act_key = LV_KEY_LEFT;
        pressed = 1;
    }
#endif
#ifdef NAV_RIGHT
    if (!pressed && nav_pin_is_usable(NAV_RIGHT) && gpio_get_level(NAV_RIGHT) == 0) {
        act_key = LV_KEY_RIGHT;
        pressed = 1;
    }
#endif

    if (pressed) {
        last_key = act_key;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    data->key = last_key;
}

/* Chunked, asynchronous flush using a small DMA buffer to avoid blocking long
 * SPI transfers which can trigger the watchdog. This breaks the area into
 * smaller blocks and sends them via esp_lcd_panel_draw_bitmap while waiting
 * on the panel IO callback semaphore to pace transfers. */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    int width = lv_area_get_width(area);
    int height = lv_area_get_height(area);
    int bytes_per_pixel = PANEL_RGB565_BYTES_PER_PIXEL;

    /* Ensure DMA buffer exists */
    if (!s_dma_buf) {
        lv_display_flush_ready(s_disp);
        return;
    }

    /* Number of pixels we can send per chunk */
    int chunk_pixels = LVGL_DMA_BUFFER_SIZE / bytes_per_pixel;
    int line_pixels = width;
    int max_lines_per_chunk = chunk_pixels / line_pixels;
    if (max_lines_per_chunk < 1) max_lines_per_chunk = 1;

    uint8_t *src = px_map;
    for (int y = 0; y < height;) {
        int lines = (height - y) > max_lines_per_chunk ? max_lines_per_chunk : (height - y);
        int chunk_bytes = lines * line_pixels * bytes_per_pixel;

        memcpy(s_dma_buf, src + y * line_pixels * bytes_per_pixel, chunk_bytes);

        /* Some ST7796 panels expect RGB565 bytes swapped before sending.
         * When using the 3.5" (DEBBIE_DISPLAY_35) configuration, swap
         * each pair of bytes in the DMA buffer to match the panel ordering. */
    #if DEBBIE_DISPLAY_35
        for (int _i = 0; _i < chunk_bytes; _i += 2) {
            uint8_t _t = s_dma_buf[_i];
            s_dma_buf[_i] = s_dma_buf[_i + 1];
            s_dma_buf[_i + 1] = _t;
        }
    #endif

        /* Start transfer for this chunk */
        esp_lcd_panel_draw_bitmap(s_panel,
                      area->x1, area->y1 + y,
                      area->x2 + 1, area->y1 + y + lines,
                      (const void *)s_dma_buf);

        /* Wait for the transfer to complete (signalled by on_color_trans_done) */
        if (s_trans_done_sem) {
            xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(2000));
        }

        y += lines;
    }

    /* Tell LVGL we're done */
    lv_display_flush_ready(s_disp);
}

/* ── LVGL tick / task ───────────────────────────────────────────────────── */
static void lvgl_tick_inc(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *pvParam)
{
    while (1) {
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY)) {
#if LVGL_VERSION_MAJOR >= 9
            lv_timer_handler();
#else
            lv_task_handler();
#endif
            xSemaphoreGive(s_lvgl_mux);
        }
        /* Slow down LVGL handler to reduce CPU load in test mode */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ── UI construction ────────────────────────────────────────────────────── */

static void build_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, CLR_BG_IDLE, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* ── Status bar ── */
    s_status_bar = lv_obj_create(screen);
    lv_obj_set_size(s_status_bar, LCD_WIDTH, 32);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(0x0D0D1A), 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_radius(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 4, 0);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_wifi = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI " --");
    lv_obj_set_style_text_color(s_lbl_wifi, CLR_TEXT_SECONDARY, 0);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_LEFT_MID, 4, 0);

    s_lbl_ai = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_ai, LV_SYMBOL_AUDIO " --");
    lv_obj_set_style_text_color(s_lbl_ai, CLR_TEXT_SECONDARY, 0);
    lv_obj_align_to(s_lbl_ai, s_lbl_wifi, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    s_lbl_name = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_name, "✨ Debbie");
    lv_obj_set_style_text_color(s_lbl_name, CLR_ACCENT, 0);
    lv_obj_align(s_lbl_name, LV_ALIGN_CENTER, 0, 0);

    s_lbl_battery = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_battery, LV_SYMBOL_BATTERY_FULL " --");
    lv_obj_set_style_text_color(s_lbl_battery, CLR_TEXT_SECONDARY, 0);
    lv_obj_align(s_lbl_battery, LV_ALIGN_RIGHT_MID, -50, 0);

    s_lbl_notif = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_notif, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(s_lbl_notif, CLR_TEXT_SECONDARY, 0);
    lv_obj_align(s_lbl_notif, LV_ALIGN_RIGHT_MID, -4, 0);

    int content_h = LCD_HEIGHT - 32 - 36;  /* minus status + spotify bars */

    /* ── Main content panel (full width, no avatar face) ── */
    s_chat_cont = lv_obj_create(screen);
    lv_obj_set_size(s_chat_cont, LCD_WIDTH, content_h);
    lv_obj_align(s_chat_cont, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_set_style_bg_color(s_chat_cont, lv_color_hex(0x12122A), 0);
    lv_obj_set_style_border_width(s_chat_cont, 0, 0);
    lv_obj_set_style_radius(s_chat_cont, 0, 0);
    lv_obj_set_style_pad_all(s_chat_cont, 8, 0);

    s_state_lbl = lv_label_create(s_chat_cont);
    lv_label_set_text(s_state_lbl, "Ready");
    lv_obj_set_style_text_color(s_state_lbl, CLR_ACCENT, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    s_chat_label = lv_label_create(s_chat_cont);
    lv_label_set_long_mode(s_chat_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_chat_label, LCD_WIDTH - 16);
    lv_label_set_text(s_chat_label,
        "Hi! I'm Debbie 😊\n"
        "I can chat, read your\n"
        "messages, and control music.\n\n"
        "Use voice, or open setup at\n"
        "http://192.168.4.1");
    lv_obj_set_style_text_color(s_chat_label, CLR_TEXT_PRIMARY, 0);
    lv_obj_align(s_chat_label, LV_ALIGN_TOP_LEFT, 0, 22);

    /* Spotify bar */
    s_spotify_bar = lv_obj_create(screen);
    lv_obj_set_size(s_spotify_bar, LCD_WIDTH, 36);
    lv_obj_align(s_spotify_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_spotify_bar, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(s_spotify_bar, 0, 0);
    lv_obj_set_style_radius(s_spotify_bar, 0, 0);
    lv_obj_set_style_pad_all(s_spotify_bar, 4, 0);
    lv_obj_clear_flag(s_spotify_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_spotify_lbl = lv_label_create(s_spotify_bar);
    lv_label_set_text(s_spotify_lbl, LV_SYMBOL_AUDIO "  Not connected to Spotify");
    lv_obj_set_style_text_color(s_spotify_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(s_spotify_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    s_netinfo_lbl = lv_label_create(s_spotify_bar);
    lv_label_set_text(s_netinfo_lbl, "AP: http://192.168.4.1  STA: --  AI: --");
    lv_label_set_long_mode(s_netinfo_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_netinfo_lbl, CLR_TEXT_SECONDARY, 0);
    lv_obj_set_width(s_netinfo_lbl, LCD_WIDTH / 2);
    lv_obj_align(s_netinfo_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
    ESP_LOGI(TAG, "UI built — LCD %dx%d", LCD_WIDTH, LCD_HEIGHT);
}

/* Minimal UI for quick testing (single centered label) */
static void build_ui_minimal(void)
{
    lv_obj_t *screen = lv_scr_act();
    /* Fill the screen with a high-contrast background for visibility testing */
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Centered label */
    lv_obj_t *lbl = lv_label_create(screen);
    lv_label_set_text(lbl, "HELLO");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);

    ESP_LOGI(TAG, "UI built (minimal test) — LCD %dx%d", LCD_WIDTH, LCD_HEIGHT);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t display_manager_init(void)
{
    esp_err_t rc = ESP_OK;
    ESP_LOGI(TAG, "display_manager_init: start");
    ESP_LOGI(TAG,
             "display profile: driver=%s host=%d mosi=%d sclk=%d dc=%d rst=%d bl=%d cs=%d freq=%d",
             LCD_DRIVER, LCD_SPI_HOST, LCD_PIN_MOSI, LCD_PIN_CLK, LCD_PIN_DC,
             LCD_PIN_RST, LCD_PIN_BL, LCD_PIN_CS, LCD_SPI_FREQ_HZ);
    if (DEBUG_SKIP_DISPLAY_INIT) {
        ESP_LOGW(TAG, "display_manager_init: SKIPPED (DEBUG_SKIP_DISPLAY_INIT=1)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_lvgl_mux = xSemaphoreCreateMutex();

    /* ── SPI bus ── */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = LCD_PIN_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    rc = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: spi_bus_initialize MOSI=%d SCLK=%d CS=%d",
                 LCD_PIN_MOSI, LCD_PIN_CLK, LCD_PIN_CS);
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: spi_bus_initialize OK");

    /* ── Panel IO ── */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_SPI_FREQ_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    rc = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                  &io_cfg, &io);
    if (rc != ESP_OK) {
        ESP_LOGI(TAG, "display_manager_init: creating panel IO (DC=%d CS=%d freq=%d)",
                 LCD_PIN_DC, LCD_PIN_CS, LCD_SPI_FREQ_HZ);
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: panel IO OK");

    /* Create semaphore and small DMA buffer for chunked async flushes
     * before registering callbacks so we can pass the semaphore as user_ctx. */
    s_trans_done_sem = xSemaphoreCreateBinary();
    if (!s_trans_done_sem) {
        ESP_LOGW(TAG, "display_manager_init: failed to create trans_done semaphore");
    }
    s_dma_buf = heap_caps_malloc(LVGL_DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "display_manager_init: dma_buf=%p size=%d", s_dma_buf, LVGL_DMA_BUFFER_SIZE);
    if (!s_dma_buf) {
        ESP_LOGW(TAG, "display_manager_init: failed to allocate DMA buffer (MALLOC_CAP_DMA)");
    }
    s_panel_io = io;

    /* Register panel IO callbacks for async transfer completion (pass semaphore as user_ctx) */
    esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    esp_lcd_panel_io_register_event_callbacks(io, &io_cbs, s_trans_done_sem);

    /* ── Panel driver ── */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .bits_per_pixel = 16,
        /* Freenove ST7796 examples use BGR MADCTL ordering. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
    };
#if DEBBIE_DISPLAY_35
    /* Provide ST7796 init sequence for 3.x" Freenove panels so the
     * st7789 driver can be configured to initialize ST7796-compatible
     * glass. */
    st7796_vendor_config_t st7796_vendor_config = {
        .init_cmds = st7796_lcd_init_cmds,
        .init_cmds_size = sizeof(st7796_lcd_init_cmds) / sizeof(st7796_lcd_init_cmds[0]),
    };
    panel_cfg.vendor_config = &st7796_vendor_config;
#endif
    rc = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: creating panel driver (RST=%d bpp=%d)",
                 LCD_PIN_RST, panel_cfg.bits_per_pixel);
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: panel driver created");
    rc = esp_lcd_panel_reset(s_panel);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: panel reset");
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: panel reset OK");
    rc = esp_lcd_panel_init(s_panel);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: panel init");
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: panel init OK");
    rc = esp_lcd_panel_disp_on_off(s_panel, true);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: panel display on");
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: panel display on OK");
    
#if DEBBIE_DISPLAY_35
    /* Force settings for 3.5" (ST7796-compatible) panels. These calls
     * set MADCTL/mirroring/inversion so the panel isn't left in an
     * unexpected vendor-default state. Adjust if your panel appears
     * rotated or colors inverted. */
    ESP_LOGI(TAG, "display_manager_init: applying ST7796 orientation (swap_xy=1, mirror_x=false, mirror_y=false, invert=1)");
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, false);
#else
    /* Default behavior for other panels */
    ESP_LOGI(TAG, "display_manager_init: applying default panel orientation (no invert/mirror)");
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
#endif
    

    /* Backlight on */
    gpio_hold_dis(LCD_PIN_BL);
    gpio_set_direction(LCD_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_PIN_BL, 1);
    gpio_hold_en(LCD_PIN_BL);
    ESP_LOGI(TAG, "display_manager_init: enabling backlight (pin %d)", LCD_PIN_BL);
    ESP_LOGI(TAG, "display_manager_init: backlight held high");
#if DEBUG_MINIMAL_UI && DEBUG_PANEL_PATTERN_TEST
    /* Quick polarity toggle test (200ms each) to help diagnose inverted/backlight issues) */
    gpio_set_level(LCD_PIN_BL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LCD_PIN_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "display_manager_init: backlight toggle test complete (DEBUG_MINIMAL_UI)");
#endif

#if DEBUG_MINIMAL_UI && DEBUG_PANEL_PATTERN_TEST
    /* Direct panel test: draw three horizontal color bands (unswapped RGB565)
     * to verify wiring, color ordering and offsets before LVGL runs. */
    ESP_LOGI(TAG, "display_manager_init: running direct panel test (unswapped color bands)");
    uint16_t *test_line = heap_caps_malloc(LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (test_line) {
        int band = LCD_HEIGHT / 3;
        /* Top: red */
        for (int i = 0; i < LCD_WIDTH; ++i) test_line[i] = 0xF800;
        for (int y = 0; y < band; ++y) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, test_line);
            if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
        }
        /* Mid: green */
        for (int i = 0; i < LCD_WIDTH; ++i) test_line[i] = 0x07E0;
        for (int y = band; y < band * 2; ++y) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, test_line);
            if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
        }
        /* Bottom: blue */
        for (int i = 0; i < LCD_WIDTH; ++i) test_line[i] = 0x001F;
        for (int y = band * 2; y < LCD_HEIGHT; ++y) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, test_line);
            if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
        }
        esp_lcd_panel_disp_on_off(s_panel, true);
        heap_caps_free(test_line);
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGI(TAG, "display_manager_init: direct panel test complete (unswapped)");

        /* Also run a swapped-bytes version immediately after so we can
         * compare hardware expectations for byte ordering without LVGL.
         * This helps determine if the panel expects MSB/LSB swapped. */
        uint16_t *swapped_line = heap_caps_malloc(LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (swapped_line) {
            ESP_LOGI(TAG, "display_manager_init: running direct panel test (swapped RGB565 bands)");
            int band = LCD_HEIGHT / 3;
            /* Fill with swapped bytes for red (0xF800) */
            for (int i = 0; i < LCD_WIDTH; ++i) {
                uint16_t v = 0xF800;
                swapped_line[i] = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));
            }
            for (int y = 0; y < band; ++y) {
                esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, swapped_line);
                if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
            }
            /* Green */
            for (int i = 0; i < LCD_WIDTH; ++i) {
                uint16_t v = 0x07E0;
                swapped_line[i] = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));
            }
            for (int y = band; y < band * 2; ++y) {
                esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, swapped_line);
                if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
            }
            /* Blue */
            for (int i = 0; i < LCD_WIDTH; ++i) {
                uint16_t v = 0x001F;
                swapped_line[i] = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));
            }
            for (int y = band * 2; y < LCD_HEIGHT; ++y) {
                esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, swapped_line);
                if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
            }
            esp_lcd_panel_disp_on_off(s_panel, true);
            heap_caps_free(swapped_line);
            ESP_LOGI(TAG, "display_manager_init: swapped direct panel test complete");
        } else {
            ESP_LOGW(TAG, "display_manager_init: failed to allocate swapped_line for direct panel test");
        }
    } else {
        ESP_LOGW(TAG, "display_manager_init: failed to allocate test_line for direct panel test");
    }

    /* Automated probe: cycle several orientation/inversion combos and draw
     * unswapped and swapped RGB565 bands for each to let us observe which
     * combination yields correct colors/ordering on the panel. */
#if DEBUG_ORIENTATION_PROBE
    {
        struct probe_cfg { int swap_xy; int mirror_x; int mirror_y; int invert; const char *name; };
        struct probe_cfg probe_list[] = {
            {0, 0, 0, 0, "normal"},
            {1, 0, 0, 0, "swap_xy"},
            {0, 1, 0, 0, "mirror_x"},
            {0, 0, 1, 0, "mirror_y"},
            {0, 0, 0, 1, "invert"},
            {1, 1, 0, 0, "swap_xy+mirror_x"},
        };

        uint16_t *probe_line = heap_caps_malloc(LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        uint16_t *probe_swapped = heap_caps_malloc(LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (probe_line && probe_swapped) {
            int probe_band = LCD_HEIGHT / 3;
            for (size_t p = 0; p < sizeof(probe_list) / sizeof(probe_list[0]); ++p) {
                ESP_LOGI(TAG, "display_manager_init: probe cfg %s swap=%d mirror_x=%d mirror_y=%d invert=%d",
                         probe_list[p].name, probe_list[p].swap_xy, probe_list[p].mirror_x, probe_list[p].mirror_y, probe_list[p].invert);

                esp_err_t rc;
                rc = esp_lcd_panel_invert_color(s_panel, probe_list[p].invert ? true : false);
                if (rc != ESP_OK) ESP_LOGW(TAG, "invert_color returned %d", rc);
                rc = esp_lcd_panel_swap_xy(s_panel, probe_list[p].swap_xy ? true : false);
                if (rc != ESP_OK) ESP_LOGW(TAG, "swap_xy returned %d", rc);
                rc = esp_lcd_panel_mirror(s_panel, probe_list[p].mirror_x ? true : false, probe_list[p].mirror_y ? true : false);
                if (rc != ESP_OK) ESP_LOGW(TAG, "mirror returned %d", rc);

                vTaskDelay(pdMS_TO_TICKS(120));

                /* Unswapped bands */
                for (int i = 0; i < LCD_WIDTH; ++i) probe_line[i] = 0xF800; /* red */
                for (int y = 0; y < probe_band; ++y) {
                    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, probe_line);
                    if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
                }
                for (int i = 0; i < LCD_WIDTH; ++i) probe_line[i] = 0x07E0; /* green */
                for (int y = probe_band; y < probe_band * 2; ++y) {
                    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, probe_line);
                    if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
                }
                for (int i = 0; i < LCD_WIDTH; ++i) probe_line[i] = 0x001F; /* blue */
                for (int y = probe_band * 2; y < LCD_HEIGHT; ++y) {
                    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, probe_line);
                    if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
                }
                esp_lcd_panel_disp_on_off(s_panel, true);
                vTaskDelay(pdMS_TO_TICKS(200));

                /* Swapped-byte bands */
                for (int i = 0; i < LCD_WIDTH; ++i) {
                    uint16_t v = 0xF800; probe_swapped[i] = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));
                }
                for (int y = 0; y < probe_band; ++y) {
                    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, probe_swapped);
                    if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
                }
                for (int i = 0; i < LCD_WIDTH; ++i) {
                    uint16_t v = 0x07E0; probe_swapped[i] = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));
                }
                for (int y = probe_band; y < probe_band * 2; ++y) {
                    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, probe_swapped);
                    if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
                }
                for (int i = 0; i < LCD_WIDTH; ++i) {
                    uint16_t v = 0x001F; probe_swapped[i] = (uint16_t)((v >> 8) | ((v & 0xFF) << 8));
                }
                for (int y = probe_band * 2; y < LCD_HEIGHT; ++y) {
                    esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + 1, probe_swapped);
                    if (s_trans_done_sem) xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(200));
                }
                esp_lcd_panel_disp_on_off(s_panel, true);
                vTaskDelay(pdMS_TO_TICKS(400));
            }

            heap_caps_free(probe_line);
            heap_caps_free(probe_swapped);
            ESP_LOGI(TAG, "display_manager_init: automated display probe complete");
        } else {
            if (probe_line) heap_caps_free(probe_line);
            if (probe_swapped) heap_caps_free(probe_swapped);
            ESP_LOGW(TAG, "display_manager_init: failed to allocate probe buffers");
        }
    }
#endif

    /* Ensure final orientation is restored after any test/probe drawing. */
#if DEBBIE_DISPLAY_35
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, false);
#endif
#endif

    /* ── LVGL ── */
    lv_init();
    ESP_LOGI(TAG, "display_manager_init: lv_init()");
    ESP_LOGI(TAG, "display_manager_init: lv_init done");

    /* Create LVGL display (LVGL v9 API) */
    static uint8_t *buf1 = NULL;
    static uint8_t *buf2 = NULL;

    s_disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (!s_disp) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        ESP_LOGI(TAG, "display_manager_init: creating LVGL display %dx%d", LCD_WIDTH, LCD_HEIGHT);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "display_manager_init: lv_display_create OK");

    /* Set color format and buffers. Try progressively smaller internal
     * DRAM allocations first to avoid using PSRAM which has been triggering
     * instability on some boards. If internal allocation fails at all sizes
     * we abort so the app can continue without creating LVGL buffers that
     * could cause a boot-loop. */
    ESP_LOGI(TAG, "display_manager_init: setting LVGL color format");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    size_t lines_options[] = { 10, 4, 2, 1 };
    size_t buf_pixels = 0;
    size_t buf_bytes = 0;

    /* Try several smaller sizes from internal DRAM only */
    for (size_t i = 0; i < sizeof(lines_options)/sizeof(lines_options[0]); ++i) {
        buf_pixels = (size_t)LCD_WIDTH * lines_options[i];
        buf_bytes  = buf_pixels * PANEL_RGB565_BYTES_PER_PIXEL;
        ESP_LOGI(TAG, "display_manager_init: trying internal buf pixels=%u bytes=%u (lines=%u)",
                 (unsigned)buf_pixels, (unsigned)buf_bytes, (unsigned)lines_options[i]);
        buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "display_manager_init: buf1(internal)=%p", buf1);
        if (buf1) break;
    }

    if (!buf1) {
        ESP_LOGW(TAG, "display_manager_init: failed to allocate internal LVGL buffer for any size; aborting LVGL setup to avoid PSRAM");
        return ESP_ERR_NO_MEM;
    }

    /* Try to get a second internal buffer for double-buffering; if it
     * fails, continue in single-buffer mode. */
    ESP_LOGI(TAG, "display_manager_init: trying to allocate secondary internal buffer (same size)");
    buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "display_manager_init: buf2(internal)=%p", buf2);
    if (!buf2) {
        ESP_LOGW(TAG, "display_manager_init: secondary internal buffer allocation failed; proceeding with single-buffer");
    }

#if LVGL_VERSION_MAJOR >= 9
    /* LVGL v9 expects buffer size in bytes. */
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    /* LVGL v8 expects buffer size in pixels. */
    lv_display_set_buffers(s_disp, buf1, buf2, buf_pixels, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    ESP_LOGI(TAG, "display_manager_init: LVGL buffers set and flush cb registered");
    ESP_LOGI(TAG, "display_manager_init: LVGL buffers set (pixels=%d bytes=%d)", (int)buf_pixels, (int)buf_bytes);

    /* Configure navigation switch GPIOs then register LVGL keypad.
     * On FNK0102, GPIO19 is used as an ADC ladder input in main.c.
     * Avoid reconfiguring it here when ladder mode is enabled. */
#if DEBBIE_USE_ADC_NAV_LADDER
    ESP_LOGI(TAG, "display_manager_init: LVGL keypad disabled (ADC nav ladder handled in main)");
#else
    uint64_t nav_pins_mask = 0ULL;
#ifdef NAV_UP
    if (nav_pin_is_usable(NAV_UP)) {
        nav_pins_mask |= (1ULL << NAV_UP);
    } else {
        ESP_LOGW(TAG, "display_manager_init: skipping NAV_UP pin %d (reserved/conflict)", NAV_UP);
    }
#endif
#ifdef NAV_DOWN
    if (nav_pin_is_usable(NAV_DOWN)) {
        nav_pins_mask |= (1ULL << NAV_DOWN);
    } else {
        ESP_LOGW(TAG, "display_manager_init: skipping NAV_DOWN pin %d (reserved/conflict)", NAV_DOWN);
    }
#endif
#ifdef NAV_LEFT
    if (nav_pin_is_usable(NAV_LEFT)) {
        nav_pins_mask |= (1ULL << NAV_LEFT);
    } else {
        ESP_LOGW(TAG, "display_manager_init: skipping NAV_LEFT pin %d (reserved/conflict)", NAV_LEFT);
    }
#endif
#ifdef NAV_RIGHT
    if (nav_pin_is_usable(NAV_RIGHT)) {
        nav_pins_mask |= (1ULL << NAV_RIGHT);
    } else {
        ESP_LOGW(TAG, "display_manager_init: skipping NAV_RIGHT pin %d (reserved/conflict)", NAV_RIGHT);
    }
#endif
#ifdef NAV_CENTER
    if (nav_pin_is_usable(NAV_CENTER)) {
        nav_pins_mask |= (1ULL << NAV_CENTER);
    } else {
        ESP_LOGW(TAG, "display_manager_init: skipping NAV_CENTER pin %d (reserved/conflict)", NAV_CENTER);
    }
#endif
    if (nav_pins_mask) {
        gpio_config_t io_conf = {
            .pin_bit_mask = nav_pins_mask,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        /* Register keypad input device (LVGL v9 uses lv_indev_create API) */
#if LVGL_VERSION_MAJOR >= 9
        lv_indev_t *indev = lv_indev_create();
        if (indev) {
            lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
            lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
            lv_indev_set_read_cb(indev, my_keypad_read);
            lv_indev_set_disp(indev, s_disp);
            s_indev_keypad = indev;
            ESP_LOGI(TAG, "display_manager_init: keypad indev registered %p", s_indev_keypad);
        } else {
            ESP_LOGW(TAG, "display_manager_init: failed to create keypad indev");
        }
#else
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_KEYPAD;
        indev_drv.read_cb = my_keypad_read;
        s_indev_keypad = lv_indev_drv_register(&indev_drv);
        ESP_LOGI(TAG, "display_manager_init: keypad indev registered %p", s_indev_keypad);
#endif
    }
    #endif

    /* LVGL tick timer: use LVGL's timer API where possible; use esp_timer for a periodic tick */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_inc,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    rc = esp_timer_create(&tick_args, &tick_timer);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: creating LVGL tick timer");
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: starting LVGL tick timer");
    rc = esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000 /* µs */);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %d", rc);
        ESP_LOGI(TAG, "display_manager_init: LVGL tick timer started");
        return rc;
    }
    ESP_LOGI(TAG, "display_manager_init: LVGL tick timer started");

    /* Build the initial UI (minimal for quick test if DEBUG_MINIMAL_UI=1) */
    if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY)) {
#if DEBUG_MINIMAL_UI
        build_ui_minimal();
#else
        build_ui();
#endif
        xSemaphoreGive(s_lvgl_mux);
    }

    /* LVGL task: create without pinning and run at low priority so it
     * cannot completely starve the idle task and trigger the Task WDT. */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL,
                tskIDLE_PRIORITY + 1, NULL);
    ESP_LOGI(TAG, "display_manager_init: creating lvgl_task (un-pinned, low prio)");

    ESP_LOGI(TAG, "Display ready — %s %dx%d", LCD_DRIVER, LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

/* ── State → face expression mapping ────────────────────────────────────── */

static void set_face_for_state(debbie_state_t state)
{
    lv_color_t bg;
    const char *status_text;
    lv_coord_t mouth_start, mouth_end;

    switch (state) {
    case DEBBIE_STATE_LISTENING:
        bg          = CLR_BG_LISTENING;
        status_text = "Listening... 👂";
        mouth_start = 200; mouth_end = 340;  /* big smile */
        /* Animate eyes — wider when listening */
        if (s_eye_left)  lv_obj_set_height(s_eye_left,  s_eye_height_px + 4);
        if (s_eye_right) lv_obj_set_height(s_eye_right, s_eye_height_px + 4);
        break;
    case DEBBIE_STATE_THINKING:
        bg          = CLR_BG_THINKING;
        status_text = "Thinking... 🤔";
        mouth_start = 220; mouth_end = 320;  /* neutral */
        if (s_eye_left)  lv_obj_set_height(s_eye_left,  s_eye_height_px / 2);
        if (s_eye_right) lv_obj_set_height(s_eye_right, s_eye_height_px / 2);
        break;
    case DEBBIE_STATE_SPEAKING:
        bg          = CLR_BG_SPEAKING;
        status_text = "Speaking 🗣️";
        mouth_start = 180; mouth_end = 360;  /* open mouth */
        if (s_eye_left)  lv_obj_set_height(s_eye_left,  s_eye_height_px);
        if (s_eye_right) lv_obj_set_height(s_eye_right, s_eye_height_px);
        break;
    case DEBBIE_STATE_NOTIFICATION:
        bg          = CLR_BG_NOTIF;
        status_text = "Notification! 🔔";
        mouth_start = 200; mouth_end = 340;
        break;
    case DEBBIE_STATE_CAMERA:
        bg          = CLR_BG_IDLE;
        status_text = "Camera 📷";
        mouth_start = 200; mouth_end = 340;
        break;
    case DEBBIE_STATE_SPOTIFY:
        bg          = CLR_BG_IDLE;
        status_text = "Music 🎵";
        mouth_start = 200; mouth_end = 340;
        break;
    case DEBBIE_STATE_SETUP:
        bg          = lv_color_hex(0x2D3436);
        status_text = "Setup mode ⚙️";
        mouth_start = 210; mouth_end = 330;
        break;
    case DEBBIE_STATE_CONNECTING:
        bg          = lv_color_hex(0x2D3436);
        status_text = "Connecting... 🔗";
        mouth_start = 210; mouth_end = 330;
        break;
    case DEBBIE_STATE_ERROR:
        bg          = lv_color_hex(0x5C1E1E);
        status_text = "Oops! 😅";
        mouth_start = 200; mouth_end = 340;
        break;
    case DEBBIE_STATE_IDLE:
    default:
        bg          = CLR_BG_IDLE;
        status_text = "Ready! 😊";
        mouth_start = 200; mouth_end = 340;
        if (s_eye_left)  lv_obj_set_height(s_eye_left,  s_eye_height_px);
        if (s_eye_right) lv_obj_set_height(s_eye_right, s_eye_height_px);
        break;
    }

    lv_obj_set_style_bg_color(lv_scr_act(), bg, 0);
    if (s_avatar_cont) {
        lv_obj_set_style_bg_color(s_avatar_cont, bg, 0);
    }
    if (s_mouth) {
        lv_arc_set_angles(s_mouth, mouth_start, mouth_end);
    }
    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, status_text);
    }
}

void display_manager_set_state(debbie_state_t state)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        set_face_for_state(state);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_show_notification(const debbie_notification_t *notif)
{
    if (!notif || !s_chat_label) return;
    const char *icon;
    switch (notif->type) {
    case NOTIF_TYPE_WHATSAPP: icon = "💬"; break;
    case NOTIF_TYPE_EMAIL:    icon = "📧"; break;
    case NOTIF_TYPE_SPOTIFY:  icon = "🎵"; break;
    default:                  icon = "🔔"; break;
    }
    char buf[220];
    snprintf(buf, sizeof(buf), "%s %s\n%s", icon, notif->sender, notif->preview);
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_chat_label, buf);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_show_text(const char *text)
{
    if (!text || !s_chat_label) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_chat_label, text);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_show_camera_frame(const uint8_t *rgb565)
{
    /* For simplicity we reuse the chat panel area for the camera preview.
     * A full implementation would use lv_canvas with a dedicated buffer. */
    if (!s_chat_label) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        if (rgb565 == NULL) {
            lv_label_set_text(s_chat_label, "");
        } else {
            lv_label_set_text(s_chat_label, "📷 Camera preview");
            /* TODO: Render rgb565 into lv_canvas for full preview */
        }
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_set_notif_count(int count)
{
    if (!s_lbl_notif) return;
    char buf[16];
    if (count > 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BELL " %d", count);
        lv_obj_set_style_text_color(s_lbl_notif, CLR_NOTIF_BADGE, 0);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(s_lbl_notif, CLR_TEXT_SECONDARY, 0);
    }
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_lbl_notif, buf);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_set_spotify_track(const char *artist, const char *title,
                                       bool is_playing)
{
    if (!s_spotify_lbl) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s — %s",
             is_playing ? "▶" : "⏸", artist ? artist : "", title ? title : "");
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_spotify_lbl, buf);
        lv_obj_set_style_text_color(s_spotify_lbl, CLR_SPOTIFY, 0);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_set_battery(uint8_t percent)
{
    if (!s_lbl_battery) return;
    const char *icon;
    if      (percent > 80) icon = LV_SYMBOL_BATTERY_FULL;
    else if (percent > 60) icon = LV_SYMBOL_BATTERY_3;
    else if (percent > 40) icon = LV_SYMBOL_BATTERY_2;
    else if (percent > 15) icon = LV_SYMBOL_BATTERY_1;
    else                   icon = LV_SYMBOL_BATTERY_EMPTY;
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %d%%", icon, percent);
    lv_color_t col = percent > 20 ? CLR_TEXT_SECONDARY : lv_color_hex(0xFF4444);
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_lbl_battery, buf);
        lv_obj_set_style_text_color(s_lbl_battery, col, 0);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_set_connection_status(bool wifi_ok, bool ai_ok)
{
    if (!s_lbl_wifi || !s_lbl_ai) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_lbl_wifi, wifi_ok ? LV_SYMBOL_WIFI " OK" : LV_SYMBOL_WIFI " --");
        lv_obj_set_style_text_color(s_lbl_wifi,
            wifi_ok ? CLR_ACCENT : CLR_TEXT_SECONDARY, 0);
        lv_label_set_text(s_lbl_ai, ai_ok ? LV_SYMBOL_AUDIO " AI" : LV_SYMBOL_AUDIO " --");
        lv_obj_set_style_text_color(s_lbl_ai,
            ai_ok ? CLR_ACCENT : CLR_TEXT_SECONDARY, 0);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_manager_set_network_info(const char *info)
{
    if (!info || !s_netinfo_lbl) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50))) {
        lv_label_set_text(s_netinfo_lbl, info);
        xSemaphoreGive(s_lvgl_mux);
    }
}
