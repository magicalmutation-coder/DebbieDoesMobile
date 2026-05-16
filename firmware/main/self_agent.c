/*
 * self_agent.c — Debbie's Full Agent Capabilities
 *
 * Provides Debbie with tools to:
 *  ✅ Inspect her own hardware and runtime state
 *  ✅ Query the internet (HTTP GET/POST)
 *  ✅ Execute all network security scan tools
 *  ✅ Browse web pages and extract text
 *  ✅ Perform DNS lookups
 *  ✅ Look up MAC vendors (OUI)
 *  ✅ Query the NVD CVE database
 *  ✅ Generate and serve vulnerability reports
 *
 * These capabilities are exposed to the OpenAI model as function tools
 * and registered at session startup.
 */

#include "self_agent.h"
#include "network_scanner.h"
#include "vuln_reporter.h"
#include "openai_client.h"
#include "display_manager.h"
#include "settings.h"
#include "debbie.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_idf_version.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "selfagent";

/* ── Self-introspection ──────────────────────────────────────────────────── */

char *self_agent_get_system_info(void)
{
    cJSON *root = cJSON_CreateObject();

    /* Chip info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip",     "ESP32-S3");
    cJSON_AddNumberToObject(root, "cores",    chip.cores);
    cJSON_AddStringToObject(root, "idf_ver",  esp_get_idf_version());
    cJSON_AddStringToObject(root, "fw_ver",   DEBBIE_FW_VERSION);

    /* Memory */
    size_t free_heap   = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t total_heap  = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t free_psram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    cJSON_AddNumberToObject(root, "free_heap_bytes",  free_heap);
    cJSON_AddNumberToObject(root, "total_heap_bytes", total_heap);
    cJSON_AddNumberToObject(root, "free_psram_bytes", free_psram);
    cJSON_AddNumberToObject(root, "heap_used_pct",
        (int)((1.0 - (double)free_heap / total_heap) * 100));

    /* Uptime */
    uint64_t uptime_s = esp_timer_get_time() / 1000000;
    char uptime_str[32];
    snprintf(uptime_str, sizeof(uptime_str), "%llu days %02llu:%02llu:%02llu",
             uptime_s / 86400,
             (uptime_s % 86400) / 3600,
             (uptime_s % 3600) / 60,
             uptime_s % 60);
    cJSON_AddStringToObject(root, "uptime", uptime_str);

    /* WiFi info */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(root, "wifi_ssid",    (char *)ap.ssid);
        cJSON_AddNumberToObject(root, "wifi_rssi",    ap.rssi);
        cJSON_AddNumberToObject(root, "wifi_channel", ap.primary);
    }

    /* IP info */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16], gw_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ip_info.gw));
            cJSON_AddStringToObject(root, "ip",      ip_str);
            cJSON_AddStringToObject(root, "gateway", gw_str);
        }
    }

    /* Task list (top-level summary) */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    cJSON_AddNumberToObject(root, "running_tasks", task_count);

    /* Features enabled */
    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "camera",        g_debbie_config.camera_enabled);
    cJSON_AddBoolToObject(features, "notifications", g_debbie_config.notifications_enabled);
    cJSON_AddBoolToObject(features, "network_scan",  true);
    cJSON_AddBoolToObject(features, "vuln_scanner",  true);
    cJSON_AddBoolToObject(features, "web_agent",     true);
    cJSON_AddItemToObject(root, "features", features);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* ── Internet HTTP fetch ─────────────────────────────────────────────────── */

/* Accumulate HTTP response body */
typedef struct { char *buf; int len; int cap; } http_resp_t;

static esp_err_t http_body_handler(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        int needed = r->len + evt->data_len + 1;
        if (needed > r->cap) {
            int newcap = needed + 1024;
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

char *self_agent_http_get(const char *url, int max_bytes)
{
    if (!url) return NULL;
    if (max_bytes <= 0 || max_bytes > 32768) max_bytes = 4096;

    http_resp_t resp = { .buf = malloc(1024), .len = 0, .cap = 1024 };
    if (!resp.buf) return NULL;
    resp.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = 8000,
        .event_handler  = http_body_handler,
        .user_data      = &resp,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(resp.buf); return NULL; }

    esp_http_client_set_header(cli, "User-Agent",
        "Mozilla/5.0 Debbie-Agent/1.0 (ESP32-S3)");

    esp_err_t err = esp_http_client_perform(cli);
    int status    = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status < 200 || status >= 400) {
        ESP_LOGW(TAG, "HTTP GET %s returned %d (%s)",
                 url, status, esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    /* Truncate to max_bytes */
    if (resp.len > max_bytes) {
        resp.buf[max_bytes] = '\0';
    }

    ESP_LOGD(TAG, "HTTP GET %s → %d bytes", url, resp.len);
    return resp.buf;  /* caller must free */
}

/* ── DNS lookup ──────────────────────────────────────────────────────────── */

char *self_agent_dns_lookup(const char *hostname)
{
    if (!hostname) return NULL;
    struct hostent *he = gethostbyname(hostname);
    if (!he || !he->h_addr_list[0]) return strdup("DNS lookup failed");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON *addrs = cJSON_CreateArray();
    for (int i = 0; he->h_addr_list[i]; i++) {
        struct in_addr addr;
        memcpy(&addr, he->h_addr_list[i], sizeof(addr));
        cJSON_AddItemToArray(addrs, cJSON_CreateString(inet_ntoa(addr)));
    }
    cJSON_AddItemToObject(root, "addresses", addrs);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* ── CVE lookup via NVD API (via HTTP) ───────────────────────────────────── */

char *self_agent_cve_lookup(const char *cve_id)
{
    if (!cve_id) return NULL;
    char url[128];
    snprintf(url, sizeof(url),
             "https://services.nvd.nist.gov/rest/json/cves/2.0?cveId=%s",
             cve_id);

    char *body = self_agent_http_get(url, 4096);
    if (!body) return strdup("{\"error\":\"CVE lookup failed\"}");

    /* Extract summary from NVD JSON */
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return strdup("{\"error\":\"Parse failed\"}");

    char *out = NULL;
    cJSON *vulns = cJSON_GetObjectItem(json, "vulnerabilities");
    cJSON *v0    = cJSON_GetArrayItem(vulns, 0);
    cJSON *cve   = v0 ? cJSON_GetObjectItem(v0, "cve") : NULL;

    if (cve) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "id", cve_id);

        /* Description */
        cJSON *descs = cJSON_GetObjectItem(cve, "descriptions");
        cJSON *d0    = cJSON_GetArrayItem(descs, 0);
        cJSON *value = d0 ? cJSON_GetObjectItem(d0, "value") : NULL;
        if (value && cJSON_IsString(value))
            cJSON_AddStringToObject(result, "description", value->valuestring);

        /* CVSS score */
        cJSON *metrics   = cJSON_GetObjectItem(cve, "metrics");
        cJSON *cvss31arr = cJSON_GetObjectItem(metrics, "cvssMetricV31");
        cJSON *cvss31_0  = cJSON_GetArrayItem(cvss31arr, 0);
        cJSON *cvssdata  = cvss31_0 ? cJSON_GetObjectItem(cvss31_0, "cvssData") : NULL;
        if (cvssdata) {
            cJSON *score    = cJSON_GetObjectItem(cvssdata, "baseScore");
            cJSON *severity = cJSON_GetObjectItem(cvssdata, "baseSeverity");
            if (score)    cJSON_AddNumberToObject(result, "cvss_score", score->valuedouble);
            if (severity) cJSON_AddStringToObject(result, "severity", severity->valuestring);
        }

        out = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
    } else {
        out = strdup("{\"error\":\"CVE not found\"}");
    }

    cJSON_Delete(json);
    return out;
}

/* ── Register all agent tools with OpenAI ────────────────────────────────── */

/*
 * Additional tool definitions injected into the OpenAI session.
 * These supplement the tools defined in openai_client.c.
 */
const char *AGENT_TOOLS_JSON =
"["
/* network_scan */
"{"
"  \"type\":\"function\","
"  \"name\":\"network_scan\","
"  \"description\":\"Scan the local WiFi network. Discovers all connected devices, "
"    open ports, services, and security vulnerabilities. "
"    IMPORTANT: Only use on networks you own or have explicit authorisation to test. "
"    Specify scope: 'wifi' for AP scan only, 'hosts' for device discovery, "
"    'ports' for port scan on a specific IP, 'full' for complete scan.\","
"  \"parameters\":{"
"    \"type\":\"object\","
"    \"properties\":{"
"      \"scope\":{\"type\":\"string\","
"        \"description\":\"One of: wifi, hosts, ports, full\"},"
"      \"target_ip\":{\"type\":\"string\","
"        \"description\":\"IP address for ports scope (e.g. 192.168.1.1)\"}"
"    },"
"    \"required\":[\"scope\"]"
"  }"
"},"
/* get_vuln_report */
"{"
"  \"type\":\"function\","
"  \"name\":\"get_vuln_report\","
"  \"description\":\"Get the vulnerability assessment report from the last network scan. "
"    Returns a summary of security findings, risk scores, and remediation steps.\","
"  \"parameters\":{\"type\":\"object\",\"properties\":{}}"
"},"
/* system_info */
"{"
"  \"type\":\"function\","
"  \"name\":\"system_info\","
"  \"description\":\"Get Debbie's own hardware and runtime information: "
"    CPU, memory usage, WiFi signal, uptime, enabled features.\","
"  \"parameters\":{\"type\":\"object\",\"properties\":{}}"
"},"
/* web_fetch */
"{"
"  \"type\":\"function\","
"  \"name\":\"web_fetch\","
"  \"description\":\"Fetch a URL from the internet and return the page content. "
"    Use for looking up information, checking websites, getting news, weather, etc.\","
"  \"parameters\":{"
"    \"type\":\"object\","
"    \"properties\":{"
"      \"url\":{\"type\":\"string\",\"description\":\"The full URL to fetch\"},"
"      \"max_bytes\":{\"type\":\"number\","
"        \"description\":\"Maximum response size in bytes (default 2048)\"}"
"    },"
"    \"required\":[\"url\"]"
"  }"
"},"
/* dns_lookup */
"{"
"  \"type\":\"function\","
"  \"name\":\"dns_lookup\","
"  \"description\":\"Resolve a hostname to IP addresses.\","
"  \"parameters\":{"
"    \"type\":\"object\","
"    \"properties\":{"
"      \"hostname\":{\"type\":\"string\",\"description\":\"The hostname to resolve\"}"
"    },"
"    \"required\":[\"hostname\"]"
"  }"
"},"
/* cve_lookup */
"{"
"  \"type\":\"function\","
"  \"name\":\"cve_lookup\","
"  \"description\":\"Look up details about a specific CVE (Common Vulnerability and Exposure) "
"    from the NVD database. Returns description, CVSS score, and severity.\","
"  \"parameters\":{"
"    \"type\":\"object\","
"    \"properties\":{"
"      \"cve_id\":{\"type\":\"string\","
"        \"description\":\"CVE identifier, e.g. CVE-2021-44228\"}"
"    },"
"    \"required\":[\"cve_id\"]"
"  }"
"}"
"]";

/* ── Handle agent function calls ─────────────────────────────────────────── */

/* Forward declaration */
extern void display_manager_show_text(const char *text);
extern volatile debbie_state_t g_debbie_state;
extern debbie_config_t g_debbie_config;

typedef struct {
    char call_id[64];
    char fn_name[64];
    char args_json[256];
} fn_call_task_args_t;

/* Scan progress → display */
static void scan_progress_display(uint8_t pct, const char *stage, void *ctx)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "🔍 %s\n%d%%", stage ? stage : "Scanning...", pct);
    display_manager_show_text(buf);
}

/* Scan done → AI callback */
typedef struct {
    char call_id[64];
} scan_done_ctx_t;

static void on_scan_done(const scan_results_t *results, void *ctx)
{
    scan_done_ctx_t *sdc = (scan_done_ctx_t *)ctx;

    /* Analyse vulnerabilities */
    vuln_report_t report = { 0 };
    vuln_reporter_analyse(results, &report);

    /* Build compact JSON result */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "hosts_found",    results->host_count);
    cJSON_AddNumberToObject(result, "wifi_aps",       results->ap_count);
    cJSON_AddNumberToObject(result, "vulnerabilities",report.count);
    cJSON_AddNumberToObject(result, "critical",       report.critical_count);
    cJSON_AddNumberToObject(result, "high",           report.high_count);
    cJSON_AddStringToObject(result, "own_ip",         results->own_ip);
    cJSON_AddStringToObject(result, "gateway_ip",     results->gateway_ip);

    /* Include per-host summary */
    cJSON *hosts = cJSON_CreateArray();
    for (int i = 0; i < results->host_count; i++) {
        const host_result_t *h = &results->hosts[i];
        cJSON *hobj = cJSON_CreateObject();
        cJSON_AddStringToObject(hobj, "ip",         h->ip_str);
        cJSON_AddStringToObject(hobj, "vendor",     h->vendor);
        cJSON_AddNumberToObject(hobj, "open_ports", h->port_count);
        cJSON_AddNumberToObject(hobj, "risk_score", h->risk_score);
        if (h->risk_summary[0])
            cJSON_AddStringToObject(hobj, "issues", h->risk_summary);
        cJSON_AddItemToArray(hosts, hobj);
    }
    cJSON_AddItemToObject(result, "hosts", hosts);

    char *result_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    if (result_str) {
        openai_client_send_function_result(sdc->call_id, result_str);
        free(result_str);
    }

    /* Update display with summary */
    char *voice_summary = vuln_reporter_get_voice_summary(&report);
    if (voice_summary) {
        display_manager_show_text(voice_summary);
        free(voice_summary);
    }

    free(sdc);
}

bool self_agent_handle_function_call(const char *fn_name,
                                     const char *args_json,
                                     const char *call_id)
{
    if (!fn_name || !call_id) return false;

    cJSON *args = args_json ? cJSON_Parse(args_json) : cJSON_CreateObject();

    /* ── system_info ── */
    if (strcmp(fn_name, "system_info") == 0) {
        char *info = self_agent_get_system_info();
        openai_client_send_function_result(call_id, info ? info : "{}");
        free(info);
        cJSON_Delete(args);
        return true;
    }

    /* ── web_fetch ── */
    if (strcmp(fn_name, "web_fetch") == 0) {
        cJSON *url_j     = cJSON_GetObjectItem(args, "url");
        cJSON *maxbytes_j = cJSON_GetObjectItem(args, "max_bytes");
        int max_bytes    = maxbytes_j ? (int)maxbytes_j->valuedouble : 2048;

        if (url_j && cJSON_IsString(url_j)) {
            display_manager_show_text("🌐 Fetching URL...");
            char *body = self_agent_http_get(url_j->valuestring, max_bytes);
            if (body) {
                /* Wrap in JSON result */
                cJSON *res = cJSON_CreateObject();
                cJSON_AddStringToObject(res, "url",     url_j->valuestring);
                cJSON_AddStringToObject(res, "content", body);
                cJSON_AddNumberToObject(res, "bytes",   strlen(body));
                char *out = cJSON_PrintUnformatted(res);
                cJSON_Delete(res);
                free(body);
                openai_client_send_function_result(call_id, out ? out : "{}");
                free(out);
            } else {
                openai_client_send_function_result(call_id,
                    "{\"error\":\"Fetch failed or no content\"}");
            }
        } else {
            openai_client_send_function_result(call_id,
                "{\"error\":\"URL parameter required\"}");
        }
        cJSON_Delete(args);
        return true;
    }

    /* ── dns_lookup ── */
    if (strcmp(fn_name, "dns_lookup") == 0) {
        cJSON *host_j = cJSON_GetObjectItem(args, "hostname");
        if (host_j && cJSON_IsString(host_j)) {
            char *result = self_agent_dns_lookup(host_j->valuestring);
            openai_client_send_function_result(call_id,
                result ? result : "{\"error\":\"lookup failed\"}");
            free(result);
        } else {
            openai_client_send_function_result(call_id,
                "{\"error\":\"hostname required\"}");
        }
        cJSON_Delete(args);
        return true;
    }

    /* ── cve_lookup ── */
    if (strcmp(fn_name, "cve_lookup") == 0) {
        cJSON *cve_j = cJSON_GetObjectItem(args, "cve_id");
        if (cve_j && cJSON_IsString(cve_j)) {
            display_manager_show_text("🔍 Looking up CVE...");
            char *result = self_agent_cve_lookup(cve_j->valuestring);
            openai_client_send_function_result(call_id,
                result ? result : "{\"error\":\"lookup failed\"}");
            free(result);
        } else {
            openai_client_send_function_result(call_id,
                "{\"error\":\"cve_id required\"}");
        }
        cJSON_Delete(args);
        return true;
    }

    /* ── network_scan ── */
    if (strcmp(fn_name, "network_scan") == 0) {
        cJSON *scope_j  = cJSON_GetObjectItem(args, "scope");
        cJSON *target_j = cJSON_GetObjectItem(args, "target_ip");
        const char *scope = scope_j && cJSON_IsString(scope_j)
                            ? scope_j->valuestring : "full";

        display_manager_show_text(
            "🔍 Network scan starting...\n"
            "⚠️ Authorised use only!\n"
            "Scanning...");

        if (strcmp(scope, "wifi") == 0) {
            net_scanner_wifi_scan(scan_progress_display, NULL);
            char *json = net_scanner_results_to_json(false);
            openai_client_send_function_result(call_id, json ? json : "{}");
            free(json);
        }
        else if (strcmp(scope, "hosts") == 0) {
            net_scanner_arp_scan(scan_progress_display, NULL, NULL);
            char *json = net_scanner_results_to_json(false);
            openai_client_send_function_result(call_id, json ? json : "{}");
            free(json);
        }
        else if (strcmp(scope, "ports") == 0 &&
                 target_j && cJSON_IsString(target_j)) {
            const scan_results_t *prev = net_scanner_get_results();
            /* Find existing host entry or create temporary one */
            host_result_t *host = NULL;
            for (int i = 0; i < prev->host_count; i++) {
                if (strcmp(prev->hosts[i].ip_str, target_j->valuestring) == 0) {
                    host = (host_result_t *)&prev->hosts[i];
                    break;
                }
            }
            host_result_t tmp_host = { 0 };
            if (!host) {
                strncpy(tmp_host.ip_str, target_j->valuestring,
                        sizeof(tmp_host.ip_str)-1);
                host = &tmp_host;
            }
            net_scanner_port_scan(target_j->valuestring, host,
                                  scan_progress_display, NULL);
            vuln_report_t report = { 0 };
            scan_results_t tmp_scan = { 0 };
            tmp_scan.host_count = 1;
            tmp_scan.hosts[0]   = *host;
            vuln_reporter_analyse(&tmp_scan, &report);
            char *vuln_json = vuln_reporter_to_json(&report);
            openai_client_send_function_result(call_id,
                vuln_json ? vuln_json : "{}");
            free(vuln_json);
        }
        else {
            /* Full scan — async */
            scan_done_ctx_t *sdc = malloc(sizeof(scan_done_ctx_t));
            if (sdc) {
                strncpy(sdc->call_id, call_id, sizeof(sdc->call_id)-1);
                net_scanner_full_scan(scan_progress_display, on_scan_done, sdc);
                /* Result sent asynchronously from on_scan_done */
            }
        }
        cJSON_Delete(args);
        return true;
    }

    /* ── get_vuln_report ── */
    if (strcmp(fn_name, "get_vuln_report") == 0) {
        const scan_results_t *results = net_scanner_get_results();
        if (results->host_count == 0) {
            openai_client_send_function_result(call_id,
                "{\"error\":\"No scan results available. Run network_scan first.\"}");
        } else {
            vuln_report_t report = { 0 };
            vuln_reporter_analyse(results, &report);
            char *json = vuln_reporter_to_json(&report);
            openai_client_send_function_result(call_id, json ? json : "{}");
            free(json);
        }
        cJSON_Delete(args);
        return true;
    }

    cJSON_Delete(args);
    return false;  /* Not handled by self_agent */
}
