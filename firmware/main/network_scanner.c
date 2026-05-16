/*
 * network_scanner.c — Ethical Network Security Scanner
 *
 * ⚠️  AUTHORISED USE ONLY — Only scan networks you own or have
 *      explicit written permission to test. ⚠️
 */

#include "network_scanner.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/opt.h"
#include "ping/ping_sock.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "netscan";

/* ── Shared results buffer ────────────────────────────────────────────────── */
static scan_results_t s_results;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_cancel = false;

/* ── Common ports to probe (curated security-relevant set) ───────────────── */
typedef struct { uint16_t port; const char *service; } port_def_t;

static const port_def_t COMMON_PORTS[] = {
    /* Remote management */
    { 22,    "ssh"          },
    { 23,    "telnet"       },
    { 3389,  "rdp"          },
    { 5900,  "vnc"          },
    { 5901,  "vnc-2"        },
    { 2222,  "ssh-alt"      },
    /* Web */
    { 80,    "http"         },
    { 443,   "https"        },
    { 8080,  "http-alt"     },
    { 8443,  "https-alt"    },
    { 8888,  "http-alt2"    },
    { 7080,  "http-alt3"    },
    { 3000,  "node-http"    },
    { 4200,  "angular-dev"  },
    /* File sharing */
    { 21,    "ftp"          },
    { 20,    "ftp-data"     },
    { 445,   "smb"          },
    { 139,   "netbios"      },
    { 2049,  "nfs"          },
    /* Mail */
    { 25,    "smtp"         },
    { 587,   "smtp-sub"     },
    { 110,   "pop3"         },
    { 143,   "imap"         },
    { 993,   "imaps"        },
    { 995,   "pop3s"        },
    /* Database */
    { 3306,  "mysql"        },
    { 5432,  "postgres"     },
    { 27017, "mongodb"      },
    { 6379,  "redis"        },
    { 1433,  "mssql"        },
    { 1521,  "oracle"       },
    { 5984,  "couchdb"      },
    { 9200,  "elasticsearch"},
    /* IoT / embedded */
    { 1883,  "mqtt"         },
    { 8883,  "mqtts"        },
    { 502,   "modbus"       },
    { 102,   "s7comm"       },
    { 4840,  "opcua"        },
    { 9100,  "printer"      },
    { 515,   "lpd"          },
    /* Media / home */
    { 8096,  "jellyfin"     },
    { 32400, "plex"         },
    { 1194,  "openvpn"      },
    { 500,   "ike-vpn"      },
    /* Admin panels */
    { 8181,  "admin-alt"    },
    { 9090,  "prometheus"   },
    { 3001,  "grafana-alt"  },
    { 9000,  "portainer"    },
    { 2375,  "docker"       },
    { 2376,  "dockerssl"    },
    /* DNS / network */
    { 53,    "dns"          },
    { 67,    "dhcp"         },
    { 123,   "ntp"          },
    { 161,   "snmp"         },
    /* Misc */
    { 6667,  "irc"          },
    { 1080,  "socks"        },
    { 3128,  "squid"        },
};
#define COMMON_PORTS_COUNT (sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]))

/* ── MAC OUI vendor lookup (small embedded table) ─────────────────────────── */
typedef struct { uint8_t oui[3]; const char *vendor; } oui_entry_t;
static const oui_entry_t OUI_TABLE[] = {
    { {0xB8,0x27,0xEB}, "Raspberry Pi" },
    { {0xDC,0xA6,0x32}, "Raspberry Pi" },
    { {0xE4,0x5F,0x01}, "Raspberry Pi" },
    { {0x00,0x50,0x56}, "VMware"       },
    { {0x00,0x0C,0x29}, "VMware"       },
    { {0xAC,0x87,0xA3}, "Apple"        },
    { {0x00,0x17,0xF2}, "Apple"        },
    { {0x78,0x4F,0x43}, "Samsung"      },
    { {0x00,0x25,0xAB}, "Cisco"        },
    { {0x00,0x1A,0xA0}, "Cisco"        },
    { {0x18,0xFE,0x34}, "Espressif"    },
    { {0xAC,0xD0,0x74}, "Espressif"    },
    { {0x10,0x02,0xB5}, "Espressif"    },
    { {0x24,0x6F,0x28}, "Espressif"    },
    { {0x30,0xAE,0xA4}, "Espressif"    },
    { {0xEC,0xFA,0xBC}, "Espressif"    },
    { {0xA4,0xCF,0x12}, "Espressif"    },
    { {0x00,0x1B,0x63}, "Apple"        },
    { {0x00,0x23,0x32}, "Apple"        },
    { {0xB8,0xC7,0x5D}, "TP-Link"      },
    { {0xF4,0xEC,0x38}, "TP-Link"      },
    { {0x50,0xC7,0xBF}, "TP-Link"      },
};
#define OUI_TABLE_COUNT (sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]))

static const char *lookup_vendor(const uint8_t *mac)
{
    for (int i = 0; i < (int)OUI_TABLE_COUNT; i++) {
        if (memcmp(mac, OUI_TABLE[i].oui, 3) == 0)
            return OUI_TABLE[i].vendor;
    }
    return "Unknown";
}

/* ── Auth mode string ─────────────────────────────────────────────────────── */
static const char *auth_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    default:                      return "UNKNOWN";
    }
}

/* ── Risk scoring ─────────────────────────────────────────────────────────── */

static int score_port(uint16_t port, const char *banner)
{
    /* Ports that significantly raise the risk score */
    switch (port) {
    case 23:   return 30;  /* Telnet — plaintext, high risk */
    case 21:   return 20;  /* FTP — plaintext */
    case 2375: return 40;  /* Docker without TLS — critical */
    case 6379: return 30;  /* Redis — often unauthenticated */
    case 27017: return 25; /* MongoDB — often unauthenticated */
    case 9200: return 25;  /* Elasticsearch — often unauthenticated */
    case 502:  return 35;  /* Modbus — industrial, critical */
    case 1883: return 20;  /* MQTT — often unauthenticated */
    case 161:  return 15;  /* SNMP */
    case 445:  return 20;  /* SMB */
    case 139:  return 15;  /* NetBIOS */
    case 3389: return 15;  /* RDP */
    case 5900: return 15;  /* VNC */
    default:   return 5;
    }
}

static void compute_risk(host_result_t *host)
{
    int score = 0;
    char findings[256] = "";

    for (int i = 0; i < host->port_count; i++) {
        if (host->ports[i].state == PORT_STATE_OPEN) {
            int s = score_port(host->ports[i].port, host->ports[i].banner);
            score += s;

            if (host->ports[i].port == 23)
                strncat(findings, "Telnet exposed! ", sizeof(findings)-strlen(findings)-1);
            if (host->ports[i].port == 2375)
                strncat(findings, "Docker API exposed (critical)! ", sizeof(findings)-strlen(findings)-1);
            if (host->ports[i].port == 6379)
                strncat(findings, "Redis possibly unauthenticated. ", sizeof(findings)-strlen(findings)-1);
            if (host->ports[i].port == 21)
                strncat(findings, "FTP open (plaintext). ", sizeof(findings)-strlen(findings)-1);
            if (host->ports[i].port == 502)
                strncat(findings, "Modbus ICS protocol exposed! ", sizeof(findings)-strlen(findings)-1);
        }
    }

    if (host->has_http && !host->has_https)
        strncat(findings, "HTTP without HTTPS. ", sizeof(findings)-strlen(findings)-1);

    if (score == 0 && host->port_count > 0)
        strncat(findings, "Minimal attack surface.", sizeof(findings)-strlen(findings)-1);
    else if (score == 0)
        strncat(findings, "No open ports detected.", sizeof(findings)-strlen(findings)-1);

    host->risk_score = score > 100 ? 100 : score;
    strncpy(host->risk_summary, findings, sizeof(host->risk_summary) - 1);
}

/* ── TCP port probe ───────────────────────────────────────────────────────── */

static port_state_t tcp_probe(const char *ip, uint16_t port,
                               char *banner, size_t banner_sz,
                               uint32_t *rtt_ms)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
        return PORT_STATE_UNKNOWN;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return PORT_STATE_UNKNOWN;

    /* Non-blocking connect with timeout */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int64_t t0 = esp_timer_get_time();
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    int64_t t1 = esp_timer_get_time();

    if (rtt_ms) *rtt_ms = (uint32_t)((t1 - t0) / 1000);

    if (rc < 0) {
        close(sock);
        return PORT_STATE_CLOSED;
    }

    /* Try to grab a banner */
    if (banner && banner_sz > 1) {
        /* Send a minimal HTTP HEAD for web ports */
        if (port == 80 || port == 8080 || port == 8888 || port == 3000) {
            const char *req = "HEAD / HTTP/1.0\r\nHost: debbie\r\n\r\n";
            send(sock, req, strlen(req), 0);
        }
        int n = recv(sock, banner, banner_sz - 1, 0);
        if (n > 0) {
            banner[n] = '\0';
            /* Strip non-printable except newlines */
            for (int i = 0; i < n; i++) {
                if ((uint8_t)banner[i] < 0x20 && banner[i] != '\n' && banner[i] != '\r')
                    banner[i] = '.';
            }
            /* Truncate at first blank line */
            char *nl = strstr(banner, "\r\n\r\n");
            if (nl) *nl = '\0';
        } else {
            banner[0] = '\0';
        }
    }

    close(sock);
    return PORT_STATE_OPEN;
}

/* ── HTTP probe ───────────────────────────────────────────────────────────── */

static void http_probe(host_result_t *host)
{
    char url[64];
    static char resp_buf[512];

    for (int use_https = 0; use_https <= 1; use_https++) {
        snprintf(url, sizeof(url), "%s://%s:%d/",
                 use_https ? "https" : "http",
                 host->ip_str,
                 use_https ? 443 : 80);

        esp_http_client_config_t cfg = {
            .url              = url,
            .timeout_ms       = 3000,
            .skip_cert_common_name_check = true,
        };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) continue;

        if (esp_http_client_perform(cli) == ESP_OK) {
            int status = esp_http_client_get_status_code(cli);
            if (status > 0) {
                if (use_https) host->has_https = true;
                else           host->has_http  = true;

                /* Grab Server header */
                esp_http_client_get_header(cli, "Server",
                    host->http_server, sizeof(host->http_server) - 1);

                /* Read body to find <title> */
                int len = esp_http_client_read_response(cli, resp_buf, sizeof(resp_buf) - 1);
                if (len > 0) {
                    resp_buf[len] = '\0';
                    char *title_s = strcasestr(resp_buf, "<title>");
                    char *title_e = title_s ? strcasestr(title_s + 7, "</title>") : NULL;
                    if (title_s && title_e) {
                        int tlen = (int)(title_e - title_s - 7);
                        if (tlen > 0 && tlen < (int)sizeof(host->http_title) - 1) {
                            memcpy(host->http_title, title_s + 7, tlen);
                            host->http_title[tlen] = '\0';
                        }
                    }
                }
            }
        }
        esp_http_client_cleanup(cli);
    }
}

/* ── ARP scan ─────────────────────────────────────────────────────────────── */

extern int etharp_request(struct netif *netif, const ip4_addr_t *ipaddr);

static bool arp_ping(uint8_t *target_ip)
{
    /* Try a TCP connect to port 80 as ARP-substitute fallback.
     * A proper ARP implementation requires raw socket or lwIP internals.
     * On ESP-IDF, we use a quick TCP connect to trigger ARP, then check
     * the ARP cache via the netif. */
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
             target_ip[0], target_ip[1], target_ip[2], target_ip[3]);

    /* Quick ICMP ping via the ping socket API */
    uint32_t rtt = 0;
    esp_err_t err = net_scanner_ping(ip_str, &rtt);
    return (err == ESP_OK);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t net_scanner_init(void)
{
    memset(&s_results, 0, sizeof(s_results));
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Network scanner initialised");
    return ESP_OK;
}

/* ── WiFi scan ───────────────────────────────────────────────────────────── */

esp_err_t net_scanner_wifi_scan(scan_progress_cb_t progress_cb, void *ctx)
{
    if (progress_cb) progress_cb(0, "Scanning WiFi networks...", ctx);

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* blocking */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = MAX_APS;
    wifi_ap_record_t aps[MAX_APS];
    memset(aps, 0, sizeof(aps));

    err = esp_wifi_scan_get_ap_records(&ap_count, aps);
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000))) {
        s_results.ap_count = ap_count;
        for (int i = 0; i < ap_count; i++) {
            wifi_ap_result_t *r = &s_results.aps[i];
            strncpy(r->ssid, (char *)aps[i].ssid, sizeof(r->ssid) - 1);
            memcpy(r->bssid, aps[i].bssid, 6);
            snprintf(r->bssid_str, sizeof(r->bssid_str),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                     aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
            r->rssi     = aps[i].rssi;
            r->channel  = aps[i].primary;
            r->auth_mode = aps[i].authmode;
            strncpy(r->auth_str, auth_str(aps[i].authmode), sizeof(r->auth_str)-1);
            r->hidden   = (strlen(r->ssid) == 0);
        }
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "WiFi scan found %d APs", ap_count);
    if (progress_cb) progress_cb(100, "WiFi scan complete", ctx);
    return ESP_OK;
}

/* ── ARP / host discovery scan ───────────────────────────────────────────── */

esp_err_t net_scanner_arp_scan(scan_progress_cb_t progress_cb,
                                host_found_cb_t    host_cb,
                                void              *ctx)
{
    /* Get our IP and subnet */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return ESP_FAIL;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return ESP_FAIL;

    uint32_t base_ip  = ntohl(ip_info.ip.addr);
    uint32_t mask     = ntohl(ip_info.netmask.addr);
    uint32_t network  = base_ip & mask;
    uint32_t host_max = ~mask;

    /* Only scan /24 or smaller subnets to avoid very long scans */
    if (host_max > 254) host_max = 254;

    uint8_t own_ip[4];
    own_ip[0] = (base_ip >> 24) & 0xFF;
    own_ip[1] = (base_ip >> 16) & 0xFF;
    own_ip[2] = (base_ip >> 8)  & 0xFF;
    own_ip[3] =  base_ip        & 0xFF;
    snprintf(s_results.own_ip, sizeof(s_results.own_ip),
             "%d.%d.%d.%d", own_ip[0], own_ip[1], own_ip[2], own_ip[3]);

    uint32_t gw = ntohl(ip_info.gw.addr);
    snprintf(s_results.gateway_ip, sizeof(s_results.gateway_ip),
             "%d.%d.%d.%d",
             (gw>>24)&0xFF, (gw>>16)&0xFF, (gw>>8)&0xFF, gw&0xFF);

    snprintf(s_results.subnet_mask, sizeof(s_results.subnet_mask),
             "%d.%d.%d.%d",
             (mask>>24)&0xFF, (mask>>16)&0xFF, (mask>>8)&0xFF, mask&0xFF);

    if (progress_cb) progress_cb(0, "Discovering hosts on subnet...", ctx);

    s_results.host_count = 0;
    int found = 0;

    for (uint32_t i = 1; i <= host_max && !s_cancel; i++) {
        uint32_t target = network | i;
        uint8_t tip[4] = {
            (target >> 24) & 0xFF,
            (target >> 16) & 0xFF,
            (target >>  8) & 0xFF,
             target        & 0xFF,
        };

        uint8_t pct = (uint8_t)(i * 100 / host_max);
        if (pct % 5 == 0 && progress_cb) {
            char stage[48];
            snprintf(stage, sizeof(stage),
                     "Scanning %d.%d.%d.%d... (%d found)",
                     tip[0], tip[1], tip[2], tip[3], found);
            progress_cb(pct, stage, ctx);
        }

        if (arp_ping(tip)) {
            host_result_t host = { 0 };
            memcpy(host.ip, tip, 4);
            snprintf(host.ip_str, sizeof(host.ip_str),
                     "%d.%d.%d.%d", tip[0], tip[1], tip[2], tip[3]);
            host.alive = true;
            strncpy(host.vendor, lookup_vendor(host.mac), sizeof(host.vendor)-1);

            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200))) {
                if (s_results.host_count < MAX_HOSTS) {
                    s_results.hosts[s_results.host_count++] = host;
                }
                xSemaphoreGive(s_mutex);
            }
            found++;

            if (host_cb) host_cb(&host, ctx);
            ESP_LOGI(TAG, "Host found: %s", host.ip_str);
        }

        /* Yield every 8 hosts to keep the watchdog happy */
        if ((i & 7) == 0) vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (progress_cb) progress_cb(100, "Host discovery complete", ctx);
    ESP_LOGI(TAG, "ARP scan found %d hosts", found);
    return ESP_OK;
}

/* ── Port scan ───────────────────────────────────────────────────────────── */

esp_err_t net_scanner_port_scan(const char    *host_ip,
                                host_result_t *result,
                                scan_progress_cb_t progress_cb,
                                void          *ctx)
{
    if (!host_ip || !result) return ESP_ERR_INVALID_ARG;

    int total = (int)COMMON_PORTS_COUNT;

    for (int i = 0; i < total && !s_cancel; i++) {
        uint16_t port    = COMMON_PORTS[i].port;
        const char *svc  = COMMON_PORTS[i].service;

        if (i % 10 == 0 && progress_cb) {
            char stage[48];
            snprintf(stage, sizeof(stage), "Scanning ports... (%d/%d)", i, total);
            progress_cb((uint8_t)(i * 100 / total), stage, ctx);
        }

        char    banner[128] = "";
        uint32_t rtt        = 0;
        port_state_t state  = tcp_probe(host_ip, port, banner, sizeof(banner), &rtt);

        if (state == PORT_STATE_OPEN && result->port_count < MAX_OPEN_PORTS) {
            scan_port_result_t *pr = &result->ports[result->port_count++];
            pr->port  = port;
            pr->proto = SCAN_PROTO_TCP;
            pr->state = PORT_STATE_OPEN;
            pr->rtt_ms = rtt;
            strncpy(pr->service, svc,    sizeof(pr->service) - 1);
            strncpy(pr->banner,  banner, sizeof(pr->banner)  - 1);
            ESP_LOGI(TAG, "%s:%d open (%s)", host_ip, port, svc);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); /* pacing — don't flood */
    }

    /* HTTP-specific probe for richer info */
    http_probe(result);
    compute_risk(result);

    if (progress_cb) progress_cb(100, "Port scan complete", ctx);
    return ESP_OK;
}

/* ── Ping ─────────────────────────────────────────────────────────────────── */

esp_err_t net_scanner_ping(const char *host_ip, uint32_t *rtt_ms)
{
    if (!host_ip) return ESP_ERR_INVALID_ARG;

    /* Use lwip's ping via a TCP connect to a common port as a fallback
     * since esp_ping requires a callback infrastructure.
     * For a real ICMP ping, use esp_ping_new_session API. */
    char    banner[8] = "";
    uint32_t rtt = 0;
    port_state_t st = tcp_probe(host_ip, 80, banner, sizeof(banner), &rtt);

    if (st == PORT_STATE_OPEN) {
        if (rtt_ms) *rtt_ms = rtt;
        return ESP_OK;
    }

    /* Try port 443 */
    st = tcp_probe(host_ip, 443, banner, sizeof(banner), &rtt);
    if (st == PORT_STATE_OPEN) {
        if (rtt_ms) *rtt_ms = rtt;
        return ESP_OK;
    }

    /* Try port 22 */
    st = tcp_probe(host_ip, 22, banner, sizeof(banner), &rtt);
    if (st == PORT_STATE_OPEN) {
        if (rtt_ms) *rtt_ms = rtt;
        return ESP_OK;
    }

    return ESP_FAIL;
}

/* ── Full scan ────────────────────────────────────────────────────────────── */

typedef struct {
    scan_progress_cb_t progress_cb;
    scan_done_cb_t     done_cb;
    void              *ctx;
} full_scan_args_t;

static void full_scan_task(void *pvParam)
{
    full_scan_args_t *args = (full_scan_args_t *)pvParam;
    s_cancel = false;
    s_results.in_progress = true;
    s_results.scan_start_ms = esp_timer_get_time() / 1000;

    /* Stage 1: WiFi scan */
    if (!s_cancel) {
        if (args->progress_cb) args->progress_cb(2, "Stage 1/3: WiFi scan", args->ctx);
        net_scanner_wifi_scan(NULL, NULL);
    }

    /* Stage 2: ARP host discovery */
    if (!s_cancel) {
        if (args->progress_cb) args->progress_cb(10, "Stage 2/3: Discovering hosts", args->ctx);
        net_scanner_arp_scan(args->progress_cb, NULL, args->ctx);
    }

    /* Stage 3: Port scan each host */
    if (!s_cancel) {
        int total = s_results.host_count;
        for (int i = 0; i < total && !s_cancel; i++) {
            host_result_t *host = &s_results.hosts[i];
            char stage[64];
            snprintf(stage, sizeof(stage),
                     "Stage 3/3: Port scanning %s (%d/%d)",
                     host->ip_str, i + 1, total);
            if (args->progress_cb)
                args->progress_cb((uint8_t)(50 + i * 40 / (total ? total : 1)),
                                  stage, args->ctx);
            net_scanner_port_scan(host->ip_str, host, NULL, NULL);
        }
    }

    s_results.in_progress       = false;
    s_results.scan_duration_ms  = esp_timer_get_time() / 1000 - s_results.scan_start_ms;

    if (args->progress_cb) args->progress_cb(100, "Scan complete!", args->ctx);
    if (args->done_cb) args->done_cb(&s_results, args->ctx);

    ESP_LOGI(TAG, "Full scan done in %lld ms — %d hosts, %d APs",
             s_results.scan_duration_ms, s_results.host_count, s_results.ap_count);

    free(args);
    vTaskDelete(NULL);
}

esp_err_t net_scanner_full_scan(scan_progress_cb_t progress_cb,
                                 scan_done_cb_t     done_cb,
                                 void              *ctx)
{
    if (s_results.in_progress) {
        ESP_LOGW(TAG, "Scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    /* Reset previous results */
    memset(&s_results.hosts, 0, sizeof(s_results.hosts));
    s_results.host_count = 0;
    memset(&s_results.aps, 0, sizeof(s_results.aps));
    s_results.ap_count = 0;

    full_scan_args_t *args = malloc(sizeof(full_scan_args_t));
    if (!args) return ESP_ERR_NO_MEM;
    args->progress_cb = progress_cb;
    args->done_cb     = done_cb;
    args->ctx         = ctx;

    xTaskCreate(full_scan_task, "net_scan", 8192, args,
                configMAX_PRIORITIES - 4, NULL);
    return ESP_OK;
}

void net_scanner_cancel(void)
{
    s_cancel = true;
    ESP_LOGI(TAG, "Scan cancelled");
}

const scan_results_t *net_scanner_get_results(void) { return &s_results; }

/* ── JSON serialisation ───────────────────────────────────────────────────── */

char *net_scanner_results_to_json(bool include_banners)
{
    cJSON *root = cJSON_CreateObject();

    /* Metadata */
    cJSON_AddStringToObject(root, "own_ip",      s_results.own_ip);
    cJSON_AddStringToObject(root, "gateway_ip",  s_results.gateway_ip);
    cJSON_AddStringToObject(root, "subnet_mask", s_results.subnet_mask);
    cJSON_AddNumberToObject(root, "scan_duration_ms", s_results.scan_duration_ms);

    /* WiFi APs */
    cJSON *aps = cJSON_CreateArray();
    for (int i = 0; i < s_results.ap_count; i++) {
        wifi_ap_result_t *a = &s_results.aps[i];
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid",     a->ssid[0] ? a->ssid : "<hidden>");
        cJSON_AddStringToObject(ap, "bssid",    a->bssid_str);
        cJSON_AddNumberToObject(ap, "rssi",     a->rssi);
        cJSON_AddNumberToObject(ap, "channel",  a->channel);
        cJSON_AddStringToObject(ap, "security", a->auth_str);
        cJSON_AddBoolToObject(ap,   "hidden",   a->hidden);
        cJSON_AddItemToArray(aps, ap);
    }
    cJSON_AddItemToObject(root, "wifi_networks", aps);

    /* Hosts */
    cJSON *hosts = cJSON_CreateArray();
    for (int i = 0; i < s_results.host_count; i++) {
        host_result_t *h = &s_results.hosts[i];
        cJSON *hobj = cJSON_CreateObject();
        cJSON_AddStringToObject(hobj, "ip",          h->ip_str);
        cJSON_AddStringToObject(hobj, "mac",         h->mac_str);
        cJSON_AddStringToObject(hobj, "vendor",      h->vendor);
        cJSON_AddStringToObject(hobj, "hostname",    h->hostname);
        cJSON_AddNumberToObject(hobj, "risk_score",  h->risk_score);
        cJSON_AddStringToObject(hobj, "risk_summary",h->risk_summary);
        cJSON_AddBoolToObject(hobj,   "has_http",    h->has_http);
        cJSON_AddBoolToObject(hobj,   "has_https",   h->has_https);
        if (h->http_title[0])
            cJSON_AddStringToObject(hobj, "http_title", h->http_title);
        if (h->http_server[0])
            cJSON_AddStringToObject(hobj, "http_server",h->http_server);

        cJSON *ports = cJSON_CreateArray();
        for (int p = 0; p < h->port_count; p++) {
            scan_port_result_t *pr = &h->ports[p];
            if (pr->state != PORT_STATE_OPEN) continue;
            cJSON *pobj = cJSON_CreateObject();
            cJSON_AddNumberToObject(pobj, "port",    pr->port);
            cJSON_AddStringToObject(pobj, "service", pr->service);
            if (include_banners && pr->banner[0])
                cJSON_AddStringToObject(pobj, "banner", pr->banner);
            cJSON_AddItemToArray(ports, pobj);
        }
        cJSON_AddItemToObject(hobj, "open_ports", ports);
        cJSON_AddItemToArray(hosts, hobj);
    }
    cJSON_AddItemToObject(root, "hosts", hosts);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *net_scanner_get_summary(void)
{
    char *buf = malloc(1024);
    if (!buf) return NULL;

    int offset = 0;
    offset += snprintf(buf + offset, 1024 - offset,
        "Network scan results — %d devices found, %d WiFi networks visible.\n",
        s_results.host_count, s_results.ap_count);

    /* Open APs warning */
    int open_aps = 0;
    for (int i = 0; i < s_results.ap_count; i++)
        if (s_results.aps[i].auth_mode == WIFI_AUTH_OPEN) open_aps++;
    if (open_aps > 0)
        offset += snprintf(buf + offset, 1024 - offset,
            "⚠️ %d open (unencrypted) WiFi networks nearby.\n", open_aps);

    /* High-risk hosts */
    int high_risk = 0;
    for (int i = 0; i < s_results.host_count; i++)
        if (s_results.hosts[i].risk_score >= 30) high_risk++;
    if (high_risk > 0)
        offset += snprintf(buf + offset, 1024 - offset,
            "🔴 %d high-risk device(s) found.\n", high_risk);

    /* Per-host summary */
    for (int i = 0; i < s_results.host_count; i++) {
        host_result_t *h = &s_results.hosts[i];
        offset += snprintf(buf + offset, 1024 - offset,
            "%s (%s) — %d open ports, risk %d/100. %s\n",
            h->ip_str, h->vendor, h->port_count, h->risk_score,
            h->risk_summary[0] ? h->risk_summary : "");
        if (offset >= 950) break;
    }

    return buf;
}
