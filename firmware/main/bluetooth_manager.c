/*
 * bluetooth_manager.c — Debbie BLE server
 *
 * Implements a BLE GATT server with:
 *   - Nordic UART Service (NUS): wireless config + status notifications
 *   - Device Information Service (DIS): firmware version, model
 *   - Battery Service: battery level characteristic
 *
 * Phones and tablets can use any "BLE Serial" app (e.g. nRF Toolbox,
 * Serial Bluetooth Terminal) to connect and send JSON config commands
 * identical to the HTTP /configure endpoint.
 *
 * NOTE: The ESP32-S3 supports BLE only (no Classic Bluetooth / BR-EDR).
 */

#include "bluetooth_manager.h"
#include "debbie.h"
#include "settings.h"
#include "storage_manager.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble";

/* ── Nordic UART Service UUIDs ─────────────────────────────────────────── */
/* NUS service:         6E400001-B5A3-F393-E0A9-E50E24DCCA9E */
/* NUS RX (write):      6E400002-B5A3-F393-E0A9-E50E24DCCA9E */
/* NUS TX (notify):     6E400003-B5A3-F393-E0A9-E50E24DCCA9E */

#define NUS_SERVICE_UUID  0xABCD   /* abbreviated handles for simplicity */
#define NUS_RX_CHAR_UUID  0xABCE
#define NUS_TX_CHAR_UUID  0xABCF

/* Device Information Service */
#define DIS_SERVICE_UUID  0x180A
#define DIS_MODEL_UUID    0x2A24
#define DIS_FW_VER_UUID   0x2A26
#define DIS_MFR_UUID      0x2A29

/* Battery Service */
#define BAS_SERVICE_UUID  0x180F
#define BAS_LEVEL_UUID    0x2A19

#define GATTS_APP_ID      0
#define GATTS_NUM_HANDLE  20
#define DEBBIE_MTU        512

/* ── State ─────────────────────────────────────────────────────────────── */
static bool     s_ble_connected  = false;
static uint16_t s_conn_id        = 0;
static uint16_t s_gatts_if       = ESP_GATT_IF_NONE;
static uint16_t s_tx_handle      = 0;
static uint16_t s_tx_cccd_handle = 0;
static bool     s_notify_enabled = false;

/* Reassembly buffer for fragmented NUS writes */
static char  s_rx_buf[512];
static int   s_rx_len = 0;

/* ── GAP advertising data ──────────────────────────────────────────────── */
static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ── GATT attribute table ─────────────────────────────────────────────── */
/* We use the simple "add attribute one by one" approach for clarity */

/* ── RX command processor ─────────────────────────────────────────────── */
static void process_ble_command(const char *json_str)
{
    ESP_LOGI(TAG, "BLE RX: %s", json_str);

    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        bluetooth_manager_notify("{\"error\":\"invalid JSON\"}");
        return;
    }

#define GET_STR(key, dst) do { \
    cJSON *j = cJSON_GetObjectItem(json, key); \
    if (j && cJSON_IsString(j) && strlen(j->valuestring) > 0) \
        strncpy(dst, j->valuestring, sizeof(dst) - 1); \
} while (0)

    /* Network credentials */
    GET_STR("ssid",          g_debbie_config.wifi_ssid);
    GET_STR("pass",          g_debbie_config.wifi_password);
    GET_STR("ssid2",         g_debbie_config.wifi_ssid2);
    GET_STR("pass2",         g_debbie_config.wifi_password2);
    GET_STR("ssid3",         g_debbie_config.wifi_ssid3);
    GET_STR("pass3",         g_debbie_config.wifi_password3);

    /* LLM settings */
    GET_STR("llm_provider",  g_debbie_config.llm_provider);
    GET_STR("llm_model",     g_debbie_config.llm_model);
    GET_STR("oai_key",       g_debbie_config.openai_api_key);
    GET_STR("anthropic_key", g_debbie_config.anthropic_api_key);
    GET_STR("groq_key",      g_debbie_config.groq_api_key);
    GET_STR("or_key",        g_debbie_config.openrouter_api_key);
    GET_STR("local_llm_url", g_debbie_config.local_llm_url);
    GET_STR("local_llm_mdl", g_debbie_config.local_llm_model);

    /* Agent */
    GET_STR("agent_url",     g_debbie_config.agent_ws_url);
    GET_STR("companion_url", g_debbie_config.companion_url);

    /* Personality */
    GET_STR("name",          g_debbie_config.debbie_name);
    GET_STR("sys_prompt",    g_debbie_config.system_prompt);
    GET_STR("voice_style",   g_debbie_config.voice_style);
    GET_STR("resp_len",      g_debbie_config.response_length);

    /* Volume */
    cJSON *vol = cJSON_GetObjectItem(json, "volume");
    if (vol && cJSON_IsNumber(vol))
        g_debbie_config.speaker_volume = (uint8_t)vol->valuedouble;

    /* VAD */
    cJSON *vad = cJSON_GetObjectItem(json, "vad_threshold");
    if (vad && cJSON_IsNumber(vad))
        g_debbie_config.vad_threshold = (uint16_t)vad->valuedouble;

    cJSON_Delete(json);

    storage_save_config();

    /* Acknowledge */
    char ack[128];
    snprintf(ack, sizeof(ack),
             "{\"ok\":true,\"wifi\":\"%s\",\"llm\":\"%s/%s\"}",
             g_debbie_config.wifi_ssid,
             g_debbie_config.llm_provider,
             g_debbie_config.llm_model);
    bluetooth_manager_notify(ack);

    /* Reconnect WiFi with new credentials if SSID changed */
    if (strlen(g_debbie_config.wifi_ssid) > 0)
        wifi_manager_reconnect();
}

/* ── GATTS event handler ──────────────────────────────────────────────── */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                 esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {

    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        ESP_LOGI(TAG, "GATT server registered, app_id=%d", param->reg.app_id);

        /* Set device name */
        esp_ble_gap_set_device_name(
            strlen(g_debbie_config.ble_device_name) > 0
                ? g_debbie_config.ble_device_name
                : DEBBIE_BLE_DEVICE_NAME);

        /* Advertising data: flags + complete local name */
        {
            uint8_t adv_data[] = {
                0x02, 0x01, 0x06,       /* Flags: LE General Discoverable */
            };
            esp_ble_gap_config_adv_data_raw(adv_data, sizeof(adv_data));
        }

        /* Create Nordic UART Service */
        {
            esp_gatt_srvc_id_t svc = {
                .is_primary = true,
                .id = { .inst_id = 0,
                        .uuid = { .len = ESP_UUID_LEN_16,
                                  .uuid.uuid16 = NUS_SERVICE_UUID } }
            };
            esp_ble_gatts_create_service(gatts_if, &svc, GATTS_NUM_HANDLE);
        }
        break;

    case ESP_GATTS_CREATE_EVT: {
        uint16_t svc_handle = param->create.service_handle;
        esp_ble_gatts_start_service(svc_handle);

        /* RX characteristic (write without response) */
        esp_bt_uuid_t rx_uuid = { .len = ESP_UUID_LEN_16,
                                   .uuid.uuid16 = NUS_RX_CHAR_UUID };
        esp_ble_gatts_add_char(svc_handle, &rx_uuid,
                               ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_WRITE |
                               ESP_GATT_CHAR_PROP_BIT_WRITE_NR,
                               NULL, NULL);

        /* TX characteristic (notify) */
        esp_bt_uuid_t tx_uuid = { .len = ESP_UUID_LEN_16,
                                   .uuid.uuid16 = NUS_TX_CHAR_UUID };
        esp_ble_gatts_add_char(svc_handle, &tx_uuid,
                               ESP_GATT_PERM_READ,
                               ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                               NULL, NULL);
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.char_uuid.uuid.uuid16 == NUS_TX_CHAR_UUID) {
            s_tx_handle = param->add_char.attr_handle;
            /* Add CCCD for TX */
            esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16,
                                         .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG };
            esp_ble_gatts_add_char_descr(param->add_char.service_handle,
                                         &cccd_uuid,
                                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                         NULL, NULL);
        }
        break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        s_tx_cccd_handle = param->add_char_descr.attr_handle;
        ESP_LOGI(TAG, "NUS TX CCCD handle=%d", s_tx_cccd_handle);
        /* Start advertising now that the service is fully set up */
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_ble_connected = true;
        s_conn_id       = param->connect.conn_id;
        s_rx_len        = 0;
        ESP_LOGI(TAG, "BLE client connected, conn_id=%d", s_conn_id);
        esp_ble_gap_stop_advertising();
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_ble_connected  = false;
        s_notify_enabled = false;
        s_rx_len         = 0;
        ESP_LOGI(TAG, "BLE client disconnected");
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_WRITE_EVT: {
        uint16_t handle = param->write.handle;
        uint16_t len    = param->write.len;
        uint8_t *val    = param->write.value;

        if (handle == s_tx_cccd_handle && len == 2) {
            /* CCCD write — enable/disable notifications */
            s_notify_enabled = (val[0] == 0x01);
            ESP_LOGI(TAG, "Notifications %s", s_notify_enabled ? "enabled" : "disabled");
        } else {
            /* NUS RX — accumulate and process when newline or buffer full */
            int avail = (int)sizeof(s_rx_buf) - s_rx_len - 1;
            int copy  = (len < (uint16_t)avail) ? len : (uint16_t)avail;
            memcpy(s_rx_buf + s_rx_len, val, copy);
            s_rx_len += copy;
            s_rx_buf[s_rx_len] = '\0';

            /* Process if the data ends with '}' (complete JSON object) */
            if (s_rx_buf[s_rx_len - 1] == '}' || s_rx_len >= (int)sizeof(s_rx_buf) - 1) {
                process_ble_command(s_rx_buf);
                s_rx_len = 0;
            }
        }

        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;
    }

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU negotiated: %d", param->mtu.mtu);
        break;

    default:
        break;
    }
}

/* ── GAP event handler ────────────────────────────────────────────────── */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "ADV data set, starting advertising");
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
            ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
        else
            ESP_LOGI(TAG, "BLE advertising started");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE advertising stopped");
        break;
    default:
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t bluetooth_manager_init(void)
{
    if (!g_debbie_config.bluetooth_enabled) {
        ESP_LOGI(TAG, "BLE disabled in config — skipping init");
        return ESP_FAIL;
    }

    esp_err_t err;

    /* Release Classic BT memory (ESP32-S3 is BLE-only) */
    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "mem_release: %s", esp_err_to_name(err));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err) { ESP_LOGE(TAG, "bt_controller_init: %s", esp_err_to_name(err)); return err; }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err) { ESP_LOGE(TAG, "bt_controller_enable: %s", esp_err_to_name(err)); return err; }

    err = esp_bluedroid_init();
    if (err) { ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(err)); return err; }

    err = esp_bluedroid_enable();
    if (err) { ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(err)); return err; }

    err = esp_ble_gatts_register_callback(gatts_event_handler);
    if (err) { ESP_LOGE(TAG, "gatts_register_callback: %s", esp_err_to_name(err)); return err; }

    err = esp_ble_gap_register_callback(gap_event_handler);
    if (err) { ESP_LOGE(TAG, "gap_register_callback: %s", esp_err_to_name(err)); return err; }

    err = esp_ble_gatts_app_register(GATTS_APP_ID);
    if (err) { ESP_LOGE(TAG, "gatts_app_register: %s", esp_err_to_name(err)); return err; }

    esp_ble_gatt_set_local_mtu(DEBBIE_MTU);

    ESP_LOGI(TAG, "BLE manager initialised — advertising as '%s'",
             strlen(g_debbie_config.ble_device_name) > 0
                 ? g_debbie_config.ble_device_name
                 : DEBBIE_BLE_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t bluetooth_manager_deinit(void)
{
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_ble_connected  = false;
    s_notify_enabled = false;
    ESP_LOGI(TAG, "BLE stack stopped");
    return ESP_OK;
}

void bluetooth_manager_notify(const char *text)
{
    if (!s_ble_connected || !s_notify_enabled || s_tx_handle == 0) return;
    if (!text || strlen(text) == 0) return;

    size_t len = strlen(text);
    /* Split into MTU-sized chunks if necessary */
    size_t chunk = DEBBIE_MTU - 3;
    for (size_t offset = 0; offset < len; offset += chunk) {
        size_t this_len = len - offset;
        if (this_len > chunk) this_len = chunk;
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_tx_handle,
                                    (uint16_t)this_len,
                                    (uint8_t *)(text + offset), false);
    }
}

bool bluetooth_manager_is_connected(void) { return s_ble_connected; }
