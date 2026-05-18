#include "notification_client.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "notif";

#define MAX_NOTIFS 32

static esp_websocket_client_handle_t s_ws      = NULL;
static notif_cb_t                    s_cb      = NULL;
static void                         *s_ctx     = NULL;
static bool                          s_connected = false;
static SemaphoreHandle_t             s_mutex   = NULL;

static debbie_notification_t s_notifs[MAX_NOTIFS];
static int                   s_notif_count = 0;
static int                   s_unread      = 0;

/* -------------------------------------------------------------------------- */

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to companion server");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected from companion server");
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (!evt->data_ptr || evt->data_len == 0) break;
        char *msg = malloc(evt->data_len + 1);
        if (!msg) break;
        memcpy(msg, evt->data_ptr, evt->data_len);
        msg[evt->data_len] = '\0';

        cJSON *json = cJSON_Parse(msg);
        free(msg);
        if (!json) break;

        cJSON *type_j    = cJSON_GetObjectItem(json, "type");
        cJSON *sender_j  = cJSON_GetObjectItem(json, "sender");
        cJSON *preview_j = cJSON_GetObjectItem(json, "preview");

        if (type_j && cJSON_IsString(type_j)) {
            debbie_notification_t notif = { 0 };

            const char *type_str = type_j->valuestring;
            if      (strcmp(type_str, "whatsapp") == 0) notif.type = NOTIF_TYPE_WHATSAPP;
            else if (strcmp(type_str, "email")    == 0) notif.type = NOTIF_TYPE_EMAIL;
            else if (strcmp(type_str, "agent")    == 0) notif.type = NOTIF_TYPE_AGENT;
            else if (strcmp(type_str, "spotify")  == 0) notif.type = NOTIF_TYPE_SPOTIFY;
            else                                         notif.type = NOTIF_TYPE_SYSTEM;

            if (sender_j  && cJSON_IsString(sender_j))
                snprintf(notif.sender, sizeof(notif.sender), "%s", sender_j->valuestring);
            if (preview_j && cJSON_IsString(preview_j))
                snprintf(notif.preview, sizeof(notif.preview), "%s", preview_j->valuestring);

            notif.timestamp_ms = esp_timer_get_time() / 1000;
            notif.read         = false;

            /* Store in ring buffer */
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100))) {
                int idx = s_notif_count % MAX_NOTIFS;
                s_notifs[idx] = notif;
                s_notif_count++;
                s_unread++;
                xSemaphoreGive(s_mutex);
            }

            ESP_LOGI(TAG, "Notification [%s] from %s: %s",
                     type_str, notif.sender, notif.preview);

            if (s_cb) s_cb(&notif, s_ctx);
        }

        cJSON_Delete(json);
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Companion server WebSocket error");
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */

esp_err_t notification_client_init(const char *server_url,
                                   notif_cb_t cb,
                                   void *user_ctx)
{
    if (!server_url || strlen(server_url) == 0) {
        ESP_LOGW(TAG, "No companion server URL — notifications disabled");
        return ESP_ERR_INVALID_ARG;
    }

    s_cb    = cb;
    s_ctx   = user_ctx;
    s_mutex = xSemaphoreCreateMutex();

    const esp_websocket_client_config_t ws_cfg = {
        .uri                    = server_url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms   = 10000,
        .network_timeout_ms     = 5000,
        .buffer_size            = 4096,
        .task_stack             = 4096,
    };

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                    ws_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));

    ESP_LOGI(TAG, "Connecting to companion server: %s", server_url);
    return ESP_OK;
}

esp_err_t notification_client_deinit(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_connected = false;
    return ESP_OK;
}

bool notification_client_is_connected(void) { return s_connected; }

int notification_client_unread_count(void) { return s_unread; }

void notification_client_clear(void)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100))) {
        s_unread = 0;
        for (int i = 0; i < MAX_NOTIFS; i++) s_notifs[i].read = true;
        xSemaphoreGive(s_mutex);
    }
}

char *notification_client_get_summary_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200))) {
        int count = s_notif_count < MAX_NOTIFS ? s_notif_count : MAX_NOTIFS;
        for (int i = 0; i < count; i++) {
            debbie_notification_t *n = &s_notifs[i];
            if (n->read) continue;
            cJSON *obj = cJSON_CreateObject();
            const char *type_str;
            switch (n->type) {
            case NOTIF_TYPE_WHATSAPP: type_str = "whatsapp"; break;
            case NOTIF_TYPE_EMAIL:    type_str = "email";    break;
            case NOTIF_TYPE_AGENT:    type_str = "agent";    break;
            case NOTIF_TYPE_SPOTIFY:  type_str = "spotify";  break;
            default:                  type_str = "system";   break;
            }
            cJSON_AddStringToObject(obj, "type",    type_str);
            cJSON_AddStringToObject(obj, "sender",  n->sender);
            cJSON_AddStringToObject(obj, "preview", n->preview);
            cJSON_AddItemToArray(arr, obj);
        }
        xSemaphoreGive(s_mutex);
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

esp_err_t notification_client_spotify_command(const char *action)
{
    if (!s_connected || !action) return ESP_ERR_INVALID_STATE;
    cJSON *cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd, "type",   "spotify_command");
    cJSON_AddStringToObject(cmd, "action", action);
    char *str = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!str) return ESP_ERR_NO_MEM;
    esp_websocket_client_send_text(s_ws, str, strlen(str), portMAX_DELAY);
    free(str);
    return ESP_OK;
}
