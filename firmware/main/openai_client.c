/*
 * openai_client.c — OpenAI Realtime API WebSocket client
 *
 * Voice path  : ESP32 mic → PCM16/24kHz → OpenAI → PCM16/24kHz → speaker
 * Vision path : Camera JPEG → base64 → Chat Completions vision endpoint
 *
 * Protocol reference:
 *   https://platform.openai.com/docs/guides/realtime-model-capabilities
 */

#include "openai_client.h"
#include "settings.h"
#include "debbie.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "oai";

/* ── State ─────────────────────────────────────────────────────────────── */
static esp_websocket_client_handle_t s_ws      = NULL;
static oai_event_cb_t                s_cb      = NULL;
static void                         *s_ctx     = NULL;
static SemaphoreHandle_t             s_mutex   = NULL;
static bool                          s_connected = false;
static char                          s_api_key[128];
static char                          s_current_response_id[64];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Encode int16 PCM → base64 for the Realtime protocol */
static char *pcm_to_base64(const int16_t *pcm, size_t count, size_t *out_len)
{
    size_t raw_bytes = count * sizeof(int16_t);
    size_t b64_len   = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, (uint8_t *)pcm, raw_bytes);
    char *b64 = malloc(b64_len + 1);
    if (!b64) return NULL;
    mbedtls_base64_encode((unsigned char *)b64, b64_len + 1, out_len,
                          (const uint8_t *)pcm, raw_bytes);
    b64[*out_len] = '\0';
    return b64;
}

/* Decode base64 → int16 PCM */
static int16_t *base64_to_pcm(const char *b64, size_t *sample_count)
{
    size_t b64_len   = strlen(b64);
    size_t raw_bytes = 0;
    mbedtls_base64_decode(NULL, 0, &raw_bytes, (const uint8_t *)b64, b64_len);
    uint8_t *raw = malloc(raw_bytes);
    if (!raw) return NULL;
    mbedtls_base64_decode(raw, raw_bytes, &raw_bytes,
                          (const uint8_t *)b64, b64_len);
    *sample_count = raw_bytes / sizeof(int16_t);
    return (int16_t *)raw;
}

/* -------------------------------------------------------------------------- */

static void send_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str && s_ws && s_connected) {
        esp_websocket_client_send_text(s_ws, str, strlen(str), portMAX_DELAY);
    }
    free(str);
}

/* ── Session initialisation ──────────────────────────────────────────────── */

static void send_session_update(const char *system_prompt)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *sess  = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();

    /* Built-in tools Debbie exposes to the model */
    cJSON *t_cam  = cJSON_CreateObject();
    cJSON_AddStringToObject(t_cam, "type", "function");
    cJSON_AddStringToObject(t_cam, "name", "capture_image");
    cJSON_AddStringToObject(t_cam, "description",
        "Capture a photo from the device camera and describe what you see. "
        "Use when the user asks 'what do you see?', 'look at this', etc.");
    cJSON_AddItemToObject(t_cam, "parameters",
        cJSON_CreateObject()); /* no params */
    cJSON_AddItemToArray(tools, t_cam);

    cJSON *t_notif = cJSON_CreateObject();
    cJSON_AddStringToObject(t_notif, "type", "function");
    cJSON_AddStringToObject(t_notif, "name", "read_notifications");
    cJSON_AddStringToObject(t_notif, "description",
        "Read any pending WhatsApp, email, or other notifications.");
    cJSON_AddItemToObject(t_notif, "parameters", cJSON_CreateObject());
    cJSON_AddItemToArray(tools, t_notif);

    cJSON *t_spot = cJSON_CreateObject();
    cJSON_AddStringToObject(t_spot, "type", "function");
    cJSON_AddStringToObject(t_spot, "name", "spotify_control");
    cJSON_AddStringToObject(t_spot, "description",
        "Control Spotify playback — play, pause, next, previous, or search.");
    cJSON *t_spot_params = cJSON_CreateObject();
    cJSON *t_spot_props  = cJSON_CreateObject();
    cJSON *t_action      = cJSON_CreateObject();
    cJSON_AddStringToObject(t_action, "type", "string");
    cJSON_AddStringToObject(t_action, "description",
        "One of: play, pause, next, previous, search:<query>, volume:<0-100>");
    cJSON_AddItemToObject(t_spot_props, "action", t_action);
    cJSON_AddItemToObject(t_spot_params, "properties", t_spot_props);
    cJSON *required_arr = cJSON_CreateArray();
    cJSON_AddItemToArray(required_arr, cJSON_CreateString("action"));
    cJSON_AddItemToObject(t_spot_params, "required", required_arr);
    cJSON_AddStringToObject(t_spot_params, "type", "object");
    cJSON_AddItemToObject(t_spot, "parameters", t_spot_params);
    cJSON_AddItemToArray(tools, t_spot);

    cJSON_AddStringToObject(root,  "type",  "session.update");
    cJSON_AddStringToObject(sess,  "modalities", "audio");
    cJSON_AddStringToObject(sess,  "voice",  "alloy");
    cJSON_AddStringToObject(sess,  "instructions", system_prompt);
    cJSON_AddStringToObject(sess,  "input_audio_format",  "pcm16");
    cJSON_AddStringToObject(sess,  "output_audio_format", "pcm16");
    cJSON_AddBoolToObject(sess,    "turn_detection", false);  /* manual VAD */
    cJSON_AddItemToObject(sess,    "tools", tools);
    cJSON_AddStringToObject(sess,  "tool_choice", "auto");
    cJSON_AddItemToObject(root,    "session", sess);

    send_json(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Session updated with persona + tools");
}

/* ── WebSocket event handler ─────────────────────────────────────────────── */

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "WebSocket connected to OpenAI Realtime API");
        {
            oai_event_data_t evt = { .type = OAI_EVT_CONNECTED };
            if (s_cb) s_cb(&evt, s_ctx);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "WebSocket disconnected");
        {
            oai_event_data_t evt = { .type = OAI_EVT_DISCONNECTED };
            if (s_cb) s_cb(&evt, s_ctx);
        }
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (!data->data_ptr || data->data_len == 0) break;

        /* Null-terminate */
        char *msg = malloc(data->data_len + 1);
        if (!msg) break;
        memcpy(msg, data->data_ptr, data->data_len);
        msg[data->data_len] = '\0';

        cJSON *json = cJSON_Parse(msg);
        free(msg);
        if (!json) break;

        cJSON *type_j = cJSON_GetObjectItem(json, "type");
        if (!type_j || !cJSON_IsString(type_j)) { cJSON_Delete(json); break; }
        const char *type = type_j->valuestring;

        if (strcmp(type, "session.created") == 0 ||
            strcmp(type, "session.updated") == 0) {
            oai_event_data_t evt = { .type = OAI_EVT_SESSION_CREATED };
            if (s_cb) s_cb(&evt, s_ctx);
        }
        else if (strcmp(type, "response.audio.delta") == 0) {
            cJSON *delta_j = cJSON_GetObjectItem(json, "delta");
            if (delta_j && cJSON_IsString(delta_j)) {
                size_t   sample_count = 0;
                int16_t *pcm = base64_to_pcm(delta_j->valuestring, &sample_count);
                if (pcm) {
                    oai_event_data_t evt = {
                        .type       = OAI_EVT_AUDIO_DELTA,
                        .audio.pcm  = pcm,
                        .audio.count = sample_count,
                    };
                    if (s_cb) s_cb(&evt, s_ctx);
                    free(pcm);
                }
            }
        }
        else if (strcmp(type, "response.audio_transcript.delta") == 0 ||
                 strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
            cJSON *text_j = cJSON_GetObjectItem(json, "transcript");
            if (!text_j) text_j = cJSON_GetObjectItem(json, "delta");
            if (text_j && cJSON_IsString(text_j)) {
                oai_event_data_t evt = {
                    .type            = OAI_EVT_TRANSCRIPT,
                    .transcript.text = text_j->valuestring,
                };
                if (s_cb) s_cb(&evt, s_ctx);
            }
        }
        else if (strcmp(type, "response.function_call_arguments.done") == 0) {
            cJSON *name_j = cJSON_GetObjectItem(json, "name");
            cJSON *args_j = cJSON_GetObjectItem(json, "arguments");
            cJSON *id_j   = cJSON_GetObjectItem(json, "call_id");
            if (name_j && cJSON_IsString(name_j) && id_j) {
                strncpy(s_current_response_id, id_j->valuestring,
                        sizeof(s_current_response_id) - 1);
                oai_event_data_t evt = {
                    .type      = OAI_EVT_FUNCTION_CALL,
                    .fn.name   = name_j->valuestring,
                    .fn.args_json = args_j ? args_j->valuestring : "{}",
                };
                if (s_cb) s_cb(&evt, s_ctx);
            }
        }
        else if (strcmp(type, "error") == 0) {
            cJSON *err_j = cJSON_GetObjectItem(json, "error");
            cJSON *msg_j = err_j ? cJSON_GetObjectItem(err_j, "message") : NULL;
            oai_event_data_t evt = {
                .type          = OAI_EVT_ERROR,
                .error.message = msg_j ? msg_j->valuestring : "unknown error",
            };
            ESP_LOGE(TAG, "API error: %s", evt.error.message);
            if (s_cb) s_cb(&evt, s_ctx);
        }

        cJSON_Delete(json);
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t openai_client_connect(const char *api_key,
                                const char *system_prompt,
                                oai_event_cb_t cb,
                                void *user_ctx)
{
    if (!api_key || strlen(api_key) == 0) {
        ESP_LOGE(TAG, "No API key provided");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    s_cb  = cb;
    s_ctx = user_ctx;

    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    /*
     * Build the combined headers string required by the OpenAI Realtime API.
     * esp_websocket_client_config_t accepts a single multi-line header string.
     * Format: "Key1: Value1\r\nKey2: Value2\r\n"
     */
    char headers[320];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\nOpenAI-Beta: realtime=v1\r\n",
             api_key);

    const esp_websocket_client_config_t ws_cfg = {
        .uri                    = "wss://" OPENAI_REALTIME_HOST OPENAI_REALTIME_PATH,
        .headers                = headers,
        .cert_pem               = NULL,   /* Uses bundled CA certificates */
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms   = 5000,
        .network_timeout_ms     = 10000,
        .buffer_size            = 8192,
        .task_stack             = 8192,
        .task_prio              = configMAX_PRIORITIES - 3,
    };

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                    ws_event_handler, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));

    /* Wait for connection up to 10 s */
    int waited = 0;
    while (!s_connected && waited < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited++;
    }

    if (!s_connected) {
        ESP_LOGE(TAG, "Timed out waiting for WebSocket connection");
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return ESP_FAIL;
    }

    /* Configure the session */
    send_session_update(system_prompt ? system_prompt :
                        "You are Debbie, a helpful and friendly AI companion.");
    return ESP_OK;
}

esp_err_t openai_client_disconnect(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_connected = false;
    return ESP_OK;
}

esp_err_t openai_client_send_audio(const int16_t *pcm, size_t count)
{
    if (!s_connected || !pcm || count == 0) return ESP_ERR_INVALID_STATE;

    size_t  b64_len = 0;
    char   *b64     = pcm_to_base64(pcm, count, &b64_len);
    if (!b64) return ESP_ERR_NO_MEM;

    cJSON *root  = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",  "input_audio_buffer.append");
    cJSON_AddStringToObject(root, "audio", b64);
    send_json(root);
    cJSON_Delete(root);
    free(b64);
    return ESP_OK;
}

esp_err_t openai_client_commit_audio(void)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    /* Commit the audio buffer */
    cJSON *commit = cJSON_CreateObject();
    cJSON_AddStringToObject(commit, "type", "input_audio_buffer.commit");
    send_json(commit);
    cJSON_Delete(commit);

    /* Request a response */
    cJSON *respond = cJSON_CreateObject();
    cJSON_AddStringToObject(respond, "type", "response.create");
    send_json(respond);
    cJSON_Delete(respond);

    return ESP_OK;
}

esp_err_t openai_client_send_text(const char *text)
{
    if (!s_connected || !text) return ESP_ERR_INVALID_STATE;

    /* Create a conversation item with the user's text */
    cJSON *root    = cJSON_CreateObject();
    cJSON *item    = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *part    = cJSON_CreateObject();

    cJSON_AddStringToObject(part, "type", "input_text");
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(content, part);
    cJSON_AddStringToObject(item, "type",    "message");
    cJSON_AddStringToObject(item, "role",    "user");
    cJSON_AddItemToObject(item,  "content",  content);
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddItemToObject(root,   "item", item);
    send_json(root);
    cJSON_Delete(root);

    /* Request a response */
    cJSON *respond = cJSON_CreateObject();
    cJSON_AddStringToObject(respond, "type", "response.create");
    send_json(respond);
    cJSON_Delete(respond);

    return ESP_OK;
}

esp_err_t openai_client_send_image(const char *jpeg_b64, const char *prompt)
{
    if (!jpeg_b64 || !s_api_key[0]) return ESP_ERR_INVALID_ARG;

    /*
     * Vision via Chat Completions API (gpt-4o).
     * The Realtime API does not yet support direct image input, so we
     * make a separate HTTPS request and inject the description back
     * into the realtime session as a text message.
     */
    cJSON *body    = cJSON_CreateObject();
    cJSON *msgs    = cJSON_CreateArray();
    cJSON *user_m  = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();

    /* Text part */
    cJSON *txt_part = cJSON_CreateObject();
    cJSON_AddStringToObject(txt_part, "type", "text");
    cJSON_AddStringToObject(txt_part, "text",
        prompt ? prompt : "Describe what you see in this image in detail.");
    cJSON_AddItemToArray(content, txt_part);

    /* Image part */
    cJSON *img_part  = cJSON_CreateObject();
    cJSON *img_url_o = cJSON_CreateObject();
    char   data_url[32];
    snprintf(data_url, sizeof(data_url), "data:image/jpeg;base64,");
    /* Build full data URL — note: large strings may need heap allocation */
    char *full_url = malloc(strlen(data_url) + strlen(jpeg_b64) + 1);
    if (!full_url) { cJSON_Delete(body); return ESP_ERR_NO_MEM; }
    strcpy(full_url, data_url);
    strcat(full_url, jpeg_b64);
    cJSON_AddStringToObject(img_url_o, "url",    full_url);
    cJSON_AddStringToObject(img_url_o, "detail", "auto");
    cJSON_AddStringToObject(img_part, "type",      "image_url");
    cJSON_AddItemToObject(img_part,   "image_url", img_url_o);
    cJSON_AddItemToArray(content, img_part);
    free(full_url);

    cJSON_AddStringToObject(user_m, "role", "user");
    cJSON_AddItemToObject(user_m, "content", content);
    cJSON_AddItemToArray(msgs, user_m);
    cJSON_AddStringToObject(body, "model",      OPENAI_VISION_MODEL);
    cJSON_AddItemToObject(body,   "messages",   msgs);
    cJSON_AddNumberToObject(body, "max_tokens", 512);

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    /* HTTPS POST */
    char auth_hdr[160];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_api_key);

    esp_http_client_config_t http_cfg = {
        .host             = OPENAI_CHAT_HOST,
        .path             = OPENAI_CHAT_PATH,
        .transport_type   = HTTP_TRANSPORT_OVER_SSL,
        .method           = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_post_field(client, body_str, strlen(body_str));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int content_len = esp_http_client_get_content_length(client);
            char *resp_buf  = malloc(content_len + 1);
            if (resp_buf) {
                esp_http_client_read_response(client, resp_buf, content_len);
                resp_buf[content_len] = '\0';

                cJSON *resp = cJSON_Parse(resp_buf);
                if (resp) {
                    cJSON *choices  = cJSON_GetObjectItem(resp, "choices");
                    cJSON *choice0  = cJSON_GetArrayItem(choices, 0);
                    cJSON *msg      = cJSON_GetObjectItem(choice0, "message");
                    cJSON *resp_txt = cJSON_GetObjectItem(msg, "content");
                    if (resp_txt && cJSON_IsString(resp_txt)) {
                        /* Inject description into realtime conversation */
                        char desc[1024];
                        snprintf(desc, sizeof(desc),
                                 "[Camera sees]: %s", resp_txt->valuestring);
                        openai_client_send_text(desc);
                    }
                    cJSON_Delete(resp);
                }
                free(resp_buf);
            }
        } else {
            ESP_LOGW(TAG, "Vision API returned HTTP %d", status);
        }
    }

    esp_http_client_cleanup(client);
    free(body_str);
    return err;
}

esp_err_t openai_client_send_function_result(const char *call_id,
                                             const char *result_json)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    cJSON *root    = cJSON_CreateObject();
    cJSON *item    = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddStringToObject(item, "type",    "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output",  result_json);
    cJSON_AddItemToObject(root, "item", item);
    send_json(root);
    cJSON_Delete(root);

    /* Ask the model to continue with a response */
    cJSON *respond = cJSON_CreateObject();
    cJSON_AddStringToObject(respond, "type", "response.create");
    send_json(respond);
    cJSON_Delete(respond);

    return ESP_OK;
}

bool openai_client_is_connected(void) { return s_connected; }
