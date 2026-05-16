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

/* Conditional include for display driver */
#if DEBBIE_DISPLAY_35
#   include "esp_lcd_st7796.h"
#   define LCD_CMD_BITS   8
#   define LCD_PARAM_BITS 8
#else
#   include "esp_lcd_st7789.h"
#   define LCD_CMD_BITS   8
#   define LCD_PARAM_BITS 8
#endif

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "display";

/* ── LVGL globals ─────────────────────────────────────────────────────────── */
static lv_disp_t            *s_disp     = NULL;
static SemaphoreHandle_t     s_lvgl_mux = NULL;
static esp_lcd_panel_handle_t s_panel   = NULL;

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

/* ── Eye blink animation ────────────────────────────────────────────────── */
static lv_anim_t s_blink_anim;
static int32_t   s_eye_height_px = 24;  /* set from eye object after creation */

static void eye_height_cb(void *obj, int32_t val)
{
    lv_obj_set_height(s_eye_left,  val);
    lv_obj_set_height(s_eye_right, val);
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
    lv_disp_flush_ready(s_disp->driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
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
            lv_task_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
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

    /* ── Avatar panel (left half below status bar) ── */
    int avatar_w = LCD_WIDTH / 2;
    int content_h = LCD_HEIGHT - 32 - 36;  /* minus status + spotify bars */

    s_avatar_cont = lv_obj_create(screen);
    lv_obj_set_size(s_avatar_cont, avatar_w, content_h);
    lv_obj_align(s_avatar_cont, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_set_style_bg_color(s_avatar_cont, CLR_BG_IDLE, 0);
    lv_obj_set_style_border_width(s_avatar_cont, 0, 0);
    lv_obj_set_style_radius(s_avatar_cont, 0, 0);
    lv_obj_clear_flag(s_avatar_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Face background circle */
    int face_r = (avatar_w < content_h ? avatar_w : content_h) / 2 - 16;
    s_face_bg = lv_obj_create(s_avatar_cont);
    lv_obj_set_size(s_face_bg, face_r * 2, face_r * 2);
    lv_obj_align(s_face_bg, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(s_face_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_face_bg, CLR_FACE, 0);
    lv_obj_set_style_border_color(s_face_bg, CLR_ACCENT, 0);
    lv_obj_set_style_border_width(s_face_bg, 3, 0);
    lv_obj_set_style_shadow_color(s_face_bg, CLR_ACCENT, 0);
    lv_obj_set_style_shadow_width(s_face_bg, 20, 0);
    lv_obj_clear_flag(s_face_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Eyes */
    int eye_w = face_r / 4;
    int eye_h = face_r / 3;
    s_eye_left = lv_obj_create(s_face_bg);
    lv_obj_set_size(s_eye_left, eye_w, eye_h);
    lv_obj_align(s_eye_left, LV_ALIGN_CENTER, -(face_r / 3), -(face_r / 6));
    lv_obj_set_style_radius(s_eye_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_left, CLR_EYE_OPEN, 0);
    lv_obj_set_style_border_width(s_eye_left, 0, 0);

    s_eye_right = lv_obj_create(s_face_bg);
    lv_obj_set_size(s_eye_right, eye_w, eye_h);
    lv_obj_align(s_eye_right, LV_ALIGN_CENTER, (face_r / 3), -(face_r / 6));
    lv_obj_set_style_radius(s_eye_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_right, CLR_EYE_OPEN, 0);
    lv_obj_set_style_border_width(s_eye_right, 0, 0);

    /* Mouth (arc approximation with a label for simplicity) */
    s_mouth = lv_arc_create(s_face_bg);
    lv_arc_set_angles(s_mouth, 200, 340);  /* lower half arc = smile */
    lv_arc_set_value(s_mouth, 0);
    lv_obj_set_size(s_mouth, face_r, face_r / 2);
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, face_r / 4);
    lv_obj_set_style_arc_color(s_mouth, CLR_MOUTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_mouth, CLR_MOUTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_mouth, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_mouth, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_TRANSP, 0);
    lv_obj_remove_style(s_mouth, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_CLICKABLE);

    /* State label below avatar */
    s_state_lbl = lv_label_create(s_avatar_cont);
    lv_label_set_text(s_state_lbl, "Hello! 👋");
    lv_obj_set_style_text_color(s_state_lbl, CLR_ACCENT, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    s_eye_height_px = eye_h;

    /* ── Chat / notification panel (right half) ── */
    s_chat_cont = lv_obj_create(screen);
    lv_obj_set_size(s_chat_cont, LCD_WIDTH - avatar_w, content_h);
    lv_obj_align(s_chat_cont, LV_ALIGN_TOP_RIGHT, 0, 32);
    lv_obj_set_style_bg_color(s_chat_cont, lv_color_hex(0x12122A), 0);
    lv_obj_set_style_border_width(s_chat_cont, 0, 0);
    lv_obj_set_style_radius(s_chat_cont, 0, 0);
    lv_obj_set_style_pad_all(s_chat_cont, 8, 0);

    s_chat_label = lv_label_create(s_chat_cont);
    lv_label_set_long_mode(s_chat_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_chat_label, LCD_WIDTH - avatar_w - 16);
    lv_label_set_text(s_chat_label,
        "Hi! I'm Debbie 😊\n"
        "I can chat, show you what\n"
        "my camera sees, read your\n"
        "messages, and control music.\n\n"
        "Press the centre button\nor just say something!");
    lv_obj_set_style_text_color(s_chat_label, CLR_TEXT_PRIMARY, 0);
    lv_obj_align(s_chat_label, LV_ALIGN_TOP_LEFT, 0, 0);

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

    /* Start blink animation */
    start_blink_animation();

    ESP_LOGI(TAG, "UI built — LCD %dx%d", LCD_WIDTH, LCD_HEIGHT);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t display_manager_init(void)
{
    s_lvgl_mux = xSemaphoreCreateMutex();

    /* ── SPI bus ── */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = LCD_PIN_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

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
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                             &io_cfg, &io));

    /* ── Panel driver ── */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
#if DEBBIE_DISPLAY_35
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_cfg, &s_panel));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* Backlight on */
    gpio_set_direction(LCD_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_PIN_BL, 1);

    /* ── LVGL ── */
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t          buf1[LCD_WIDTH * 40];
    static lv_color_t          buf2[LCD_WIDTH * 40];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_WIDTH;
    disp_drv.ver_res    = LCD_HEIGHT;
    disp_drv.flush_cb   = lvgl_flush_cb;
    disp_drv.draw_buf   = &draw_buf;
    s_disp = lv_disp_drv_register(&disp_drv);

    /* LVGL tick timer */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_inc,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                    LVGL_TICK_MS * 1000 /* µs */));

    /* Build the initial UI */
    if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY)) {
        build_ui();
        xSemaphoreGive(s_lvgl_mux);
    }

    /* LVGL task */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL,
                            configMAX_PRIORITIES - 1, NULL, 1);

    ESP_LOGI(TAG, "Display ready — %s %dx%d", LCD_DRIVER, LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

/* ── State → face expression mapping ────────────────────────────────────── */

static void set_face_for_state(debbie_state_t state)
{
    if (!s_face_bg) return;
    lv_color_t bg;
    const char *status_text;
    lv_coord_t mouth_start, mouth_end;

    switch (state) {
    case DEBBIE_STATE_LISTENING:
        bg          = CLR_BG_LISTENING;
        status_text = "Listening... 👂";
        mouth_start = 200; mouth_end = 340;  /* big smile */
        /* Animate eyes — wider when listening */
        lv_obj_set_height(s_eye_left,  s_eye_height_px + 4);
        lv_obj_set_height(s_eye_right, s_eye_height_px + 4);
        break;
    case DEBBIE_STATE_THINKING:
        bg          = CLR_BG_THINKING;
        status_text = "Thinking... 🤔";
        mouth_start = 220; mouth_end = 320;  /* neutral */
        lv_obj_set_height(s_eye_left,  s_eye_height_px / 2);
        lv_obj_set_height(s_eye_right, s_eye_height_px / 2);
        break;
    case DEBBIE_STATE_SPEAKING:
        bg          = CLR_BG_SPEAKING;
        status_text = "Speaking 🗣️";
        mouth_start = 180; mouth_end = 360;  /* open mouth */
        lv_obj_set_height(s_eye_left,  s_eye_height_px);
        lv_obj_set_height(s_eye_right, s_eye_height_px);
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
        lv_obj_set_height(s_eye_left,  s_eye_height_px);
        lv_obj_set_height(s_eye_right, s_eye_height_px);
        break;
    }

    lv_obj_set_style_bg_color(lv_scr_act(), bg, 0);
    lv_obj_set_style_bg_color(s_avatar_cont, bg, 0);
    lv_arc_set_angles(s_mouth, mouth_start, mouth_end);
    lv_label_set_text(s_state_lbl, status_text);
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
