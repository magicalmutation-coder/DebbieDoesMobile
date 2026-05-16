#include "web_server.h"
#include "debbie.h"
#include "settings.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "camera_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";

/* -------------------------------------------------------------------------- */

/* Inline HTML for the setup portal — friendly, mobile-first design */
static const char SETUP_HTML[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Debbie Setup ✨</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}"
".hero{background:linear-gradient(135deg,#0f3460,#533483);padding:40px 20px;text-align:center}"
".hero h1{font-size:2.2rem;margin-bottom:8px}span.wave{display:inline-block;animation:wave 1.5s infinite}"
"@keyframes wave{0%,100%{transform:rotate(0)}50%{transform:rotate(20deg)}}"
".hero p{opacity:.8;font-size:1rem}"
".card{background:#12122a;border-radius:16px;padding:24px;margin:16px auto;max-width:480px;box-shadow:0 8px 32px rgba(0,0,0,.4)}"
"h2{color:#06d6a0;margin-bottom:16px;font-size:1.1rem}"
"label{display:block;margin-bottom:4px;font-size:.85rem;color:#aaa}"
"input,select,textarea{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #333;"
"background:#1a1a2e;color:#eee;font-size:.95rem;margin-bottom:12px}"
"input:focus,textarea:focus{outline:none;border-color:#06d6a0}"
"textarea{resize:vertical;min-height:80px}"
"button{width:100%;padding:12px;border-radius:8px;border:none;font-size:1rem;cursor:pointer;transition:.2s}"
".btn-primary{background:linear-gradient(90deg,#06d6a0,#5e548e);color:#fff;font-weight:700}"
".btn-danger{background:#c0392b;color:#fff;margin-top:8px}"
"button:hover{opacity:.9;transform:translateY(-1px)}"
".status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}"
".ok{background:#06d6a0}.err{background:#e74c3c}"
".msg{padding:10px;border-radius:8px;margin-top:12px;display:none}"
".msg.ok{background:#0d4d3a;color:#06d6a0;display:block}"
".msg.err{background:#4d0d0d;color:#e74c3c;display:block}"
"</style></head><body>"
"<div class='hero'>"
"<h1><span class='wave'>✨</span> Hi, I'm Debbie!</h1>"
"<p>Your portable personal AI companion — let's get set up!</p>"
"</div>"

"<div class='card'>"
"<h2>📶 Wi-Fi Connection</h2>"
"<label>Network (SSID)</label><input id='ssid' type='text' placeholder='Your WiFi name'>"
"<label>Password</label><input id='pass' type='password' placeholder='WiFi password'>"
"</div>"

"<div class='card'>"
"<h2>🤖 AI Settings</h2>"
"<label>OpenAI API Key</label>"
"<input id='oai_key' type='password' placeholder='sk-...'>"
"<label>Custom Agent WebSocket URL (optional)</label>"
"<input id='agent_url' type='text' placeholder='ws://your-agent:8080'>"
"<label>Companion Server URL (for WhatsApp, Email, Spotify)</label>"
"<input id='companion_url' type='text' placeholder='ws://192.168.1.10:3001'>"
"</div>"

"<div class='card'>"
"<h2>🎀 Debbie's Personality</h2>"
"<label>Name</label><input id='name' type='text' placeholder='Debbie' value='Debbie'>"
"<label>System Prompt (persona)</label>"
"<textarea id='sys_prompt' placeholder='You are Debbie, a warm and helpful AI companion...'></textarea>"
"<label>Speaker Volume (0–100)</label>"
"<input id='volume' type='number' min='0' max='100' value='75'>"
"</div>"

"<div class='card'>"
"<button class='btn-primary' onclick='save()'>💾 Save & Connect</button>"
"<button class='btn-danger' onclick='reset()'>🗑️ Factory Reset</button>"
"<div id='msg'></div>"
"</div>"

"<script>"
"function save(){"
"  const d={ssid:g('ssid'),pass:g('pass'),oai_key:g('oai_key'),"
"    agent_url:g('agent_url'),companion_url:g('companion_url'),"
"    name:g('name'),sys_prompt:g('sys_prompt'),volume:parseInt(g('volume'))||75};"
"  fetch('/configure',{method:'POST',headers:{'Content-Type':'application/json'},"
"    body:JSON.stringify(d)})"
"  .then(r=>r.json()).then(j=>msg(j.ok?'ok':'err',j.msg||'Done!'))"
"  .catch(e=>msg('err','Error: '+e));"
"}"
"function reset(){"
"  if(!confirm('Factory reset Debbie? All settings will be lost.'))return;"
"  fetch('/reset',{method:'POST'}).then(()=>msg('ok','Reset done. Rebooting...'));"
"}"
"function g(id){return document.getElementById(id).value;}"
"function msg(cls,txt){"
"  const el=document.getElementById('msg');"
"  el.className='msg '+cls;el.textContent=txt;"
"}"
"// Pre-fill from status"
"fetch('/status').then(r=>r.json()).then(j=>{"
"  if(j.name)document.getElementById('name').value=j.name;"
"  if(j.sys_prompt)document.getElementById('sys_prompt').value=j.sys_prompt;"
"  if(j.volume!=null)document.getElementById('volume').value=j.volume;"
"  if(j.agent_url)document.getElementById('agent_url').value=j.agent_url;"
"  if(j.companion_url)document.getElementById('companion_url').value=j.companion_url;"
"}).catch(()=>{});"
"</script></body></html>";

/* ── Handlers ──────────────────────────────────────────────────────────── */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SETUP_HTML, strlen(SETUP_HTML));
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "name",         g_debbie_config.debbie_name);
    cJSON_AddStringToObject(json, "sys_prompt",   g_debbie_config.system_prompt);
    cJSON_AddStringToObject(json, "agent_url",    g_debbie_config.agent_ws_url);
    cJSON_AddStringToObject(json, "companion_url",g_debbie_config.companion_url);
    cJSON_AddNumberToObject(json, "volume",       g_debbie_config.speaker_volume);
    cJSON_AddBoolToObject(json,   "wifi_ok",      wifi_manager_is_connected());
    cJSON_AddStringToObject(json, "state",
        g_debbie_state == DEBBIE_STATE_IDLE      ? "idle"      :
        g_debbie_state == DEBBIE_STATE_LISTENING ? "listening" :
        g_debbie_state == DEBBIE_STATE_THINKING  ? "thinking"  :
        g_debbie_state == DEBBIE_STATE_SPEAKING  ? "speaking"  : "other");

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}

static esp_err_t handler_configure(httpd_req_t *req)
{
    char *buf = malloc(req->content_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

#define GET_STR(key, dst) do { \
    cJSON *j = cJSON_GetObjectItem(json, key); \
    if (j && cJSON_IsString(j) && strlen(j->valuestring) > 0) \
        strncpy(dst, j->valuestring, sizeof(dst) - 1); \
} while (0)

    GET_STR("ssid",         g_debbie_config.wifi_ssid);
    GET_STR("pass",         g_debbie_config.wifi_password);
    GET_STR("oai_key",      g_debbie_config.openai_api_key);
    GET_STR("agent_url",    g_debbie_config.agent_ws_url);
    GET_STR("companion_url",g_debbie_config.companion_url);
    GET_STR("name",         g_debbie_config.debbie_name);
    GET_STR("sys_prompt",   g_debbie_config.system_prompt);

    cJSON *vol = cJSON_GetObjectItem(json, "volume");
    if (vol && cJSON_IsNumber(vol))
        g_debbie_config.speaker_volume = (uint8_t)vol->valuedouble;

    cJSON_Delete(json);
    storage_save_config();

    const char *resp = "{\"ok\":true,\"msg\":\"Saved! Connecting to WiFi...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    /* Reconnect with new credentials in background */
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_reconnect();

    return ESP_OK;
}

static esp_err_t handler_reset(httpd_req_t *req)
{
    storage_factory_reset();
    httpd_resp_send(req, "{\"ok\":true}", strlen("{\"ok\":true}"));
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_snapshot(httpd_req_t *req)
{
    if (!g_debbie_config.camera_enabled) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Camera disabled");
        return ESP_FAIL;
    }
    uint8_t *jpeg = NULL;
    size_t   len  = 0;
    if (camera_manager_capture_jpeg(&jpeg, &len) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (char *)jpeg, len);
    camera_manager_free_frame(jpeg);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

static httpd_handle_t s_server = NULL;

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    const httpd_uri_t routes[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = handler_root      },
        { .uri = "/status",    .method = HTTP_GET,  .handler = handler_status    },
        { .uri = "/configure", .method = HTTP_POST, .handler = handler_configure },
        { .uri = "/reset",     .method = HTTP_POST, .handler = handler_reset     },
        { .uri = "/snapshot",  .method = HTTP_GET,  .handler = handler_snapshot  },
    };

    for (int i = 0; i < (int)(sizeof(routes) / sizeof(routes[0])); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }

    ESP_LOGI(TAG, "Web server started at http://%s/", DEBBIE_AP_IP);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
