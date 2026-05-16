#pragma once
/*
 * network_scanner.h — Ethical Network Security Scanner
 *
 * ⚠️  AUTHORISED USE ONLY ⚠️
 * These tools are intended solely for testing networks you own or have
 * explicit written authorisation to test. Unauthorised scanning or
 * probing of networks is illegal in most jurisdictions.
 *
 * Features:
 *  • WiFi network discovery (SSID, BSSID, channel, RSSI, encryption)
 *  • ARP scan — enumerate live hosts on the local subnet
 *  • TCP port scan — common service ports with banner grabbing
 *  • ICMP ping with RTT measurement
 *  • HTTP/HTTPS probe — detect web servers, titles, server headers
 *  • Service fingerprinting from banners
 *  • Results delivered via callback and stored for AI query
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_wifi_types.h"

/* ── Result types ─────────────────────────────────────────────────────────── */

typedef enum {
    SCAN_PROTO_TCP = 0,
    SCAN_PROTO_UDP,
} scan_proto_t;

typedef enum {
    PORT_STATE_OPEN = 0,
    PORT_STATE_CLOSED,
    PORT_STATE_FILTERED,
    PORT_STATE_UNKNOWN,
} port_state_t;

typedef struct {
    uint16_t     port;
    scan_proto_t proto;
    port_state_t state;
    char         service[32];     /* e.g. "http", "ssh", "telnet" */
    char         banner[128];     /* first response bytes */
    uint32_t     rtt_ms;
} scan_port_result_t;

#define MAX_OPEN_PORTS  64

typedef struct {
    uint8_t  ip[4];               /* IPv4 address */
    char     ip_str[16];
    uint8_t  mac[6];
    char     mac_str[18];
    char     hostname[64];        /* from reverse DNS / mDNS */
    char     vendor[48];          /* from MAC OUI lookup */
    uint32_t rtt_ms;              /* ping RTT */
    bool     alive;

    /* Port scan results */
    scan_port_result_t ports[MAX_OPEN_PORTS];
    int                port_count;

    /* HTTP info */
    char  http_title[128];
    char  http_server[64];
    bool  has_http;
    bool  has_https;

    /* Risk */
    int   risk_score;             /* 0–100 */
    char  risk_summary[256];      /* human-readable findings */
} host_result_t;

typedef struct {
    char           ssid[33];
    uint8_t        bssid[6];
    char           bssid_str[18];
    int8_t         rssi;
    uint8_t        channel;
    wifi_auth_mode_t auth_mode;
    char           auth_str[16];  /* "OPEN","WEP","WPA","WPA2","WPA3" */
    bool           hidden;
} wifi_ap_result_t;

#define MAX_HOSTS  64
#define MAX_APS    32

typedef struct {
    /* WiFi APs visible from this location */
    wifi_ap_result_t aps[MAX_APS];
    int              ap_count;

    /* Hosts on local subnet */
    host_result_t    hosts[MAX_HOSTS];
    int              host_count;

    /* Our own info */
    char  own_ip[16];
    char  own_mac[18];
    char  gateway_ip[16];
    char  subnet_mask[16];
    char  dns_primary[16];
    char  dns_secondary[16];

    /* Scan metadata */
    int64_t  scan_start_ms;
    int64_t  scan_duration_ms;
    bool     in_progress;
    uint8_t  progress_pct;        /* 0–100 */
} scan_results_t;

/* ── Callbacks ────────────────────────────────────────────────────────────── */

/** Called when a host is discovered during ARP scan */
typedef void (*host_found_cb_t)(const host_result_t *host, void *ctx);

/** Called when entire scan completes */
typedef void (*scan_done_cb_t)(const scan_results_t *results, void *ctx);

/** Called with progress percentage updates (0–100) */
typedef void (*scan_progress_cb_t)(uint8_t pct, const char *stage, void *ctx);

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise the scanner (call once after WiFi is connected).
 */
esp_err_t net_scanner_init(void);

/**
 * @brief  Scan all visible WiFi access points.
 *         Results are stored in the shared scan_results_t.
 */
esp_err_t net_scanner_wifi_scan(scan_progress_cb_t progress_cb, void *ctx);

/**
 * @brief  Discover all live hosts on the current subnet via ARP.
 *
 * @param progress_cb  Optional progress callback.
 * @param host_cb      Called immediately when each host responds.
 * @param ctx          User context.
 */
esp_err_t net_scanner_arp_scan(scan_progress_cb_t progress_cb,
                                host_found_cb_t    host_cb,
                                void              *ctx);

/**
 * @brief  Run a port scan against a single host.
 *         Scans a curated list of ~120 common/high-risk ports.
 *
 * @param host_ip   Dotted-decimal IPv4 string.
 * @param result    Filled in with open ports and banners.
 */
esp_err_t net_scanner_port_scan(const char    *host_ip,
                                host_result_t *result,
                                scan_progress_cb_t progress_cb,
                                void          *ctx);

/**
 * @brief  Run a full scan: WiFi → ARP → port scan all hosts → risk score.
 *         This is what the AI calls via function tool "full_network_scan".
 *
 * @param done_cb  Called when the scan finishes.
 * @param ctx      User context for callbacks.
 */
esp_err_t net_scanner_full_scan(scan_progress_cb_t progress_cb,
                                 scan_done_cb_t     done_cb,
                                 void              *ctx);

/**
 * @brief  Ping a host. Returns ESP_OK if alive.
 */
esp_err_t net_scanner_ping(const char *host_ip, uint32_t *rtt_ms);

/**
 * @brief  Get a pointer to the shared results (read-only).
 */
const scan_results_t *net_scanner_get_results(void);

/**
 * @brief  Serialise scan results to a JSON string.
 *         Caller must free() the returned pointer.
 */
char *net_scanner_results_to_json(bool include_banners);

/**
 * @brief  Get a concise human-readable summary (for AI/voice output).
 *         Caller must free() the returned pointer.
 */
char *net_scanner_get_summary(void);

/**
 * @brief  Cancel an in-progress scan.
 */
void net_scanner_cancel(void);
