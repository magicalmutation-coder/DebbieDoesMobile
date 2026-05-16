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
            "You are Debbie, a warm, helpful, and friendly personal AI companion. "
            "You are running on a small portable device and love to help with daily tasks, "
            "chat, answer questions, read notifications, and more. Keep responses concise "
            "and conversational unless the user asks for detail.",
            sizeof(g_debbie_config.system_prompt) - 1);
    g_debbie_config.speaker_volume         = 75;
    g_debbie_config.notifications_enabled  = true;
    g_debbie_config.camera_enabled         = true;
    g_debbie_config.use_custom_agent       = false;
    strncpy(g_debbie_config.agent_ws_url, AGENT_WS_DEFAULT_URL,
            sizeof(g_debbie_config.agent_ws_url) - 1);

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
    NVS_GET_STR("oai_key",       g_debbie_config.openai_api_key);
    NVS_GET_STR("agent_url",     g_debbie_config.agent_ws_url);
    NVS_GET_STR("companion_url", g_debbie_config.companion_url);
    NVS_GET_STR("name",          g_debbie_config.debbie_name);
    NVS_GET_STR("sys_prompt",    g_debbie_config.system_prompt);
    NVS_GET_STR("spotify_tok",   g_debbie_config.spotify_token);
    NVS_GET_U8("volume",         g_debbie_config.speaker_volume);

    uint8_t tmp;
    if (nvs_get_u8(nvs, "use_agent", &tmp) == ESP_OK)
        g_debbie_config.use_custom_agent = (tmp != 0);
    if (nvs_get_u8(nvs, "notifs_en", &tmp) == ESP_OK)
        g_debbie_config.notifications_enabled = (tmp != 0);
    if (nvs_get_u8(nvs, "cam_en", &tmp) == ESP_OK)
        g_debbie_config.camera_enabled = (tmp != 0);

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
    NVS_SET_STR("oai_key",       g_debbie_config.openai_api_key);
    NVS_SET_STR("agent_url",     g_debbie_config.agent_ws_url);
    NVS_SET_STR("companion_url", g_debbie_config.companion_url);
    NVS_SET_STR("name",          g_debbie_config.debbie_name);
    NVS_SET_STR("sys_prompt",    g_debbie_config.system_prompt);
    NVS_SET_STR("spotify_tok",   g_debbie_config.spotify_token);
    NVS_SET_U8("volume",         g_debbie_config.speaker_volume);
    NVS_SET_U8("use_agent",      g_debbie_config.use_custom_agent ? 1 : 0);
    NVS_SET_U8("notifs_en",      g_debbie_config.notifications_enabled ? 1 : 0);
    NVS_SET_U8("cam_en",         g_debbie_config.camera_enabled ? 1 : 0);

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
