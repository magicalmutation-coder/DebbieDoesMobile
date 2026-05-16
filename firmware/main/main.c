/*
 * main.c — Debbie: Portable Personal AI Friend
 *
 * Freenove Media Kit ESP32-S3 (FNK0102)
 *
 * Features:
 *  ✅ Voice conversations via OpenAI Realtime API (gpt-4o-realtime-preview)
 *  ✅ Camera vision — capture and describe images using gpt-4o
 *  ✅ WhatsApp / Email / agent notification pager
 *  ✅ Spotify & Audible playback control
 *  ✅ Friendly LVGL avatar UI with animated expressions
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
#include "driver/adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char *TAG = "debbie";

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
        set_state(DEBBIE_STATE_IDLE);
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
        ESP_LOGE(TAG, "OpenAI error: %s", evt->error.message);
        display_manager_show_text("😅 Something went wrong.\nPlease try again.");
        set_state(DEBBIE_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        set_state(DEBBIE_STATE_IDLE);
        break;

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
        snprintf(msg, sizeof(msg),
                 "[System: New %s from %s: \"%s\". "
                 "Please let the user know briefly.]",
                 type_str, notif->sender, notif->preview);
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

static void button_task(void *pvParam)
{
    btn_event_t evt;
    while (1) {
        if (xQueueReceive(s_btn_queue, &evt, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50));  /* debounce */

            switch (evt) {
            case BTN_EVT_CENTER:
                /* Centre button: capture image or toggle listening */
                if (g_debbie_state == DEBBIE_STATE_IDLE ||
                    g_debbie_state == DEBBIE_STATE_LISTENING) {
                    set_state(DEBBIE_STATE_CAMERA);
                    char *b64 = NULL;
                    size_t b64_len = 0;
                    if (camera_manager_capture_base64(&b64, &b64_len) == ESP_OK && b64) {
                        display_manager_show_text("📷 What do you see, Debbie?");
                        openai_client_send_image(b64,
                            "What can you see in this photo? Describe it naturally.");
                        free(b64);
                        set_state(DEBBIE_STATE_THINKING);
                    } else {
                        set_state(DEBBIE_STATE_IDLE);
                    }
                }
                break;

            case BTN_EVT_UP:
                /* Volume up */
                if (g_debbie_config.speaker_volume < 100)
                    g_debbie_config.speaker_volume += 10;
                audio_manager_set_volume(g_debbie_config.speaker_volume);
                {
                    char msg[32];
                    snprintf(msg, sizeof(msg), "🔊 Volume: %d%%",
                             g_debbie_config.speaker_volume);
                    display_manager_show_text(msg);
                }
                break;

            case BTN_EVT_DOWN:
                /* Volume down */
                if (g_debbie_config.speaker_volume > 0)
                    g_debbie_config.speaker_volume -= 10;
                audio_manager_set_volume(g_debbie_config.speaker_volume);
                {
                    char msg[32];
                    snprintf(msg, sizeof(msg), "🔉 Volume: %d%%",
                             g_debbie_config.speaker_volume);
                    display_manager_show_text(msg);
                }
                break;

            case BTN_EVT_LONG_PRESS:
                /* Long press: read notifications aloud */
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
                }
                break;
            }
        }
    }
}

/* ── Battery monitoring task ─────────────────────────────────────────────── */

static void battery_task(void *pvParam)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_11);

    while (1) {
        int raw   = adc1_get_raw(ADC1_CHANNEL_3);
        float v   = (raw / 4095.0f) * 3.3f * BAT_DIVIDER_RATIO;
        /* Approximate Li-Ion percentage: 4.2 V = 100%, 3.2 V = 0% */
        int percent = (int)((v - 3.2f) / (4.2f - 3.2f) * 100.0f);
        if (percent > 100) percent = 100;
        if (percent < 0)   percent = 0;
        display_manager_set_battery((uint8_t)percent);
        vTaskDelay(pdMS_TO_TICKS(30000));  /* check every 30 s */
    }
}

/* ── GPIO setup ──────────────────────────────────────────────────────────── */

static void gpio_init(void)
{
    s_btn_queue = xQueueCreate(8, sizeof(btn_event_t));

    gpio_config_t io_cfg = {
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };

    io_cfg.pin_bit_mask = (1ULL << NAV_CENTER);
    gpio_config(&io_cfg);
    io_cfg.pin_bit_mask = (1ULL << NAV_UP);
    gpio_config(&io_cfg);
    io_cfg.pin_bit_mask = (1ULL << NAV_DOWN);
    gpio_config(&io_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(NAV_CENTER, gpio_isr_handler, (void *)BTN_EVT_CENTER);
    gpio_isr_handler_add(NAV_UP,     gpio_isr_handler, (void *)BTN_EVT_UP);
    gpio_isr_handler_add(NAV_DOWN,   gpio_isr_handler, (void *)BTN_EVT_DOWN);

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

/* ── App entry point ─────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════╗");
    ESP_LOGI(TAG, "║     Debbie — v1.0.0          ║");
    ESP_LOGI(TAG, "║  Portable Personal AI Friend  ║");
    ESP_LOGI(TAG, "╚══════════════════════════════╝");

    /* 1. NVS + configuration */
    ESP_ERROR_CHECK(storage_init());

    /* 1b. Memory manager — loads persisted facts & recent turns from NVS */
    memory_manager_init();

    /* 2. Display — show boot splash as early as possible */
    ESP_ERROR_CHECK(display_manager_init());
    set_state(DEBBIE_STATE_BOOT);
    show_boot_splash();

    /* 3. Audio */
    ESP_ERROR_CHECK(audio_manager_init());
    audio_manager_set_volume(g_debbie_config.speaker_volume);

    /* 4. Camera */
    if (g_debbie_config.camera_enabled) {
        esp_err_t cam_err = camera_manager_init();
        if (cam_err != ESP_OK) {
            ESP_LOGW(TAG, "Camera init failed — camera features disabled");
            g_debbie_config.camera_enabled = false;
        }
    }

    /* 5. GPIO / buttons */
    gpio_init();

    /* 6. WiFi */
    set_state(DEBBIE_STATE_CONNECTING);
    display_manager_show_text("📶 Connecting to WiFi...");
    display_manager_set_connection_status(false, false);

    esp_err_t wifi_err = wifi_manager_init();

    /* 7. Bluetooth (BLE) — start alongside WiFi */
    bluetooth_manager_init();  /* silently skips if bt disabled in config */

    /* 8. Always start the web server (for setup and status) */
    ESP_ERROR_CHECK(web_server_start());

    /* 9. Network scanner — init after WiFi (even before connecting) */
    ESP_ERROR_CHECK(net_scanner_init());

    if (wifi_err != ESP_OK) {
        /* No WiFi — enter setup mode */
        set_state(DEBBIE_STATE_SETUP);
        display_manager_show_text(
            "⚙️ Setup needed!\n\n"
            "Connect to WiFi: \"Debbie\"\n"
            "Then visit: 192.168.4.1\n"
            "to configure your settings.");
        ESP_LOGI(TAG, "No WiFi — in setup mode at %s", DEBBIE_AP_IP);

        /* Stay in setup mode loop */
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    display_manager_set_connection_status(true, false);
    display_manager_show_text("✅ Connected!\nConnecting to AI...");

    /* 9. Companion server notifications */
    if (g_debbie_config.notifications_enabled &&
        strlen(g_debbie_config.companion_url) > 0) {
        esp_err_t notif_err = notification_client_init(
            g_debbie_config.companion_url,
            on_notification, NULL);
        if (notif_err == ESP_OK) {
            ESP_LOGI(TAG, "Notification client connecting to %s",
                     g_debbie_config.companion_url);
        }
    } else {
        ESP_LOGI(TAG, "Companion server not configured — notifications disabled");
    }

    /* 10. Battery monitor */
    xTaskCreate(battery_task, "battery", 2048, NULL, 1, NULL);

    /* 11. Connect to AI (inject memory context into system prompt) */
    if (strlen(g_debbie_config.openai_api_key) > 0) {
        /* Build memory-enriched system prompt */
        char *enriched_prompt = memory_manager_enrich_prompt(
            g_debbie_config.system_prompt);
        const char *prompt_to_use = enriched_prompt
            ? enriched_prompt : g_debbie_config.system_prompt;

        esp_err_t ai_err = openai_client_connect(
            g_debbie_config.openai_api_key,
            prompt_to_use,
            on_oai_event, NULL);

        free(enriched_prompt); /* safe even if NULL */

        if (ai_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not connect to OpenAI — check API key");
            display_manager_show_text(
                "⚠️ AI connection failed.\n"
                "Check your API key at\n"
                "192.168.4.1 or say 'setup'.");
            set_state(DEBBIE_STATE_IDLE);
        }
    } else {
        ESP_LOGW(TAG, "No OpenAI API key — visit 192.168.4.1 to configure");
        display_manager_show_text(
            "👋 Hi! I'm Debbie!\n\n"
            "Please add your API key:\n"
            "Visit 192.168.4.1 in\nyour browser.\n\n"
            "Or connect me to your\nown AI agent!");
        set_state(DEBBIE_STATE_IDLE);
    }

    /* 12. Start microphone — continuous VAD listening */
    ESP_ERROR_CHECK(audio_manager_start_capture(on_audio_capture));
    ESP_LOGI(TAG, "Debbie is ready! 🎉 (Network scanner active, say 'scan my network' to start)");

    /* Main loop — lightweight, most work is in callbacks and tasks */
    while (1) {
        /* Periodic reconnect if AI disconnects */
        if (g_debbie_state != DEBBIE_STATE_SETUP &&
            wifi_manager_is_connected() &&
            !openai_client_is_connected() &&
            strlen(g_debbie_config.openai_api_key) > 0) {
            ESP_LOGI(TAG, "Reconnecting to OpenAI...");
            display_manager_show_text("Reconnecting...");
            openai_client_connect(
                g_debbie_config.openai_api_key,
                g_debbie_config.system_prompt,
                on_oai_event, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
