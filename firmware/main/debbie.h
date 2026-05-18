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

/* LLM provider identifiers */
#define LLM_PROVIDER_OPENAI     "openai"
#define LLM_PROVIDER_OLLAMA     "ollama"
#define LLM_PROVIDER_LMSTUDIO   "lmstudio"
#define LLM_PROVIDER_ANTHROPIC  "anthropic"
#define LLM_PROVIDER_GROQ       "groq"
#define LLM_PROVIDER_OPENROUTER "openrouter"

/* Debbie configuration stored in NVS */
typedef struct {
    /* ── Network: WiFi (up to 3 saved networks) ───────────────────────────── */
    char wifi_ssid[64];
    char wifi_password[64];
    char wifi_ssid2[64];
    char wifi_password2[64];
    char wifi_ssid3[64];
    char wifi_password3[64];

    /* ── Network: Bluetooth ────────────────────────────────────────────────── */
    bool bluetooth_enabled;
    char ble_device_name[32];    /* advertised BLE name, default = debbie_name */

    /* ── AI: provider & model ─────────────────────────────────────────────── */
    char llm_provider[32];       /* "openai" | "ollama" | "lmstudio" | "anthropic" | "groq" | "openrouter" */
    char llm_model[64];          /* e.g. "gpt-4o", "llama3", "claude-3-5-sonnet-20241022" */
    char openai_api_key[128];
    char anthropic_api_key[128];
    char groq_api_key[128];
    char openrouter_api_key[128];
    char local_llm_url[256];     /* Ollama / LM Studio base URL e.g. http://192.168.1.5:11434 */
    char local_llm_model[64];    /* model name on local server */

    /* ── AI: agent / companion ────────────────────────────────────────────── */
    char agent_ws_url[256];      /* custom agent WebSocket endpoint */
    char companion_url[256];     /* companion server base URL */
    char companion_external_api_key[128]; /* Bearer key for external API routes */
    bool use_custom_agent;       /* use agent endpoint instead of direct LLM */

    /* ── Services: Spotify ────────────────────────────────────────────────── */
    char spotify_token[512];     /* Spotify OAuth token (via companion) */

    /* ── Personality ──────────────────────────────────────────────────────── */
    char debbie_name[32];        /* customisable name, default "Debbie" */
    char system_prompt[512];     /* custom persona prompt */
    char voice_style[16];        /* "friendly" | "professional" | "playful" */
    char response_length[16];    /* "brief" | "normal" | "detailed" */

    /* ── Audio / UI ───────────────────────────────────────────────────────── */
    uint8_t speaker_volume;      /* 0–100 */
    uint16_t vad_threshold;      /* voice activity detection threshold, default 300 */
    bool camera_enabled;

    /* ── Notifications ────────────────────────────────────────────────────── */
    bool notifications_enabled;
    bool notif_whatsapp;
    bool notif_email;
    bool notif_spotify;

    /* ── Memory & RAG ─────────────────────────────────────────────────────── */
    bool    memory_enabled;       /* master on/off for conversation memory */
    bool    memory_rag_enabled;   /* query companion server for RAG retrieval */
    uint8_t memory_max_turns;     /* number of recent turns to keep (default 20) */
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
