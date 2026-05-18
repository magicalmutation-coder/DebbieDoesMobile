/*
 * main.c — Debbie: Portable Personal AI Friend
 *
 * Freenove Media Kit ESP32-S3 (FNK0102)
 *
 * Features:
 *  ✅ Voice conversations via OpenAI Realtime API (gpt-4o-realtime-preview)
 *  ✅ Camera vision — capture and describe images using gpt-4o
 *  ✅ WhatsApp / Email / agent notification pager
 *  ✅ On-device launcher navigation (network route, setup, scanner)
 *  ✅ Practical full-width LVGL status/text UI
 *  ✅ WiFi captive-portal setup
 *  ✅ Custom agent endpoint support (ws:// or wss://)
 *  ✅ Battery monitoring
 *  ✅ OTA-ready partition layout
 */

#include "debbie.h"
#include "settings.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "bluetooth_manager.h"
#include "audio_manager.h"
#include "camera_manager.h"
#include "display_manager.h"
#include "openai_client.h"
#include "notification_client.h"
#include "web_server.h"
#include "network_scanner.h"
#include "vuln_reporter.h"
#include "self_agent.h"
#include "memory_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "build_info.h"

static const char *TAG = "debbie";

#ifndef DEBBIE_OPENAI_API_KEY_OVERRIDE
#define DEBBIE_OPENAI_API_KEY_OVERRIDE ""
#endif

/* ── Global state (declared in debbie.h) ─────────────────────────────────── */
volatile debbie_state_t g_debbie_state = DEBBIE_STATE_BOOT;
debbie_config_t         g_debbie_config = { 0 };
ESP_EVENT_DEFINE_BASE(DEBBIE_EVENT_BASE);

/* ── Event queue for button / async events ─────────────────────────────── */
typedef enum {
    BTN_EVT_CENTER = 0,
    BTN_EVT_UP,
    BTN_EVT_DOWN,
    BTN_EVT_LONG_PRESS,
} btn_event_t;

static QueueHandle_t s_btn_queue = NULL;

typedef enum {
    MENU_ITEM_NETWORK_ROUTE = 0,
    MENU_ITEM_PHONE_HOTSPOT,
    MENU_ITEM_VULN_SCAN,
    MENU_ITEM_NOTIFICATIONS,
    MENU_ITEM_COUNT,
} menu_item_t;

static bool       s_menu_open       = false;
static menu_item_t s_menu_selection = MENU_ITEM_NETWORK_ROUTE;
static bool       s_scan_running    = false;

typedef enum {
    NAV_LADDER_KEY_NONE = -1,
    NAV_LADDER_KEY_CENTER = 0,
    NAV_LADDER_KEY_UP,
    NAV_LADDER_KEY_DOWN,
    NAV_LADDER_KEY_LEFT,
    NAV_LADDER_KEY_RIGHT,
} nav_ladder_key_t;

static bool                       s_nav_ladder_enabled = false;
static adc_oneshot_unit_handle_t  s_nav_ladder_adc     = NULL;
static adc_unit_t                 s_nav_ladder_unit    = ADC_UNIT_1;
static adc_channel_t              s_nav_ladder_channel = ADC_CHANNEL_0;
static int                        s_nav_ladder_idle_raw = 3580;

#define NAV_LADDER_POLL_MS          20
#define NAV_LADDER_STABLE_TICKS      2
/* Freenove reference ladder points from driver_button.cpp (mV):
 * idle=2800, center=0, key1=700, key2=1350, key3=2000, key4=2600.
 * We scale these targets using measured idle raw at boot to avoid
 * board-to-board ADC variation and false "up" events near idle. */
#define NAV_LADDER_MV_IDLE              2800
#define NAV_LADDER_MV_CENTER              0
#define NAV_LADDER_MV_LEFT              700
#define NAV_LADDER_MV_RIGHT            1350
#define NAV_LADDER_MV_DOWN             2000
#define NAV_LADDER_MV_UP               2600
#define NAV_LADDER_MV_CENTER_MAX        180
#define NAV_LADDER_MV_MATCH_RANGE       140
#define NAV_LADDER_MV_IDLE_RANGE        180
#define NAV_LADDER_IDLE_SAMPLES          16

/* ── Voice activity detection state ─────────────────────────────────────── */
static bool     s_user_speaking     = false;
static uint32_t s_silence_ms        = 0;
static uint32_t s_vad_start_ms      = 0;
#define VAD_SILENCE_TIMEOUT_MS  1200   /* stop after 1.2 s of silence */
#define VAD_MIN_SPEECH_MS        200   /* ignore very short bursts */

/* ── Pending notification ─────────────────────────────────────────────────  */
static debbie_notification_t s_pending_notif = { 0 };
static bool                  s_has_notif     = false;

/* -------------------------------------------------------------------------- */

static void set_state(debbie_state_t new_state)
{
    g_debbie_state = new_state;
    display_manager_set_state(new_state);
    esp_event_post(DEBBIE_EVENT_BASE, DEBBIE_EVT_STATE_CHANGE,
                   &new_state, sizeof(new_state), 0);
}

/* ── OpenAI event callback ───────────────────────────────────────────────── */

static void on_oai_event(const oai_event_data_t *evt, void *ctx)
{
    switch (evt->type) {
    case OAI_EVT_CONNECTED:
        ESP_LOGI(TAG, "Connected to OpenAI Realtime API");
        display_manager_set_connection_status(true, true);
        /* Enter listening-ready mode immediately after connect so speech can
         * start without extra button presses. */
        s_menu_open = false;
        set_state(DEBBIE_STATE_LISTENING);
        display_manager_show_text(
            "Agent connected.\n"
            "Speech-to-text is ready.\n"
            "Speak now."
        );
        audio_manager_beep(880, 120);
        vTaskDelay(pdMS_TO_TICKS(80));
        audio_manager_beep(1100, 120);
        break;

    case OAI_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from OpenAI");
        display_manager_set_connection_status(true, false);
        set_state(DEBBIE_STATE_IDLE);
        break;

    case OAI_EVT_SESSION_CREATED:
        ESP_LOGI(TAG, "Session ready");
        break;

    case OAI_EVT_AUDIO_DELTA:
        /* Play audio immediately as it streams */
        set_state(DEBBIE_STATE_SPEAKING);
        audio_manager_play_pcm(evt->audio.pcm, evt->audio.count);
        break;

    case OAI_EVT_TRANSCRIPT:
        ESP_LOGI(TAG, "Transcript: %s", evt->transcript.text);
        display_manager_show_text(evt->transcript.text);
        /* Record AI turn in memory */
        memory_manager_add_turn("assistant", evt->transcript.text);
        memory_manager_sync_turn("assistant", evt->transcript.text);
        break;

    case OAI_EVT_USER_TRANSCRIPT:
        ESP_LOGI(TAG, "User said: %s", evt->transcript.text);
        /* Record user turn in memory */
        memory_manager_add_turn("user", evt->transcript.text);
        memory_manager_sync_turn("user", evt->transcript.text);
        break;

    case OAI_EVT_FUNCTION_CALL:
        ESP_LOGI(TAG, "Function call: %s(%s)", evt->fn.name, evt->fn.args_json);

        /* ── Camera ── */
        if (strcmp(evt->fn.name, "capture_image") == 0) {
#if DEBBIE_ENABLE_CAMERA_RUNTIME
            set_state(DEBBIE_STATE_CAMERA);
            char *b64 = NULL;
            size_t b64_len = 0;
            if (camera_manager_capture_base64(&b64, &b64_len) == ESP_OK && b64) {
                display_manager_show_text("📷 Captured! Analysing...");
                openai_client_send_image(b64,
                    "Please describe in detail what you see in this image, "
                    "including objects, people, text, colours, and anything notable.");
                free(b64);
                openai_client_send_function_result(evt->fn.call_id,
                    "{\"status\":\"ok\",\"message\":\"Image captured and sent for analysis\"}");
            } else {
                openai_client_send_function_result(evt->fn.call_id,
                    "{\"status\":\"error\",\"message\":\"Camera capture failed\"}");
            }
            set_state(DEBBIE_STATE_THINKING);
#else
            ESP_LOGW(TAG, "capture_image requested but camera runtime is disabled (DEBBIE_ENABLE_CAMERA_RUNTIME=0)");
            display_manager_show_text("📷 Camera is disabled on this hardware profile.");
            openai_client_send_function_result(evt->fn.call_id,
                "{\"status\":\"error\",\"message\":\"Camera runtime disabled on this hardware profile\"}");
#endif

        /* ── Notifications ── */
        } else if (strcmp(evt->fn.name, "read_notifications") == 0) {
            char *summary = notification_client_get_summary_json();
            if (summary) {
                char result[512];
                snprintf(result, sizeof(result),
                         "{\"notifications\":%s}", summary);
                openai_client_send_function_result(evt->fn.call_id, result);
                free(summary);
                notification_client_clear();
                display_manager_set_notif_count(0);
            } else {
                openai_client_send_function_result(evt->fn.call_id,
                    "{\"notifications\":[],\"message\":\"No pending notifications\"}");
            }

        /* ── Spotify ── */
        } else if (strcmp(evt->fn.name, "spotify_control") == 0) {
#if DEBBIE_ENABLE_SPOTIFY_RUNTIME
            cJSON *args = cJSON_Parse(evt->fn.args_json);
            cJSON *act  = args ? cJSON_GetObjectItem(args, "action") : NULL;
            if (act && cJSON_IsString(act)) {
                notification_client_spotify_command(act->valuestring);
                char result[128];
                snprintf(result, sizeof(result),
                         "{\"status\":\"ok\",\"action\":\"%s\"}", act->valuestring);
                openai_client_send_function_result(evt->fn.call_id, result);
                set_state(DEBBIE_STATE_SPOTIFY);
            } else {
                openai_client_send_function_result(evt->fn.call_id,
                    "{\"error\":\"action parameter required\"}");
            }
            if (args) cJSON_Delete(args);
#else
            display_manager_show_text("Spotify controls are paused in this build.");
            openai_client_send_function_result(evt->fn.call_id,
                "{\"status\":\"disabled\",\"message\":\"Spotify runtime is paused\"}");
#endif

        /* ── Network security & self-agent (all other tools) ── */
        } else if (strcmp(evt->fn.name, "save_memory") == 0) {
            /* AI can explicitly save a fact to long-term memory */
            cJSON *args = cJSON_Parse(evt->fn.args_json);
            cJSON *key  = args ? cJSON_GetObjectItem(args, "key")   : NULL;
            cJSON *val  = args ? cJSON_GetObjectItem(args, "value") : NULL;
            cJSON *imp  = args ? cJSON_GetObjectItem(args, "importance") : NULL;
            if (key && val && cJSON_IsString(key) && cJSON_IsString(val)) {
                uint8_t importance = imp ? (uint8_t)imp->valuedouble : 7;
                memory_manager_save_fact(key->valuestring,
                                         val->valuestring,
                                         importance);
                char result[256];
                snprintf(result, sizeof(result),
                         "{\"status\":\"ok\",\"key\":\"%s\",\"value\":\"%s\"}",
                         key->valuestring, val->valuestring);
                openai_client_send_function_result(evt->fn.call_id, result);
            } else {
                openai_client_send_function_result(evt->fn.call_id,
                    "{\"error\":\"key and value are required\"}");
            }
            if (args) cJSON_Delete(args);

        } else {
            /* Defer to the self_agent dispatcher for:
             * network_scan, get_vuln_report, system_info,
             * web_fetch, dns_lookup, cve_lookup */
            if (!self_agent_handle_function_call(evt->fn.name,
                                                 evt->fn.args_json,
                                                 evt->fn.call_id)) {
                ESP_LOGW(TAG, "Unknown function: %s", evt->fn.name);
                char result[128];
                snprintf(result, sizeof(result),
                         "{\"error\":\"Unknown function: %s\"}", evt->fn.name);
                openai_client_send_function_result(evt->fn.call_id, result);
            }
        }

        /* Return to thinking/idle after handling */
        if (g_debbie_state != DEBBIE_STATE_SPEAKING)
            set_state(DEBBIE_STATE_THINKING);
        break;

    case OAI_EVT_ERROR:
    {
        const char *err_msg = evt->error.message ? evt->error.message : "unknown";
        ESP_LOGE(TAG, "OpenAI error: %s", err_msg);

        bool invalid_key = strstr(err_msg, "Incorrect API key") != NULL ||
                           strstr(err_msg, "invalid_api_key") != NULL ||
                           strstr(err_msg, "API key") != NULL;
        if (invalid_key) {
            display_manager_show_text(
                "OpenAI key rejected.\n"
                "Open setup at 192.168.4.1\n"
                "and paste a valid sk- key.");
        } else {
            display_manager_show_text("😅 Something went wrong.\nPlease try again.");
        }
        set_state(DEBBIE_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        set_state(DEBBIE_STATE_IDLE);
        break;
    }

    default:
        break;
    }
}

/* ── Notification callback ───────────────────────────────────────────────── */

static void on_notification(const debbie_notification_t *notif, void *ctx)
{
    s_pending_notif = *notif;
    s_has_notif     = true;

    int count = notification_client_unread_count();
    display_manager_set_notif_count(count);
    display_manager_show_notification(notif);
    set_state(DEBBIE_STATE_NOTIFICATION);

    /* Gentle beep for notifications */
    audio_manager_beep(660, 80);

    /* Tell the AI about it if connected */
    if (openai_client_is_connected()) {
        char msg[256];
        const char *type_str;
        switch (notif->type) {
        case NOTIF_TYPE_WHATSAPP: type_str = "WhatsApp"; break;
        case NOTIF_TYPE_EMAIL:    type_str = "email";    break;
        case NOTIF_TYPE_AGENT:    type_str = "agent";    break;
        default:                  type_str = "notification"; break;
        }
        /* Truncate sender and preview to avoid overflowing `msg` */
        char sender_trunc[32];
        char preview_trunc[64];
        strncpy(sender_trunc, notif->sender, sizeof(sender_trunc) - 1);
        sender_trunc[sizeof(sender_trunc) - 1] = '\0';
        strncpy(preview_trunc, notif->preview, sizeof(preview_trunc) - 1);
        preview_trunc[sizeof(preview_trunc) - 1] = '\0';

        snprintf(msg, sizeof(msg),
             "[System: New %s from %s: \"%s\". "
             "Please let the user know briefly.]",
             type_str, sender_trunc, preview_trunc);
        openai_client_send_text(msg);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    set_state(DEBBIE_STATE_IDLE);
}

/* ── Microphone / VAD callback ───────────────────────────────────────────── */

static void on_audio_capture(const int16_t *samples, size_t count)
{
    if (g_debbie_state != DEBBIE_STATE_LISTENING &&
        g_debbie_state != DEBBIE_STATE_IDLE) {
        return;
    }

    bool voice = audio_manager_vad(samples, count);

    if (voice) {
        s_silence_ms = 0;
        if (!s_user_speaking) {
            s_user_speaking = true;
            s_vad_start_ms  = (uint32_t)(esp_timer_get_time() / 1000);
            set_state(DEBBIE_STATE_LISTENING);
        }
        /* Stream audio to OpenAI */
        if (openai_client_is_connected())
            openai_client_send_audio(samples, count);

    } else if (s_user_speaking) {
        s_silence_ms += AUDIO_BUF_MS;
        if (s_silence_ms >= VAD_SILENCE_TIMEOUT_MS) {
            uint32_t now_ms    = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t speech_ms = now_ms - s_vad_start_ms;
            s_user_speaking = false;
            s_silence_ms    = 0;

            if (speech_ms >= VAD_MIN_SPEECH_MS && openai_client_is_connected()) {
                set_state(DEBBIE_STATE_THINKING);
                openai_client_commit_audio();
            } else {
                set_state(DEBBIE_STATE_IDLE);
            }
        }
    }
}

/* ── Button ISR / debounce task ──────────────────────────────────────────── */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    btn_event_t evt = (btn_event_t)(uintptr_t)arg;
    xQueueSendFromISR(s_btn_queue, &evt, NULL);
}

static bool button_pin_is_usable(int pin)
{
    if (pin < 0) {
        return false;
    }
    if (pin == LCD_PIN_MOSI || pin == LCD_PIN_CLK || pin == LCD_PIN_CS ||
        pin == LCD_PIN_DC   || pin == LCD_PIN_RST || pin == LCD_PIN_BL) {
        return false;
    }
    /* ESP32-S3 default console UART pins in this project. */
    if (pin == 43 || pin == 44) {
        return false;
    }
    return true;
}

static int nav_ladder_mv_to_raw(int mv)
{
    int idle_raw = s_nav_ladder_idle_raw > 0 ? s_nav_ladder_idle_raw : 3580;
    if (mv <= 0) {
        return 0;
    }

    int raw = (mv * idle_raw + (NAV_LADDER_MV_IDLE / 2)) / NAV_LADDER_MV_IDLE;
    if (raw < 0) {
        return 0;
    }
    if (raw > 4095) {
        return 4095;
    }
    return raw;
}

static void nav_ladder_calibrate_idle_raw(void)
{
    if (!s_nav_ladder_adc) {
        return;
    }

    int sum = 0;
    int count = 0;
    for (int i = 0; i < NAV_LADDER_IDLE_SAMPLES; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_nav_ladder_adc, s_nav_ladder_channel, &raw) == ESP_OK) {
            sum += raw;
            count++;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }

    if (count > 0) {
        s_nav_ladder_idle_raw = sum / count;
    }

    ESP_LOGI(TAG,
             "NAV ladder: idle_raw=%d center<=%d left=%d right=%d down=%d up=%d",
             s_nav_ladder_idle_raw,
             nav_ladder_mv_to_raw(NAV_LADDER_MV_CENTER_MAX),
             nav_ladder_mv_to_raw(NAV_LADDER_MV_LEFT),
             nav_ladder_mv_to_raw(NAV_LADDER_MV_RIGHT),
             nav_ladder_mv_to_raw(NAV_LADDER_MV_DOWN),
             nav_ladder_mv_to_raw(NAV_LADDER_MV_UP));
}

static nav_ladder_key_t nav_ladder_decode_raw(int raw)
{
    int best_diff = 100000;
    nav_ladder_key_t best_key = NAV_LADDER_KEY_NONE;

    if (raw < 0) {
        return NAV_LADDER_KEY_NONE;
    }

    if (raw <= nav_ladder_mv_to_raw(NAV_LADDER_MV_CENTER_MAX)) {
        return NAV_LADDER_KEY_CENTER;
    }

    int idle_tol = nav_ladder_mv_to_raw(NAV_LADDER_MV_IDLE_RANGE);
    if (abs(raw - s_nav_ladder_idle_raw) <= idle_tol) {
        return NAV_LADDER_KEY_NONE;
    }

    int d_left = abs(raw - nav_ladder_mv_to_raw(NAV_LADDER_MV_LEFT));
    if (d_left < best_diff) {
        best_diff = d_left;
        best_key = NAV_LADDER_KEY_LEFT;
    }

    int d_right = abs(raw - nav_ladder_mv_to_raw(NAV_LADDER_MV_RIGHT));
    if (d_right < best_diff) {
        best_diff = d_right;
        best_key = NAV_LADDER_KEY_RIGHT;
    }

    int d_down = abs(raw - nav_ladder_mv_to_raw(NAV_LADDER_MV_DOWN));
    if (d_down < best_diff) {
        best_diff = d_down;
        best_key = NAV_LADDER_KEY_DOWN;
    }

    int d_up = abs(raw - nav_ladder_mv_to_raw(NAV_LADDER_MV_UP));
    if (d_up < best_diff) {
        best_diff = d_up;
        best_key = NAV_LADDER_KEY_UP;
    }

    if (best_diff <= nav_ladder_mv_to_raw(NAV_LADDER_MV_MATCH_RANGE)) {
        return best_key;
    }

    return NAV_LADDER_KEY_NONE;
}

static const char *nav_ladder_key_name(nav_ladder_key_t key)
{
    switch (key) {
    case NAV_LADDER_KEY_CENTER: return "center";
    case NAV_LADDER_KEY_UP:     return "up";
    case NAV_LADDER_KEY_DOWN:   return "down";
    case NAV_LADDER_KEY_LEFT:   return "left";
    case NAV_LADDER_KEY_RIGHT:  return "right";
    default:                    return "none";
    }
}

static bool nav_ladder_key_to_event(nav_ladder_key_t key, btn_event_t *evt)
{
    if (!evt) {
        return false;
    }

    switch (key) {
    case NAV_LADDER_KEY_CENTER:
        *evt = BTN_EVT_CENTER;
        return true;
    case NAV_LADDER_KEY_UP:
        *evt = BTN_EVT_UP;
        return true;
    case NAV_LADDER_KEY_DOWN:
    case NAV_LADDER_KEY_LEFT:
        *evt = BTN_EVT_DOWN;
        return true;
    case NAV_LADDER_KEY_RIGHT:
        /* Ignore right-lane ladder hits in 3-button mode to avoid
         * accidental top-button triggers from noisy ADC transitions. */
        return false;
    default:
        return false;
    }
}

static bool init_nav_ladder(void)
{
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t rc = adc_oneshot_io_to_channel(NAV_CENTER, &unit, &channel);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "NAV ladder: GPIO %d is not ADC-capable (%s)", NAV_CENTER, esp_err_to_name(rc));
        return false;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { 0 };
    unit_cfg.unit_id = unit;
    rc = adc_oneshot_new_unit(&unit_cfg, &s_nav_ladder_adc);
    if (rc != ESP_OK || !s_nav_ladder_adc) {
        ESP_LOGW(TAG, "NAV ladder: adc_oneshot_new_unit failed (%s)", esp_err_to_name(rc));
        s_nav_ladder_adc = NULL;
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = { 0 };
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    chan_cfg.atten = ADC_ATTEN_DB_12;
    rc = adc_oneshot_config_channel(s_nav_ladder_adc, channel, &chan_cfg);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "NAV ladder: adc_oneshot_config_channel failed (%s)", esp_err_to_name(rc));
        adc_oneshot_del_unit(s_nav_ladder_adc);
        s_nav_ladder_adc = NULL;
        return false;
    }

    s_nav_ladder_unit = unit;
    s_nav_ladder_channel = channel;
    nav_ladder_calibrate_idle_raw();
    ESP_LOGI(TAG, "NAV ladder: enabled on GPIO %d (ADC unit=%d channel=%d)",
             NAV_CENTER, (int)s_nav_ladder_unit, (int)s_nav_ladder_channel);
    return true;
}

static void button_ladder_task(void *pvParam)
{
    nav_ladder_key_t sample_prev = NAV_LADDER_KEY_NONE;
    nav_ladder_key_t stable_key = NAV_LADDER_KEY_NONE;
    nav_ladder_key_t dispatched_key = NAV_LADDER_KEY_NONE;
    int stable_ticks = 0;

    while (1) {
        int raw = 4095;
        if (!s_nav_ladder_enabled || !s_nav_ladder_adc) {
            vTaskDelay(pdMS_TO_TICKS(NAV_LADDER_POLL_MS));
            continue;
        }

        esp_err_t rc = adc_oneshot_read(s_nav_ladder_adc, s_nav_ladder_channel, &raw);
        if (rc != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(NAV_LADDER_POLL_MS));
            continue;
        }

        nav_ladder_key_t sample_key = nav_ladder_decode_raw(raw);
        if (sample_key == sample_prev) {
            if (stable_ticks < 100) {
                stable_ticks++;
            }
        } else {
            sample_prev = sample_key;
            stable_ticks = 0;
        }

        if (stable_ticks >= NAV_LADDER_STABLE_TICKS && sample_key != stable_key) {
            stable_key = sample_key;

            if (stable_key == NAV_LADDER_KEY_NONE) {
                dispatched_key = NAV_LADDER_KEY_NONE;
            } else if (stable_key != dispatched_key) {
                btn_event_t evt;
                if (nav_ladder_key_to_event(stable_key, &evt)) {
                    ESP_LOGI(TAG, "NAV ladder press: key=%s raw=%d evt=%d",
                             nav_ladder_key_name(stable_key), raw, (int)evt);
                    xQueueSend(s_btn_queue, &evt, 0);
                }
                dispatched_key = stable_key;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(NAV_LADDER_POLL_MS));
    }
}

static const char *menu_item_name(menu_item_t item)
{
    switch (item) {
    case MENU_ITEM_NETWORK_ROUTE: return "Network Route";
    case MENU_ITEM_PHONE_HOTSPOT: return "Phone Hotspot Setup";
    case MENU_ITEM_VULN_SCAN:     return "Vulnerability Scanner";
    case MENU_ITEM_NOTIFICATIONS: return "Notifications";
    default:                      return "Menu";
    }
}

static void show_network_route_screen(void)
{
    char ap_ip[20];
    char sta_ip[20];
    wifi_manager_get_ap_ip(ap_ip, sizeof(ap_ip));
    wifi_manager_get_sta_ip(sta_ip, sizeof(sta_ip));

    char msg[420];
    snprintf(msg, sizeof(msg),
             "Network Route\n"
             "Upstream: phone hotspot or home WiFi\n"
             "Debbie joins as STA client\n"
             "STA IP: %s\n"
             "Setup AP fallback: http://%s\n\n"
             "Use saved SSIDs for your phone hotspot.",
             sta_ip,
             ap_ip);
    display_manager_show_text(msg);
}

static void show_phone_hotspot_guide(void)
{
    display_manager_show_text(
        "Phone Hotspot Setup\n"
        "1) Enable hotspot on phone\n"
        "2) Save hotspot SSID/pass in Debbie\n"
        "3) Debbie auto-joins as client\n"
        "4) Keep Debbie AP for setup fallback\n\n"
        "Tip: put hotspot in SSID slot 1."
    );
}

static void show_notifications_card(void)
{
    int unread = notification_client_unread_count();
    char *summary = notification_client_get_summary_json();
    char summary_trunc[200] = "[]";
    if (summary) {
        strncpy(summary_trunc, summary, sizeof(summary_trunc) - 1);
        summary_trunc[sizeof(summary_trunc) - 1] = '\0';
        free(summary);
    }

    char msg[360];
    snprintf(msg, sizeof(msg),
             "Notifications\n"
             "Unread: %d\n"
             "%s",
             unread,
             summary_trunc);
    display_manager_show_text(msg);
}

static void show_menu_screen(void)
{
    char msg[520];
    snprintf(msg, sizeof(msg),
             "Launcher Menu\n"
             "%s %s\n"
             "%s %s\n"
             "%s %s\n"
             "%s %s\n\n"
             "Top button: mic quick action (always)\n"
             "Middle button: next menu item\n"
             "Bottom button: open selected item",
             s_menu_selection == MENU_ITEM_NETWORK_ROUTE ? ">" : " ", menu_item_name(MENU_ITEM_NETWORK_ROUTE),
             s_menu_selection == MENU_ITEM_PHONE_HOTSPOT ? ">" : " ", menu_item_name(MENU_ITEM_PHONE_HOTSPOT),
             s_menu_selection == MENU_ITEM_VULN_SCAN ? ">" : " ", menu_item_name(MENU_ITEM_VULN_SCAN),
             s_menu_selection == MENU_ITEM_NOTIFICATIONS ? ">" : " ", menu_item_name(MENU_ITEM_NOTIFICATIONS));
    display_manager_show_text(msg);
}

static void menu_scan_progress_cb(uint8_t pct, const char *stage, void *ctx)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "Scanner\n%s\n%d%%", stage ? stage : "Running...", pct);
    display_manager_show_text(msg);
}

static void menu_scan_done_cb(const scan_results_t *results, void *ctx)
{
    vuln_report_t report = { 0 };
    vuln_reporter_analyse(results, &report);

    char msg[420];
    snprintf(msg, sizeof(msg),
             "Scan Complete\n"
             "Hosts: %d\n"
             "APs: %d\n"
             "Findings: %d (critical %d, high %d)\n\n"
             "Middle button for launcher menu.",
             results ? results->host_count : 0,
             results ? results->ap_count : 0,
             report.count,
             report.critical_count,
             report.high_count);
    display_manager_show_text(msg);
    s_scan_running = false;
    set_state(DEBBIE_STATE_IDLE);
}

static void menu_scan_task(void *pvParam)
{
    esp_err_t rc = net_scanner_full_scan(menu_scan_progress_cb, menu_scan_done_cb, NULL);
    if (rc != ESP_OK) {
        s_scan_running = false;
        set_state(DEBBIE_STATE_IDLE);
        display_manager_show_text("Scanner failed to start. Check WiFi connection.");
    }
    vTaskDelete(NULL);
}

static void launch_vuln_scan_from_menu(void)
{
    if (s_scan_running) {
        display_manager_show_text("Scanner already running...");
        return;
    }

    s_scan_running = true;
    s_menu_open = false;
    set_state(DEBBIE_STATE_SCANNING);
    display_manager_show_text(
        "Scanner starting...\n"
        "Authorised use only.\n"
        "Scanning your current network."
    );
    xTaskCreate(menu_scan_task, "menu_scan", 8192, NULL, 4, NULL);
}

static void activate_menu_item(menu_item_t item)
{
    s_menu_open = false;
    switch (item) {
    case MENU_ITEM_NETWORK_ROUTE:
        show_network_route_screen();
        break;
    case MENU_ITEM_PHONE_HOTSPOT:
        show_phone_hotspot_guide();
        break;
    case MENU_ITEM_VULN_SCAN:
        launch_vuln_scan_from_menu();
        break;
    case MENU_ITEM_NOTIFICATIONS:
        show_notifications_card();
        break;
    default:
        break;
    }
}

static void button_task(void *pvParam)
{
    btn_event_t evt;
    while (1) {
        if (xQueueReceive(s_btn_queue, &evt, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50));  /* debounce */

            switch (evt) {
            case BTN_EVT_CENTER:
                /* Middle button: cycle launcher menu */
                s_menu_open = true;
                s_menu_selection = (menu_item_t)((s_menu_selection + 1) % MENU_ITEM_COUNT);
                show_menu_screen();
                break;

            case BTN_EVT_UP:
                /* Top button: mic quick action */
                s_menu_open = false;

                if (openai_client_is_connected()) {
                    set_state(DEBBIE_STATE_LISTENING);
                    audio_manager_beep(740, 80);
                    display_manager_show_text(
                        "Mic quick action\n"
                        "Speak now. Debbie is listening."
                    );
                } else {
                    display_manager_show_text(
                        "Mic quick action\n"
                        "AI is offline right now.\n"
                        "Check network route and provider settings."
                    );
                }
                break;

            case BTN_EVT_DOWN:
                /* Bottom button: open launcher / activate selected item */
                if (!s_menu_open) {
                    s_menu_open = true;
                    show_menu_screen();
                } else {
                    activate_menu_item(s_menu_selection);
                }
                break;

            case BTN_EVT_LONG_PRESS:
                /* Long press: read notifications aloud (or show summary if offline) */
                if (openai_client_is_connected()) {
                    char *summary = notification_client_get_summary_json();
                    if (summary) {
                        char text[512];
                        snprintf(text, sizeof(text),
                            "[System: Please read out the following notifications "
                            "to the user: %s]", summary);
                        openai_client_send_text(text);
                        free(summary);
                        notification_client_clear();
                        display_manager_set_notif_count(0);
                    } else {
                        openai_client_send_text("Are there any notifications for me?");
                    }
                } else {
                    show_notifications_card();
                }
                break;
            }
        }
    }
}

/* ── Battery monitoring task ─────────────────────────────────────────────── */

static void battery_task(void *pvParam)
{
    /* ADC driver API differs across ESP-IDF versions. Stub battery
     * reporting during builds to avoid dependency on target-specific
     * ADC headers. Display will show full battery until ADC is
     * implemented for this IDF release. */
    while (1) {
        display_manager_set_battery(100);
        vTaskDelay(pdMS_TO_TICKS(30000));  /* update every 30 s */
    }
}

/* ── GPIO setup ──────────────────────────────────────────────────────────── */

static void gpio_init(void)
{
    s_btn_queue = xQueueCreate(8, sizeof(btn_event_t));
    s_nav_ladder_enabled = init_nav_ladder();

    gpio_config_t io_cfg = {
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };

    bool isr_service_ready = false;

    if (s_nav_ladder_enabled) {
        ESP_LOGI(TAG, "Using ADC nav ladder mode; NAV_CENTER/NAV_UP/NAV_DOWN GPIO ISRs disabled");
        xTaskCreate(button_ladder_task, "btn_ladder", 4096, NULL, 6, NULL);
    } else {
        if (button_pin_is_usable(NAV_CENTER)) {
            io_cfg.pin_bit_mask = (1ULL << NAV_CENTER);
            gpio_config(&io_cfg);
            if (!isr_service_ready) {
                gpio_install_isr_service(0);
                isr_service_ready = true;
            }
            gpio_isr_handler_add(NAV_CENTER, gpio_isr_handler, (void *)BTN_EVT_CENTER);
        } else {
            ESP_LOGW(TAG, "Skipping NAV_CENTER pin %d (reserved/conflict)", NAV_CENTER);
        }

        if (button_pin_is_usable(NAV_UP)) {
            io_cfg.pin_bit_mask = (1ULL << NAV_UP);
            gpio_config(&io_cfg);
            if (!isr_service_ready) {
                gpio_install_isr_service(0);
                isr_service_ready = true;
            }
            gpio_isr_handler_add(NAV_UP, gpio_isr_handler, (void *)BTN_EVT_UP);
        } else {
            ESP_LOGW(TAG, "Skipping NAV_UP pin %d (reserved/conflict)", NAV_UP);
        }

        if (button_pin_is_usable(NAV_DOWN)) {
            io_cfg.pin_bit_mask = (1ULL << NAV_DOWN);
            gpio_config(&io_cfg);
            if (!isr_service_ready) {
                gpio_install_isr_service(0);
                isr_service_ready = true;
            }
            gpio_isr_handler_add(NAV_DOWN, gpio_isr_handler, (void *)BTN_EVT_DOWN);
        } else {
            ESP_LOGW(TAG, "Skipping NAV_DOWN pin %d (reserved/conflict)", NAV_DOWN);
        }
    }

    xTaskCreate(button_task, "btn", 4096, NULL, 5, NULL);
}

/* ── Boot splash ─────────────────────────────────────────────────────────── */

static void show_boot_splash(void)
{
    display_manager_show_text(
        "✨ Debbie starting up...\n\n"
        "Please wait while I connect\n"
        "to WiFi and the AI...\n\n"
        "This takes about 10 seconds.");
    audio_manager_beep(440, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_manager_beep(550, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    audio_manager_beep(660, 200);
}

static const char *active_openai_api_key(void)
{
    if (DEBBIE_OPENAI_API_KEY_OVERRIDE[0] != '\0') {
        return DEBBIE_OPENAI_API_KEY_OVERRIDE;
    }
    return g_debbie_config.openai_api_key;
}

static bool has_openai_api_key(void)
{
    return active_openai_api_key()[0] != '\0';
}

static esp_err_t connect_openai_if_configured(void)
{
    const bool use_openai   = strcmp(g_debbie_config.llm_provider, LLM_PROVIDER_OPENAI) == 0;
    const bool use_lmstudio = strcmp(g_debbie_config.llm_provider, LLM_PROVIDER_LMSTUDIO) == 0;
    const char *openai_api_key = active_openai_api_key();

    if (g_debbie_config.use_custom_agent) {
        ESP_LOGW(TAG, "Custom agent mode enabled; OpenAI Realtime connection skipped");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!use_openai && !use_lmstudio) {
        ESP_LOGW(TAG,
                 "LLM provider '%s' is configured, but voice runtime currently supports '%s' or '%s'",
                 g_debbie_config.llm_provider,
                 LLM_PROVIDER_OPENAI,
                 LLM_PROVIDER_LMSTUDIO);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (use_openai && !has_openai_api_key()) {
        ESP_LOGW(TAG, "OpenAI API key missing; AI voice features are offline");
        return ESP_ERR_INVALID_STATE;
    }

    if (use_lmstudio && !g_debbie_config.local_llm_url[0]) {
        ESP_LOGW(TAG, "LM Studio provider selected but local_llm_url is empty");
        return ESP_ERR_INVALID_STATE;
    }

    if (use_lmstudio) {
        if (strstr(g_debbie_config.local_llm_url, ":11434")) {
            ESP_LOGW(TAG,
                     "LM Studio provider is using port 11434 (commonly Ollama). "
                     "If this is LM Studio, try port 1234.");
        }
        if (strncmp(g_debbie_config.local_llm_model, "gpt-", 4) == 0) {
            ESP_LOGW(TAG,
                     "LM Studio local model is '%s'. Confirm this model exists in LM Studio.",
                     g_debbie_config.local_llm_model);
        }
    }

    const char *base_prompt = g_debbie_config.system_prompt[0]
                            ? g_debbie_config.system_prompt
                            : DEBBIE_DEFAULT_SYSTEM_PROMPT;
    char *prompt = memory_manager_enrich_prompt(base_prompt);
    esp_err_t rc = openai_client_connect(
        use_openai ? openai_api_key : NULL,
        prompt ? prompt : base_prompt,
        on_oai_event,
        NULL
    );
    if (prompt) {
        free(prompt);
    }
    return rc;
}

static void refresh_network_info_overlay(bool wifi_ok, bool ai_ok)
{
    char ap_ip[20];
    char sta_ip[20];
    wifi_manager_get_ap_ip(ap_ip, sizeof(ap_ip));
    wifi_manager_get_sta_ip(sta_ip, sizeof(sta_ip));

    const char *provider = g_debbie_config.llm_provider[0]
                         ? g_debbie_config.llm_provider
                         : DEFAULT_LLM_PROVIDER;
    char line[180];
    snprintf(line, sizeof(line),
             "AP:http://%s STA:%s AI:%s(%s)",
             ap_ip,
             wifi_ok ? sta_ip : "--",
             ai_ok ? "on" : "off",
             provider);
    display_manager_set_network_info(line);
}

static const char *ssid_or_dash(const char *ssid)
{
    return (ssid && ssid[0]) ? ssid : "--";
}

static void show_network_info_in_chat(const char *phase, bool wifi_ok, bool ai_ok)
{
    char ap_ip[20];
    char sta_ip[20];
    wifi_manager_get_ap_ip(ap_ip, sizeof(ap_ip));
    wifi_manager_get_sta_ip(sta_ip, sizeof(sta_ip));

    const char *provider = g_debbie_config.llm_provider[0]
                         ? g_debbie_config.llm_provider
                         : DEFAULT_LLM_PROVIDER;

    char msg[420];
    snprintf(msg, sizeof(msg),
             "%s\n"
             "Upstream route: phone hotspot / WiFi (STA client)\n"
             "AP: http://%s\n"
             "STA IP: %s\n"
             "Saved SSIDs: %s | %s | %s\n"
             "AI: %s (%s)",
             phase ? phase : "Network status",
             ap_ip,
             wifi_ok ? sta_ip : "--",
             ssid_or_dash(g_debbie_config.wifi_ssid),
             ssid_or_dash(g_debbie_config.wifi_ssid2),
             ssid_or_dash(g_debbie_config.wifi_ssid3),
             ai_ok ? "connected" : "offline",
             provider);

    display_manager_show_text(msg);
}

/* ── App entry point ─────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "Debbie boot start");
#ifndef DEBBIE_GIT_HASH
#define DEBBIE_GIT_HASH "unknown"
#endif
    ESP_LOGI(TAG, "Build: %s", DEBBIE_GIT_HASH);

    /* 1. NVS + configuration */
    ESP_ERROR_CHECK(storage_init());

    /* 2. Display */
    bool display_ok = false;
    esp_err_t rc = ESP_FAIL;
    for (int attempt = 0; attempt < 2; ++attempt) {
        ESP_LOGI(TAG, "Attempting display_manager_init() (attempt %d)", attempt + 1);
        rc = display_manager_init();
        if (rc == ESP_OK) {
            display_ok = true;
            break;
        }
        ESP_LOGW(TAG, "display_manager_init failed (attempt %d): %d", attempt + 1, rc);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (display_ok) {
        ESP_LOGI(TAG, "display_manager_init succeeded");
        g_debbie_state = DEBBIE_STATE_BOOT;
        display_manager_set_connection_status(false, false);
        refresh_network_info_overlay(false, false);
        show_network_info_in_chat("Booting", false, false);
    } else {
        ESP_LOGE(TAG, "display_manager_init failed after retries: %s", esp_err_to_name(rc));
    }

    /* 3. Core subsystems */
    rc = memory_manager_init();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "memory_manager_init failed: %s", esp_err_to_name(rc));
    }

    rc = net_scanner_init();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "net_scanner_init failed: %s", esp_err_to_name(rc));
    }

    rc = audio_manager_init();
    if (rc == ESP_OK) {
        audio_manager_set_volume(g_debbie_config.speaker_volume);
        if (display_ok) {
            show_boot_splash();
        }
    } else {
        ESP_LOGW(TAG, "audio_manager_init failed: %s", esp_err_to_name(rc));
    }

#if DEBBIE_ENABLE_CAMERA_RUNTIME
    if (g_debbie_config.camera_enabled) {
        rc = camera_manager_init();
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "camera_manager_init failed: %s", esp_err_to_name(rc));
        }
    }
#else
    if (g_debbie_config.camera_enabled) {
        ESP_LOGW(TAG, "Camera requested but runtime is disabled (DEBBIE_ENABLE_CAMERA_RUNTIME=0)");
    }
#endif

    /* 4. Connectivity + configuration services */
    rc = wifi_manager_init();
    bool wifi_ok = (rc == ESP_OK) && wifi_manager_is_connected();
    ESP_LOGI(TAG, "WiFi init result: %s (connected=%d)", esp_err_to_name(rc), wifi_ok ? 1 : 0);

    rc = web_server_start();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "web_server_start failed: %s", esp_err_to_name(rc));
    }

#if DEBBIE_ENABLE_BLE_RUNTIME
    rc = bluetooth_manager_init();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "bluetooth_manager_init: %s", esp_err_to_name(rc));
    }
#else
    if (g_debbie_config.bluetooth_enabled) {
        ESP_LOGW(TAG, "BLE requested but runtime is disabled (DEBBIE_ENABLE_BLE_RUNTIME=0)");
    } else {
        ESP_LOGI(TAG, "BLE runtime disabled (DEBBIE_ENABLE_BLE_RUNTIME=0)");
    }
#endif

    /* 5. Local input/background tasks */
    gpio_init();
    xTaskCreate(battery_task, "battery", 3072, NULL, 3, NULL);

    if (display_ok) {
        if (wifi_ok) {
            set_state(DEBBIE_STATE_CONNECTING);
            show_network_info_in_chat("WiFi connected (client mode)", true, false);
        } else {
            set_state(DEBBIE_STATE_SETUP);
            show_network_info_in_chat("Setup mode (Debbie AP fallback)", false, false);
        }
        refresh_network_info_overlay(wifi_ok, false);
    }

    /* 6. Cloud/service connections */
    if (wifi_ok) {
        display_manager_set_connection_status(true, false);

        rc = connect_openai_if_configured();
        if (rc == ESP_OK) {
            audio_manager_start_capture(on_audio_capture);
        } else if (display_ok) {
            if (strcmp(g_debbie_config.llm_provider, LLM_PROVIDER_OPENAI) == 0 &&
                !has_openai_api_key()) {
                display_manager_show_text(
                    "🔑 OpenAI API key not configured.\n"
                    "Add it in the setup portal to enable voice chat."
                );
            } else if (strcmp(g_debbie_config.llm_provider, LLM_PROVIDER_LMSTUDIO) == 0) {
                display_manager_show_text(
                    "🧪 LM Studio not connected.\n"
                    "Check local URL/model and ensure\n"
                    "LM Studio API server is running."
                );
            } else {
                display_manager_show_text(
                    "AI provider is not supported for\n"
                    "live voice runtime in this build."
                );
            }
        }

        if (g_debbie_config.notifications_enabled &&
            strlen(g_debbie_config.companion_url) > 0) {
            rc = notification_client_init(g_debbie_config.companion_url,
                                          on_notification,
                                          NULL);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "notification_client_init failed: %s", esp_err_to_name(rc));
            }
        }
    }

    bool prev_wifi = wifi_manager_is_connected();
    bool prev_ai   = openai_client_is_connected();
    int64_t last_ai_attempt_ms    = 0;
    int64_t last_notif_attempt_ms = 0;

    while (1) {
        bool wifi_now = wifi_manager_is_connected();
        bool ai_now   = openai_client_is_connected();
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (wifi_now != prev_wifi || ai_now != prev_ai) {
            ESP_LOGI(TAG, "link status: wifi=%d ai=%d", wifi_now ? 1 : 0, ai_now ? 1 : 0);
            if (display_ok) {
                display_manager_set_connection_status(wifi_now, ai_now);
                refresh_network_info_overlay(wifi_now, ai_now);
                show_network_info_in_chat("Link update", wifi_now, ai_now);
            }
            prev_wifi = wifi_now;
            prev_ai   = ai_now;
        }

        if (!wifi_now) {
            if (ai_now) {
                openai_client_disconnect();
            }
            if (display_ok && g_debbie_state != DEBBIE_STATE_SETUP) {
                set_state(DEBBIE_STATE_SETUP);
            }
        } else {
            if (!ai_now && (now_ms - last_ai_attempt_ms) > 10000) {
                last_ai_attempt_ms = now_ms;
                rc = connect_openai_if_configured();
                if (rc == ESP_OK) {
                    audio_manager_start_capture(on_audio_capture);
                }
            }

            if (g_debbie_config.notifications_enabled &&
                strlen(g_debbie_config.companion_url) > 0 &&
                !notification_client_is_connected() &&
                (now_ms - last_notif_attempt_ms) > 15000) {
                last_notif_attempt_ms = now_ms;
                rc = notification_client_init(g_debbie_config.companion_url,
                                              on_notification,
                                              NULL);
                if (rc != ESP_OK) {
                    ESP_LOGW(TAG, "notification reconnect failed: %s", esp_err_to_name(rc));
                }
            }
        }

        if (s_has_notif && bluetooth_manager_is_connected()) {
            char ble_msg[260];
            const char *sender = s_pending_notif.sender[0]
                               ? s_pending_notif.sender
                               : "Notification";
            const char *preview = s_pending_notif.preview;
            snprintf(ble_msg, sizeof(ble_msg),
                     "{\"type\":\"notif\",\"sender\":\"%s\",\"preview\":\"%s\"}",
                     sender, preview ? preview : "");
            bluetooth_manager_notify(ble_msg);
            s_has_notif = false;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
