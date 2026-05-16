/*
 * memory_manager.c — Debbie's short-term & long-term memory + RAG
 *
 * Short-term : circular buffer of recent turns in RAM.
 * Long-term  : key-value facts persisted to NVS flash.
 * RAG        : companion server queried via HTTP for richer context.
 */

#include "memory_manager.h"
#include "debbie.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "mem";

#define NVS_MEM_NS   "debbie_mem"

/* ── State ───────────────────────────────────────────────────────────────── */

/* Short-term: ring buffer */
static memory_turn_t s_turns[MEMORY_MAX_TURNS];
static int           s_turn_head  = 0;   /* next write position */
static int           s_turn_count = 0;   /* how many are populated */

/* Long-term facts */
static memory_fact_t s_facts[MEMORY_MAX_FACTS];
static int           s_fact_count = 0;

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

static void nvs_write_facts(nvs_handle_t nvs)
{
    nvs_set_u8(nvs, "mem_n_facts", (uint8_t)s_fact_count);
    char key[16];
    for (int i = 0; i < s_fact_count; i++) {
        /* Serialise each fact as compact JSON */
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "k", s_facts[i].key);
        cJSON_AddStringToObject(j, "v", s_facts[i].value);
        cJSON_AddNumberToObject(j, "i", s_facts[i].importance);
        cJSON_AddNumberToObject(j, "t", (double)s_facts[i].timestamp_ms);
        char *str = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (!str) continue;
        snprintf(key, sizeof(key), "mf%d", i);
        nvs_set_str(nvs, key, str);
        free(str);
    }
}

static void nvs_write_recent_turns(nvs_handle_t nvs)
{
    /* Persist last 5 turns so memory survives a reboot */
    int keep = s_turn_count < 5 ? s_turn_count : 5;
    nvs_set_u8(nvs, "mem_n_turns", (uint8_t)keep);
    char key[16];
    for (int i = 0; i < keep; i++) {
        int idx = ((s_turn_head - keep + i) + MEMORY_MAX_TURNS) % MEMORY_MAX_TURNS;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "r", s_turns[idx].role);
        cJSON_AddStringToObject(j, "t", s_turns[idx].text);
        cJSON_AddNumberToObject(j, "s", (double)s_turns[idx].timestamp_ms);
        char *str = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (!str) continue;
        snprintf(key, sizeof(key), "mt%d", i);
        nvs_set_str(nvs, key, str);
        free(str);
    }
}

static void nvs_read_facts(nvs_handle_t nvs)
{
    uint8_t count = 0;
    if (nvs_get_u8(nvs, "mem_n_facts", &count) != ESP_OK) return;
    if (count > MEMORY_MAX_FACTS) count = MEMORY_MAX_FACTS;

    char key[16];
    for (int i = 0; i < (int)count; i++) {
        snprintf(key, sizeof(key), "mf%d", i);
        char buf[300];
        size_t len = sizeof(buf);
        if (nvs_get_str(nvs, key, buf, &len) != ESP_OK) continue;

        cJSON *j = cJSON_Parse(buf);
        if (!j) continue;
        cJSON *k = cJSON_GetObjectItem(j, "k");
        cJSON *v = cJSON_GetObjectItem(j, "v");
        cJSON *imp = cJSON_GetObjectItem(j, "i");
        cJSON *ts  = cJSON_GetObjectItem(j, "t");
        if (k && v && cJSON_IsString(k) && cJSON_IsString(v)) {
            strncpy(s_facts[s_fact_count].key,   k->valuestring,
                    sizeof(s_facts[s_fact_count].key) - 1);
            strncpy(s_facts[s_fact_count].value, v->valuestring,
                    sizeof(s_facts[s_fact_count].value) - 1);
            s_facts[s_fact_count].importance    = imp ? (uint8_t)imp->valuedouble : 5;
            s_facts[s_fact_count].timestamp_ms  = ts  ? (int64_t)ts->valuedouble  : 0;
            s_fact_count++;
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "Loaded %d long-term facts from NVS", s_fact_count);
}

static void nvs_read_turns(nvs_handle_t nvs)
{
    uint8_t count = 0;
    if (nvs_get_u8(nvs, "mem_n_turns", &count) != ESP_OK) return;
    if (count > 5) count = 5;

    char key[16];
    for (int i = 0; i < (int)count; i++) {
        snprintf(key, sizeof(key), "mt%d", i);
        char buf[600];
        size_t len = sizeof(buf);
        if (nvs_get_str(nvs, key, buf, &len) != ESP_OK) continue;

        cJSON *j  = cJSON_Parse(buf);
        if (!j) continue;
        cJSON *r  = cJSON_GetObjectItem(j, "r");
        cJSON *t  = cJSON_GetObjectItem(j, "t");
        cJSON *ts = cJSON_GetObjectItem(j, "s");
        if (r && t && cJSON_IsString(r) && cJSON_IsString(t)) {
            strncpy(s_turns[s_turn_head].role, r->valuestring,
                    sizeof(s_turns[s_turn_head].role) - 1);
            strncpy(s_turns[s_turn_head].text, t->valuestring,
                    sizeof(s_turns[s_turn_head].text) - 1);
            s_turns[s_turn_head].timestamp_ms = ts ? (int64_t)ts->valuedouble : 0;
            s_turn_head = (s_turn_head + 1) % MEMORY_MAX_TURNS;
            s_turn_count++;
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "Loaded %d recent turns from NVS", s_turn_count);
}

/* ── HTTP helper ─────────────────────────────────────────────────────────── */

typedef struct { char *buf; int len; int cap; } http_resp_t;

static esp_err_t http_on_data(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        int needed = r->len + evt->data_len + 1;
        if (needed > r->cap) {
            int newcap = needed + 512;
            char *tmp = realloc(r->buf, newcap);
            if (!tmp) return ESP_FAIL;
            r->buf = tmp;
            r->cap = newcap;
        }
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

/* Fire-and-forget HTTP POST with a JSON body; returns true on 2xx. */
static bool http_post_json(const char *url, const char *json_body)
{
    esp_http_client_config_t cfg = {
        .url                      = url,
        .timeout_ms               = 4000,
        .method                   = HTTP_METHOD_POST,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, json_body, (int)strlen(json_body));
    esp_err_t err = esp_http_client_perform(cli);
    int status    = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    return (err == ESP_OK && status >= 200 && status < 300);
}

/* HTTP GET returning heap-allocated body, or NULL. Caller must free(). */
static char *http_get(const char *url, int max_bytes)
{
    if (max_bytes <= 0 || max_bytes > 8192) max_bytes = 2048;
    http_resp_t resp = { .buf = malloc(512), .len = 0, .cap = 512 };
    if (!resp.buf) return NULL;
    resp.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url                      = url,
        .timeout_ms               = 5000,
        .event_handler            = http_on_data,
        .user_data                = &resp,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(resp.buf); return NULL; }

    esp_err_t err = esp_http_client_perform(cli);
    int status    = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status < 200 || status >= 300) {
        free(resp.buf);
        return NULL;
    }

    /* Truncate to max_bytes */
    if (resp.len > max_bytes) {
        resp.buf[max_bytes] = '\0';
    }
    return resp.buf;
}

/* Build HTTP companion URL from base (ws://... or http://...) + path */
static void make_companion_url(char *out, size_t out_sz, const char *path)
{
    /* companion_url may be ws:// or http:// — normalise to http:// */
    const char *base = g_debbie_config.companion_url;
    if (strncmp(base, "ws://", 5) == 0) {
        snprintf(out, out_sz, "http://%s%s", base + 5, path);
    } else if (strncmp(base, "wss://", 6) == 0) {
        snprintf(out, out_sz, "https://%s%s", base + 6, path);
    } else {
        snprintf(out, out_sz, "%s%s", base, path);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t memory_manager_init(void)
{
    memset(s_turns, 0, sizeof(s_turns));
    memset(s_facts, 0, sizeof(s_facts));
    s_turn_head  = 0;
    s_turn_count = 0;
    s_fact_count = 0;

    if (!g_debbie_config.memory_enabled) {
        ESP_LOGI(TAG, "Memory disabled — skipping load");
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_MEM_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved memory — starting fresh");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    nvs_read_facts(nvs);
    nvs_read_turns(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Memory initialised: %d facts, %d recent turns",
             s_fact_count, s_turn_count);
    return ESP_OK;
}

esp_err_t memory_manager_save(void)
{
    if (!g_debbie_config.memory_enabled) return ESP_OK;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_MEM_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_write_facts(nvs);
    nvs_write_recent_turns(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t memory_manager_clear(void)
{
    memset(s_turns, 0, sizeof(s_turns));
    memset(s_facts, 0, sizeof(s_facts));
    s_turn_head  = 0;
    s_turn_count = 0;
    s_fact_count = 0;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_MEM_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Memory cleared");
    return ESP_OK;
}

void memory_manager_add_turn(const char *role, const char *text)
{
    if (!g_debbie_config.memory_enabled) return;
    if (!role || !text || strlen(text) == 0) return;

    memory_turn_t *slot = &s_turns[s_turn_head];
    strncpy(slot->role, role, sizeof(slot->role) - 1);
    strncpy(slot->text, text, sizeof(slot->text) - 1);
    slot->text[sizeof(slot->text) - 1] = '\0';
    slot->timestamp_ms = (int64_t)(esp_timer_get_time() / 1000);

    s_turn_head = (s_turn_head + 1) % MEMORY_MAX_TURNS;
    if (s_turn_count < MEMORY_MAX_TURNS) s_turn_count++;

    /* Persist asynchronously every 4 turns to avoid NVS wear */
    if (s_turn_count % 4 == 0) {
        memory_manager_save();
    }
}

const memory_turn_t *memory_manager_get_turns(void) { return s_turns; }
int  memory_manager_turn_count(void)               { return s_turn_count; }

esp_err_t memory_manager_save_fact(const char *key,
                                   const char *value,
                                   uint8_t     importance)
{
    if (!key || !value || strlen(key) == 0) return ESP_ERR_INVALID_ARG;

    /* Update existing fact if key matches */
    for (int i = 0; i < s_fact_count; i++) {
        if (strcmp(s_facts[i].key, key) == 0) {
            strncpy(s_facts[i].value, value, sizeof(s_facts[i].value) - 1);
            s_facts[i].importance   = importance;
            s_facts[i].timestamp_ms = (int64_t)(esp_timer_get_time() / 1000);
            ESP_LOGI(TAG, "Updated fact: %s = %s", key, value);
            return memory_manager_save();
        }
    }

    /* Add new fact */
    if (s_fact_count >= MEMORY_MAX_FACTS) {
        /* Evict the oldest, least important fact */
        int evict = 0;
        for (int i = 1; i < s_fact_count; i++) {
            if (s_facts[i].importance < s_facts[evict].importance)
                evict = i;
        }
        memmove(&s_facts[evict], &s_facts[evict + 1],
                sizeof(memory_fact_t) * (s_fact_count - evict - 1));
        s_fact_count--;
    }

    strncpy(s_facts[s_fact_count].key,   key,   sizeof(s_facts[s_fact_count].key)   - 1);
    strncpy(s_facts[s_fact_count].value, value, sizeof(s_facts[s_fact_count].value) - 1);
    s_facts[s_fact_count].importance   = importance;
    s_facts[s_fact_count].timestamp_ms = (int64_t)(esp_timer_get_time() / 1000);
    s_fact_count++;

    ESP_LOGI(TAG, "Saved fact [%d]: %s = %s", s_fact_count - 1, key, value);
    return memory_manager_save();
}

const memory_fact_t *memory_manager_get_facts(int *count_out)
{
    if (count_out) *count_out = s_fact_count;
    return s_facts;
}

char *memory_manager_build_context(void)
{
    if (!g_debbie_config.memory_enabled) return NULL;
    if (s_turn_count == 0 && s_fact_count == 0) return NULL;

    char *ctx = malloc(MEMORY_CONTEXT_MAX);
    if (!ctx) return NULL;
    ctx[0] = '\0';
    int pos = 0;

#define CTX_APPEND(fmt, ...) \
    do { \
        int _n = snprintf(ctx + pos, MEMORY_CONTEXT_MAX - pos, fmt, ##__VA_ARGS__); \
        if (_n > 0) pos += _n; \
    } while (0)

    CTX_APPEND("\n\n--- MEMORY ---\n");

    /* Long-term facts */
    if (s_fact_count > 0) {
        CTX_APPEND("Known facts:");
        for (int i = 0; i < s_fact_count && pos < MEMORY_CONTEXT_MAX - 64; i++) {
            CTX_APPEND(" %s=%s;", s_facts[i].key, s_facts[i].value);
        }
        CTX_APPEND("\n");
    }

    /* Recent conversation — last min(s_turn_count, 10) turns */
    if (s_turn_count > 0) {
        int show = s_turn_count < 10 ? s_turn_count : 10;
        CTX_APPEND("Recent conversation:\n");
        for (int i = 0; i < show && pos < MEMORY_CONTEXT_MAX - 128; i++) {
            int idx = ((s_turn_head - show + i) + MEMORY_MAX_TURNS) % MEMORY_MAX_TURNS;
            CTX_APPEND("  %s: %s\n", s_turns[idx].role, s_turns[idx].text);
        }
    }

    CTX_APPEND("--- END MEMORY ---\n");
    return ctx;
}

char *memory_manager_enrich_prompt(const char *base_prompt)
{
    char *ctx = memory_manager_build_context();
    if (!ctx) {
        /* No memory — just duplicate the base prompt */
        return base_prompt ? strdup(base_prompt) : NULL;
    }

    size_t base_len = base_prompt ? strlen(base_prompt) : 0;
    size_t ctx_len  = strlen(ctx);
    char  *out      = malloc(base_len + ctx_len + 4);
    if (!out) { free(ctx); return NULL; }

    if (base_prompt) {
        memcpy(out, base_prompt, base_len);
        out[base_len] = '\0';
    } else {
        out[0] = '\0';
    }
    strncat(out, ctx, ctx_len);
    free(ctx);
    return out;
}

char *memory_manager_query_rag(const char *query)
{
    if (!g_debbie_config.memory_enabled ||
        !g_debbie_config.memory_rag_enabled) return NULL;
    if (!query || strlen(query) == 0) return NULL;
    if (strlen(g_debbie_config.companion_url) == 0) return NULL;

    /* Build URL: http://<companion>/memory/query?q=<encoded_query>&limit=5 */
    char url[512];
    char companion_http[256];
    make_companion_url(companion_http, sizeof(companion_http), "");
    /* Strip trailing slash from base */
    int base_len = (int)strlen(companion_http);
    while (base_len > 0 && companion_http[base_len - 1] == '/') {
        companion_http[--base_len] = '\0';
    }

    /* Simple percent-encoding of the query (spaces → %20) */
    char encoded_query[256] = {0};
    int qi = 0, ei = 0;
    while (query[qi] && ei < (int)sizeof(encoded_query) - 4) {
        char c = query[qi++];
        if (c == ' ') {
            encoded_query[ei++] = '%';
            encoded_query[ei++] = '2';
            encoded_query[ei++] = '0';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~') {
            encoded_query[ei++] = c;
        } else {
            ei += snprintf(encoded_query + ei, sizeof(encoded_query) - ei,
                           "%%%02X", (unsigned char)c);
        }
    }

    snprintf(url, sizeof(url), "%s/memory/query?q=%s&limit=5",
             companion_http, encoded_query);

    char *body = http_get(url, 2048);
    if (!body) return NULL;

    /* The companion returns JSON: {"memories":[{"text":"..."},...]}
     * Extract into a plain text summary */
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return NULL;

    cJSON *mems = cJSON_GetObjectItem(json, "memories");
    if (!mems || !cJSON_IsArray(mems) || cJSON_GetArraySize(mems) == 0) {
        cJSON_Delete(json);
        return NULL;
    }

    char *summary = malloc(1024);
    if (!summary) { cJSON_Delete(json); return NULL; }
    summary[0] = '\0';
    strncat(summary, "Relevant memories: ", 1023);

    int n = cJSON_GetArraySize(mems);
    for (int i = 0; i < n; i++) {
        cJSON *m    = cJSON_GetArrayItem(mems, i);
        cJSON *text = cJSON_GetObjectItem(m, "text");
        if (text && cJSON_IsString(text)) {
            int rem = (int)(1023 - strlen(summary));
            if (rem > 4) {
                strncat(summary, text->valuestring, rem - 2);
                if (i < n - 1) strncat(summary, ". ", 2);
            }
        }
    }
    cJSON_Delete(json);
    return summary;
}

void memory_manager_sync_turn(const char *role, const char *text)
{
    if (!g_debbie_config.memory_enabled ||
        !g_debbie_config.memory_rag_enabled) return;
    if (strlen(g_debbie_config.companion_url) == 0) return;
    if (!role || !text || strlen(text) == 0) return;

    /* Build JSON body */
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "role", role);
    cJSON_AddStringToObject(j, "text", text);
    cJSON_AddNumberToObject(j, "ts",
        (double)(esp_timer_get_time() / 1000));
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return;

    char url[256];
    make_companion_url(url, sizeof(url), "/memory/turn");

    /* Best-effort: ignore errors — we don't want to block audio pipeline */
    http_post_json(url, body);
    free(body);
}
