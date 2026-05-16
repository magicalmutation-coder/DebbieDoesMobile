#include "storage_manager.h"
#include "debbie.h"
#include "settings.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "storage";

/* -------------------------------------------------------------------------- */

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated — erasing…");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* populate defaults */
    memset(&g_debbie_config, 0, sizeof(g_debbie_config));
    strncpy(g_debbie_config.debbie_name, "Debbie", sizeof(g_debbie_config.debbie_name) - 1);
    strncpy(g_debbie_config.system_prompt,
            DEBBIE_DEFAULT_SYSTEM_PROMPT,
            sizeof(g_debbie_config.system_prompt) - 1);
    g_debbie_config.speaker_volume         = 75;
    g_debbie_config.notifications_enabled  = true;
    g_debbie_config.notif_whatsapp         = true;
    g_debbie_config.notif_email            = true;
    g_debbie_config.notif_spotify          = true;
    g_debbie_config.camera_enabled         = true;
    g_debbie_config.use_custom_agent       = false;
    g_debbie_config.bluetooth_enabled      = DEBBIE_BLE_DEFAULT_ON;
    g_debbie_config.vad_threshold          = DEFAULT_VAD_THRESHOLD;
    strncpy(g_debbie_config.agent_ws_url, AGENT_WS_DEFAULT_URL,
            sizeof(g_debbie_config.agent_ws_url) - 1);
    strncpy(g_debbie_config.ble_device_name, DEBBIE_BLE_DEVICE_NAME,
            sizeof(g_debbie_config.ble_device_name) - 1);
    strncpy(g_debbie_config.llm_provider, DEFAULT_LLM_PROVIDER,
            sizeof(g_debbie_config.llm_provider) - 1);
    strncpy(g_debbie_config.llm_model, DEFAULT_LLM_MODEL,
            sizeof(g_debbie_config.llm_model) - 1);
    strncpy(g_debbie_config.local_llm_url, LOCAL_LLM_DEFAULT_URL,
            sizeof(g_debbie_config.local_llm_url) - 1);
    strncpy(g_debbie_config.local_llm_model, LOCAL_LLM_DEFAULT_MODEL,
            sizeof(g_debbie_config.local_llm_model) - 1);
    g_debbie_config.memory_enabled      = MEMORY_ENABLED_DEFAULT;
    g_debbie_config.memory_rag_enabled  = MEMORY_RAG_ENABLED_DEFAULT;
    g_debbie_config.memory_max_turns    = MEMORY_MAX_TURNS_DEFAULT;
    strncpy(g_debbie_config.voice_style, DEFAULT_VOICE_STYLE,
            sizeof(g_debbie_config.voice_style) - 1);
    strncpy(g_debbie_config.response_length, DEFAULT_RESPONSE_LENGTH,
            sizeof(g_debbie_config.response_length) - 1);

    /* try to load from NVS */
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config — using defaults");
        return ESP_OK;
    }
    ESP_ERROR_CHECK(err);

#define NVS_GET_STR(key, dst) do {                                      \
        size_t _len = sizeof(dst);                                      \
        nvs_get_str(nvs, key, dst, &_len);                              \
    } while (0)
#define NVS_GET_U8(key, dst) nvs_get_u8(nvs, key, &(dst))

    NVS_GET_STR("wifi_ssid",     g_debbie_config.wifi_ssid);
    NVS_GET_STR("wifi_pass",     g_debbie_config.wifi_password);
    NVS_GET_STR("wifi_ssid2",    g_debbie_config.wifi_ssid2);
    NVS_GET_STR("wifi_pass2",    g_debbie_config.wifi_password2);
    NVS_GET_STR("wifi_ssid3",    g_debbie_config.wifi_ssid3);
    NVS_GET_STR("wifi_pass3",    g_debbie_config.wifi_password3);
    NVS_GET_STR("oai_key",       g_debbie_config.openai_api_key);
    NVS_GET_STR("anthropic_key", g_debbie_config.anthropic_api_key);
    NVS_GET_STR("groq_key",      g_debbie_config.groq_api_key);
    NVS_GET_STR("or_key",        g_debbie_config.openrouter_api_key);
    NVS_GET_STR("llm_provider",  g_debbie_config.llm_provider);
    NVS_GET_STR("llm_model",     g_debbie_config.llm_model);
    NVS_GET_STR("local_llm_url", g_debbie_config.local_llm_url);
    NVS_GET_STR("local_llm_mdl", g_debbie_config.local_llm_model);
    NVS_GET_STR("agent_url",     g_debbie_config.agent_ws_url);
    NVS_GET_STR("companion_url", g_debbie_config.companion_url);
    NVS_GET_STR("name",          g_debbie_config.debbie_name);
    NVS_GET_STR("sys_prompt",    g_debbie_config.system_prompt);
    NVS_GET_STR("voice_style",   g_debbie_config.voice_style);
    NVS_GET_STR("resp_len",      g_debbie_config.response_length);
    NVS_GET_STR("ble_name",      g_debbie_config.ble_device_name);
    NVS_GET_STR("spotify_tok",   g_debbie_config.spotify_token);
    NVS_GET_U8("volume",         g_debbie_config.speaker_volume);

    uint8_t tmp;
    if (nvs_get_u8(nvs, "use_agent",  &tmp) == ESP_OK)
        g_debbie_config.use_custom_agent = (tmp != 0);
    if (nvs_get_u8(nvs, "notifs_en",  &tmp) == ESP_OK)
        g_debbie_config.notifications_enabled = (tmp != 0);
    if (nvs_get_u8(nvs, "notif_wa",   &tmp) == ESP_OK)
        g_debbie_config.notif_whatsapp = (tmp != 0);
    if (nvs_get_u8(nvs, "notif_em",   &tmp) == ESP_OK)
        g_debbie_config.notif_email = (tmp != 0);
    if (nvs_get_u8(nvs, "notif_sp",   &tmp) == ESP_OK)
        g_debbie_config.notif_spotify = (tmp != 0);
    if (nvs_get_u8(nvs, "cam_en",     &tmp) == ESP_OK)
        g_debbie_config.camera_enabled = (tmp != 0);
    if (nvs_get_u8(nvs, "bt_en",      &tmp) == ESP_OK)
        g_debbie_config.bluetooth_enabled = (tmp != 0);

    if (nvs_get_u8(nvs, "mem_en",     &tmp) == ESP_OK)
        g_debbie_config.memory_enabled = (tmp != 0);
    if (nvs_get_u8(nvs, "mem_rag",    &tmp) == ESP_OK)
        g_debbie_config.memory_rag_enabled = (tmp != 0);
    if (nvs_get_u8(nvs, "mem_turns",  &tmp) == ESP_OK)
        g_debbie_config.memory_max_turns = tmp ? tmp : MEMORY_MAX_TURNS_DEFAULT;

    uint16_t vad;
    if (nvs_get_u16(nvs, "vad_thresh", &vad) == ESP_OK)
        g_debbie_config.vad_threshold = vad;

    nvs_close(nvs);
    ESP_LOGI(TAG, "Config loaded — WiFi SSID: %s", g_debbie_config.wifi_ssid);
    return ESP_OK;
}

esp_err_t storage_save_config(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));

#define NVS_SET_STR(key, src) nvs_set_str(nvs, key, src)
#define NVS_SET_U8(key, val)  nvs_set_u8(nvs, key, val)

    NVS_SET_STR("wifi_ssid",     g_debbie_config.wifi_ssid);
    NVS_SET_STR("wifi_pass",     g_debbie_config.wifi_password);
    NVS_SET_STR("wifi_ssid2",    g_debbie_config.wifi_ssid2);
    NVS_SET_STR("wifi_pass2",    g_debbie_config.wifi_password2);
    NVS_SET_STR("wifi_ssid3",    g_debbie_config.wifi_ssid3);
    NVS_SET_STR("wifi_pass3",    g_debbie_config.wifi_password3);
    NVS_SET_STR("oai_key",       g_debbie_config.openai_api_key);
    NVS_SET_STR("anthropic_key", g_debbie_config.anthropic_api_key);
    NVS_SET_STR("groq_key",      g_debbie_config.groq_api_key);
    NVS_SET_STR("or_key",        g_debbie_config.openrouter_api_key);
    NVS_SET_STR("llm_provider",  g_debbie_config.llm_provider);
    NVS_SET_STR("llm_model",     g_debbie_config.llm_model);
    NVS_SET_STR("local_llm_url", g_debbie_config.local_llm_url);
    NVS_SET_STR("local_llm_mdl", g_debbie_config.local_llm_model);
    NVS_SET_STR("agent_url",     g_debbie_config.agent_ws_url);
    NVS_SET_STR("companion_url", g_debbie_config.companion_url);
    NVS_SET_STR("name",          g_debbie_config.debbie_name);
    NVS_SET_STR("sys_prompt",    g_debbie_config.system_prompt);
    NVS_SET_STR("voice_style",   g_debbie_config.voice_style);
    NVS_SET_STR("resp_len",      g_debbie_config.response_length);
    NVS_SET_STR("ble_name",      g_debbie_config.ble_device_name);
    NVS_SET_STR("spotify_tok",   g_debbie_config.spotify_token);
    NVS_SET_U8("volume",         g_debbie_config.speaker_volume);
    NVS_SET_U8("use_agent",      g_debbie_config.use_custom_agent ? 1 : 0);
    NVS_SET_U8("notifs_en",      g_debbie_config.notifications_enabled ? 1 : 0);
    NVS_SET_U8("notif_wa",       g_debbie_config.notif_whatsapp ? 1 : 0);
    NVS_SET_U8("notif_em",       g_debbie_config.notif_email ? 1 : 0);
    NVS_SET_U8("notif_sp",       g_debbie_config.notif_spotify ? 1 : 0);
    NVS_SET_U8("cam_en",         g_debbie_config.camera_enabled ? 1 : 0);
    NVS_SET_U8("bt_en",          g_debbie_config.bluetooth_enabled ? 1 : 0);
    NVS_SET_U8("mem_en",         g_debbie_config.memory_enabled ? 1 : 0);
    NVS_SET_U8("mem_rag",        g_debbie_config.memory_rag_enabled ? 1 : 0);
    NVS_SET_U8("mem_turns",      g_debbie_config.memory_max_turns);
    nvs_set_u16(nvs, "vad_thresh", g_debbie_config.vad_threshold);

    esp_err_t err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config saved");
    return err;
}

esp_err_t storage_factory_reset(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    esp_err_t err = nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Factory reset complete");
    return err;
}
