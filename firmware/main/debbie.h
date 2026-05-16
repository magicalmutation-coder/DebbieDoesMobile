#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

/* ── Debbie shared types ─────────────────────────────────────────────────── */

/* Firmware version */
#define DEBBIE_FW_VERSION  "1.0.0"

typedef enum {
    DEBBIE_STATE_BOOT = 0,
    DEBBIE_STATE_SETUP,         /* captive-portal config */
    DEBBIE_STATE_CONNECTING,    /* joining WiFi */
    DEBBIE_STATE_IDLE,          /* waiting for user */
    DEBBIE_STATE_LISTENING,     /* mic active */
    DEBBIE_STATE_THINKING,      /* request sent, waiting */
    DEBBIE_STATE_SPEAKING,      /* playing TTS audio */
    DEBBIE_STATE_CAMERA,        /* camera preview / capture */
    DEBBIE_STATE_NOTIFICATION,  /* showing incoming notification */
    DEBBIE_STATE_SPOTIFY,       /* music playback control */
    DEBBIE_STATE_SCANNING,      /* network security scan in progress */
    DEBBIE_STATE_ERROR,
} debbie_state_t;

typedef enum {
    NOTIF_TYPE_WHATSAPP = 0,
    NOTIF_TYPE_EMAIL,
    NOTIF_TYPE_AGENT,
    NOTIF_TYPE_SPOTIFY,
    NOTIF_TYPE_SYSTEM,
} notif_type_t;

typedef struct {
    notif_type_t type;
    char         sender[64];
    char         preview[128];
    int64_t      timestamp_ms;
    bool         read;
} debbie_notification_t;

/* Debbie configuration stored in NVS */
typedef struct {
    char wifi_ssid[64];
    char wifi_password[64];
    char openai_api_key[128];
    char agent_ws_url[256];      /* custom agent endpoint */
    char spotify_token[512];     /* Spotify OAuth token (via companion) */
    char companion_url[256];     /* companion server base URL */
    char debbie_name[32];        /* customisable name, default "Debbie" */
    char system_prompt[512];     /* custom persona prompt */
    bool use_custom_agent;       /* use agent endpoint instead of OpenAI */
    uint8_t speaker_volume;      /* 0–100 */
    bool notifications_enabled;
    bool camera_enabled;
} debbie_config_t;

/* Event IDs published on the default event loop */
/* Custom event base for the Debbie application */
ESP_EVENT_DECLARE_BASE(DEBBIE_EVENT_BASE);

typedef enum {
    DEBBIE_EVT_STATE_CHANGE = 0,
    DEBBIE_EVT_NOTIFICATION,
    DEBBIE_EVT_WIFI_CONNECTED,
    DEBBIE_EVT_WIFI_DISCONNECTED,
    DEBBIE_EVT_AI_CONNECTED,
    DEBBIE_EVT_AI_DISCONNECTED,
    DEBBIE_EVT_CAMERA_FRAME,
    DEBBIE_EVT_BUTTON_PRESS,
    DEBBIE_EVT_SPOTIFY_UPDATE,
} debbie_event_id_t;

/* Global state (set by main, read by display/audio/etc.) */
extern volatile debbie_state_t g_debbie_state;
extern debbie_config_t         g_debbie_config;
