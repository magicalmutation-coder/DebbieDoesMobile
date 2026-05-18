#include "wifi_manager.h"
#include "debbie.h"
#include "settings.h"
#include "storage_manager.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_MAX_RETRY       5

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_count = 0;
static bool               s_connected   = false;
static esp_netif_t       *s_sta_netif   = NULL;
static esp_netif_t       *s_ap_netif    = NULL;

/* -------------------------------------------------------------------------- */

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            if (s_retry_count < WIFI_MAX_RETRY) {
                esp_wifi_connect();
                s_retry_count++;
                ESP_LOGI(TAG, "Retrying WiFi… (%d/%d)",
                         s_retry_count, WIFI_MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            esp_event_post(DEBBIE_EVENT_BASE, DEBBIE_EVT_WIFI_DISCONNECTED,
                           NULL, 0, portMAX_DELAY);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Client connected to Debbie AP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        s_connected   = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_event_post(DEBBIE_EVENT_BASE, DEBBIE_EVT_WIFI_CONNECTED,
                       NULL, 0, portMAX_DELAY);
    }
}

/* -------------------------------------------------------------------------- */

/**
 * @brief  Try connecting to a single SSID/password combination.
 *         Returns ESP_OK if connected within timeout, ESP_FAIL otherwise.
 */
static esp_err_t try_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_FAIL;

    ESP_LOGI(TAG, "Trying WiFi network: %s", ssid);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    /* Reconfigure can be triggered from setup portal while WiFi is still in
     * AP-only mode. Move to APSTA first so STA config/connect is valid. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t rc = esp_wifi_get_mode(&mode);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_get_mode failed: %s", esp_err_to_name(rc));
        return rc;
    }
    if (mode == WIFI_MODE_AP) {
        rc = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(rc));
            return rc;
        }
    }

    wifi_config_t sta_config = { 0 };
    strncpy((char *)sta_config.sta.ssid,     ssid,     sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    rc = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(rc));
        return rc;
    }

    rc = esp_wifi_disconnect();
    if (rc != ESP_OK && rc != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(rc));
    }

    rc = esp_wifi_connect();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(rc));
        return rc;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* -------------------------------------------------------------------------- */

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    /* AP config — always on so the config portal is reachable */
    wifi_config_t ap_config = {
        .ap = {
            .ssid            = DEBBIE_AP_SSID,
            .ssid_len        = strlen(DEBBIE_AP_SSID),
            .password        = DEBBIE_AP_PASSWORD,
            .max_connection  = 4,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };

    /* Check if any network is configured */
    bool has_network = (strlen(g_debbie_config.wifi_ssid)  > 0 ||
                        strlen(g_debbie_config.wifi_ssid2) > 0 ||
                        strlen(g_debbie_config.wifi_ssid3) > 0);

    if (has_network) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Try each saved network in priority order */
        const char *ssids[3] = {
            g_debbie_config.wifi_ssid,
            g_debbie_config.wifi_ssid2,
            g_debbie_config.wifi_ssid3,
        };
        const char *passwords[3] = {
            g_debbie_config.wifi_password,
            g_debbie_config.wifi_password2,
            g_debbie_config.wifi_password3,
        };

        for (int i = 0; i < 3; i++) {
            if (try_connect(ssids[i], passwords[i]) == ESP_OK) {
                ESP_LOGI(TAG, "Connected to WiFi: %s", ssids[i]);
                return ESP_OK;
            }
        }

        ESP_LOGW(TAG, "All saved networks failed — staying in AP+STA mode");
    } else {
        /* AP-only setup mode */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "AP-only mode — connect to '%s' and visit http://%s/",
                 DEBBIE_AP_SSID, DEBBIE_AP_IP);
    }
    return ESP_FAIL;
}

bool wifi_manager_is_connected(void) { return s_connected; }

static bool format_netif_ip(esp_netif_t *netif, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }

    buf[0] = '\0';
    if (!netif) {
        snprintf(buf, len, "--");
        return false;
    }

    esp_netif_ip_info_t ip_info = { 0 };
    esp_err_t rc = esp_netif_get_ip_info(netif, &ip_info);
    if (rc != ESP_OK || ip_info.ip.addr == 0) {
        snprintf(buf, len, "--");
        return false;
    }

    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

bool wifi_manager_get_sta_ip(char *buf, size_t len)
{
    return format_netif_ip(s_sta_netif, buf, len);
}

bool wifi_manager_get_ap_ip(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }
    if (format_netif_ip(s_ap_netif, buf, len)) {
        return true;
    }

    snprintf(buf, len, "%s", DEBBIE_AP_IP);
    return false;
}

esp_err_t wifi_manager_reconnect(void)
{
    const char *ssids[3] = {
        g_debbie_config.wifi_ssid,
        g_debbie_config.wifi_ssid2,
        g_debbie_config.wifi_ssid3,
    };
    const char *passwords[3] = {
        g_debbie_config.wifi_password,
        g_debbie_config.wifi_password2,
        g_debbie_config.wifi_password3,
    };

    for (int i = 0; i < 3; i++) {
        if (try_connect(ssids[i], passwords[i]) == ESP_OK)
            return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    strncpy(g_debbie_config.wifi_ssid,     ssid,     sizeof(g_debbie_config.wifi_ssid) - 1);
    strncpy(g_debbie_config.wifi_password, password, sizeof(g_debbie_config.wifi_password) - 1);
    storage_save_config();
    return wifi_manager_reconnect();
}

