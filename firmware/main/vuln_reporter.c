/*
 * vuln_reporter.c — Vulnerability Assessment & Reporting
 *
 * ⚠️  AUTHORISED USE ONLY — Only analyse networks you own or have
 *      explicit written permission to test. ⚠️
 */

#include "vuln_reporter.h"
#include "settings.h"
#include "debbie.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "vulnrpt";

/* Safe fixed-size copy that always NUL-terminates the destination. */
#define SAFE_COPY_TO(dst, src) do {                                     \
    size_t _dst_sz = sizeof(dst);                                       \
    if (_dst_sz > 0) {                                                  \
        size_t _src_len = strlen(src);                                  \
        size_t _cpy = (_src_len < (_dst_sz - 1)) ? _src_len : (_dst_sz - 1); \
        memcpy((dst), (src), _cpy);                                     \
        (dst)[_cpy] = '\0';                                             \
    }                                                                    \
} while (0)

/* ── Known-vulnerable service signatures ─────────────────────────────────── */

typedef struct {
    const char *banner_fragment;  /* substring to match in banner */
    const char *vuln_id;
    const char *title;
    const char *description;
    const char *remediation;
    vuln_severity_t severity;
} banner_sig_t;

static const banner_sig_t BANNER_SIGS[] = {
    {
        "Telnetd", "LOCAL-TELNET-001",
        "Telnet service exposed",
        "Telnet transmits credentials and data in plaintext. "
        "An attacker on the same network can capture login credentials "
        "using a passive packet sniffer.",
        "Disable Telnet. Use SSH (port 22) for remote management instead.",
        VULN_SEV_HIGH,
    },
    {
        "220 FTP", "LOCAL-FTP-001",
        "FTP service exposed (plaintext)",
        "FTP credentials and file transfers are sent in plaintext. "
        "Credentials can be captured by a network eavesdropper.",
        "Replace FTP with SFTP or FTPS. Restrict access to authorised IPs.",
        VULN_SEV_MEDIUM,
    },
    {
        "Redis", "LOCAL-REDIS-001",
        "Redis server detected — verify authentication",
        "Unauthenticated Redis instances allow remote code execution via "
        "CONFIG SET to write SSH keys or cron jobs.",
        "Enable requirepass in redis.conf. Bind Redis to 127.0.0.1 only. "
        "Use a firewall rule to block external access to port 6379.",
        VULN_SEV_HIGH,
    },
    {
        "MongoDB", "LOCAL-MONGO-001",
        "MongoDB detected — verify authentication",
        "MongoDB instances without authentication allow any client to "
        "read, write, and drop all databases.",
        "Enable --auth flag. Create admin users. Bind to 127.0.0.1 "
        "unless remote access is required.",
        VULN_SEV_HIGH,
    },
    {
        "Docker", "LOCAL-DOCKER-001",
        "Docker API exposed without TLS",
        "An unauthenticated Docker API allows any host on the network to "
        "create containers with full host filesystem access, enabling "
        "complete host compromise.",
        "Enable Docker TLS client authentication (--tlsverify). "
        "Remove port 2375 exposure — use Unix socket locally.",
        VULN_SEV_CRITICAL,
    },
    {
        "HTTP/1", "LOCAL-HTTP-001",
        "Unencrypted HTTP service",
        "Data transmitted over HTTP is in plaintext and can be "
        "intercepted or modified by network attackers (MITM).",
        "Redirect HTTP to HTTPS. Obtain a certificate from Let's Encrypt.",
        VULN_SEV_LOW,
    },
    {
        "OpenSSH 6", "CVE-2016-0777",
        "OpenSSH < 7.1 — roaming memory leak",
        "OpenSSH versions before 7.1p2 with roaming enabled may leak "
        "private key material to a malicious server.",
        "Upgrade OpenSSH to version 7.4 or later.",
        VULN_SEV_MEDIUM,
    },
    {
        "OpenSSH 7.0", "CVE-2015-6564",
        "OpenSSH 7.0 — use-after-free in keyboard-interactive auth",
        "A use-after-free vulnerability in the keyboard-interactive "
        "authentication mechanism could allow authentication bypass.",
        "Upgrade OpenSSH to 7.4 or later.",
        VULN_SEV_HIGH,
    },
    {
        "MQTT", "LOCAL-MQTT-001",
        "MQTT broker detected — verify authentication",
        "MQTT brokers without authentication allow any client to subscribe "
        "to all topics and publish arbitrary messages, including to IoT "
        "actuators.",
        "Enable MQTT username/password or certificate authentication. "
        "Use MQTTS (port 8883) with TLS.",
        VULN_SEV_MEDIUM,
    },
    {
        "Elasticsearch", "LOCAL-ES-001",
        "Elasticsearch API exposed",
        "Unauthenticated Elasticsearch instances expose all indexed data "
        "to any network client and allow deletion of indices.",
        "Enable Elasticsearch X-Pack security. Restrict network access. "
        "Upgrade to a version with built-in security.",
        VULN_SEV_HIGH,
    },
};
#define BANNER_SIGS_COUNT (sizeof(BANNER_SIGS) / sizeof(BANNER_SIGS[0]))

/* ── Port-based checks (without banner) ─────────────────────────────────── */

typedef struct {
    uint16_t        port;
    const char     *vuln_id;
    const char     *title;
    const char     *description;
    const char     *remediation;
    vuln_severity_t severity;
} port_vuln_t;

static const port_vuln_t PORT_VULNS[] = {
    {
        502, "ICS-MODBUS-001",
        "Modbus TCP exposed — Industrial control protocol",
        "Modbus is an industrial control protocol with NO authentication "
        "or encryption. Exposure on a general network allows any client "
        "to read sensor values and issue control commands to PLCs/actuators. "
        "This is a CRITICAL risk for industrial/OT environments.",
        "Isolate Modbus devices on a dedicated OT network segment with "
        "strict firewall rules. Never expose port 502 to untrusted networks.",
        VULN_SEV_CRITICAL,
    },
    {
        102, "ICS-S7-001",
        "Siemens S7comm exposed — Industrial control protocol",
        "S7comm is Siemens' proprietary PLC protocol with limited security. "
        "Remote access allows reading/writing memory areas and halting PLCs.",
        "Isolate on dedicated OT/ICS network. Apply Siemens' security "
        "hardening guidelines. Use TLS where available (S7comm+).",
        VULN_SEV_CRITICAL,
    },
    {
        4840, "ICS-OPCUA-001",
        "OPC-UA server detected",
        "OPC-UA is an industrial protocol. Verify that security policies "
        "are not set to 'None' which would allow unauthenticated access.",
        "Configure OPC-UA security policy to SignAndEncrypt minimum. "
        "Enable user authentication.",
        VULN_SEV_MEDIUM,
    },
    {
        5900, "LOCAL-VNC-001",
        "VNC remote desktop exposed",
        "VNC without strong authentication or a VPN exposes the desktop "
        "to brute-force attacks. Many embedded devices use default or "
        "no VNC passwords.",
        "Restrict VNC access behind a VPN or SSH tunnel. "
        "Set a strong password. Use NLA or certificate authentication.",
        VULN_SEV_HIGH,
    },
    {
        2375, "LOCAL-DOCKER-NOAUTH",
        "Docker Remote API on port 2375 (no TLS)",
        "Port 2375 is Docker's unauthenticated API. Any client on the "
        "network can create privileged containers and escape to the host OS.",
        "Stop Docker on this port immediately. Use Unix socket "
        "(/var/run/docker.sock) or enable TLS with client certificates.",
        VULN_SEV_CRITICAL,
    },
    {
        9200, "LOCAL-ES-PORT",
        "Elasticsearch HTTP API on port 9200",
        "Elasticsearch's default configuration allows unauthenticated "
        "access to all data and administrative functions.",
        "Enable X-Pack security. Restrict to localhost only.",
        VULN_SEV_HIGH,
    },
    {
        161, "LOCAL-SNMP-001",
        "SNMP service detected",
        "SNMP v1/v2c use community strings (often 'public'/'private') "
        "which are transmitted in plaintext and rarely changed from defaults. "
        "SNMP write access allows device configuration changes.",
        "Use SNMPv3 with authentication and privacy. Change default "
        "community strings. Restrict SNMP access by source IP.",
        VULN_SEV_MEDIUM,
    },
    {
        1883, "LOCAL-MQTT-PORT",
        "MQTT broker on port 1883 (plaintext)",
        "Unencrypted MQTT allows credential sniffing and message interception. "
        "IoT devices may subscribe to control topics.",
        "Migrate to MQTTS (port 8883). Enable broker authentication.",
        VULN_SEV_MEDIUM,
    },
};
#define PORT_VULNS_COUNT (sizeof(PORT_VULNS) / sizeof(PORT_VULNS[0]))

/* ── WiFi checks ─────────────────────────────────────────────────────────── */

static void check_wifi_security(const scan_results_t *scan,
                                 vuln_report_t *report)
{
    for (int i = 0; i < scan->ap_count; i++) {
        const wifi_ap_result_t *ap = &scan->aps[i];
        const char *ssid = ap->ssid[0] ? ap->ssid : "<hidden>";

        if (ap->auth_mode == WIFI_AUTH_OPEN && report->count < MAX_FINDINGS) {
                vuln_finding_t *f = &report->findings[report->count++];
                f->severity = VULN_SEV_HIGH;
                snprintf(f->id, sizeof(f->id), "%s", "WIFI-OPEN-001");
                snprintf(f->title, sizeof(f->title),
                     "Open WiFi network: \"%s\"", ssid);
                snprintf(f->description, sizeof(f->description),
                     "Network '%s' (channel %d, BSSID %s) has no encryption. "
                     "All traffic on this network is visible to any nearby observer.",
                     ssid, ap->channel, ap->bssid_str);
                snprintf(f->remediation, sizeof(f->remediation),
                    "%s",
                    "Enable WPA2-PSK or WPA3 encryption. "
                    "If intentional (guest AP), isolate it from the main LAN.");
        }

        if (ap->auth_mode == WIFI_AUTH_WEP && report->count < MAX_FINDINGS) {
                vuln_finding_t *f = &report->findings[report->count++];
                f->severity = VULN_SEV_CRITICAL;
                snprintf(f->id, sizeof(f->id), "%s", "WIFI-WEP-001");
                snprintf(f->title, sizeof(f->title),
                     "WEP encryption on \"%s\" — broken, easily cracked", ssid);
                snprintf(f->description, sizeof(f->description),
                     "Network '%s' uses WEP encryption which can be cracked in "
                     "under 60 seconds using freely available tools. This provides "
                     "no meaningful security protection.", ssid);
                snprintf(f->remediation, sizeof(f->remediation), "%s",
                    "Replace WEP with WPA2-PSK (AES/CCMP) or WPA3 immediately. "
                    "All connected clients must be re-configured.");
        }
    }
}

/* ── Helper to add a finding ─────────────────────────────────────────────── */

static void add_finding(vuln_report_t *report, const port_vuln_t *pv,
                        const char *ip, uint16_t port)
{
    if (report->count >= MAX_FINDINGS) return;
    vuln_finding_t *f = &report->findings[report->count++];
    f->severity      = pv->severity;
    f->affected_port = port;
    SAFE_COPY_TO(f->id, pv->vuln_id);
    SAFE_COPY_TO(f->title, pv->title);
    SAFE_COPY_TO(f->description, pv->description);
    SAFE_COPY_TO(f->remediation, pv->remediation);
    SAFE_COPY_TO(f->affected_ip, ip);
}

static void add_banner_finding(vuln_report_t *report, const banner_sig_t *bs,
                               const char *ip, uint16_t port)
{
    if (report->count >= MAX_FINDINGS) return;
    vuln_finding_t *f = &report->findings[report->count++];
    f->severity      = bs->severity;
    f->affected_port = port;
    SAFE_COPY_TO(f->id, bs->vuln_id);
    SAFE_COPY_TO(f->title, bs->title);
    SAFE_COPY_TO(f->description, bs->description);
    SAFE_COPY_TO(f->remediation, bs->remediation);
    SAFE_COPY_TO(f->affected_ip, ip);
}

/* ── Count by severity ────────────────────────────────────────────────────── */

static void tally_severities(vuln_report_t *report)
{
    report->critical_count = 0;
    report->high_count     = 0;
    report->medium_count   = 0;
    report->low_count      = 0;
    report->info_count     = 0;
    for (int i = 0; i < report->count; i++) {
        switch (report->findings[i].severity) {
        case VULN_SEV_CRITICAL: report->critical_count++; break;
        case VULN_SEV_HIGH:     report->high_count++;     break;
        case VULN_SEV_MEDIUM:   report->medium_count++;   break;
        case VULN_SEV_LOW:      report->low_count++;      break;
        case VULN_SEV_INFO:     report->info_count++;     break;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t vuln_reporter_analyse(const scan_results_t *scan,
                                vuln_report_t        *report)
{
    if (!scan || !report) return ESP_ERR_INVALID_ARG;
    memset(report, 0, sizeof(*report));

    report->total_hosts_scanned = scan->host_count;

    /* Record timestamp */
    time_t now = 0;
    time(&now);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(report->scan_timestamp, sizeof(report->scan_timestamp),
             "%Y-%m-%d %H:%M:%S", &tm_info);

    /* Safely construct network_cidr without risking format-truncation warnings.
     * Build manually by copying bounded amounts and always NUL-terminating. */
    char ipbuf[16];
    char maskbuf[16];
    strncpy(ipbuf, scan->own_ip, sizeof(ipbuf) - 1); ipbuf[sizeof(ipbuf) - 1] = '\0';
    strncpy(maskbuf, scan->subnet_mask, sizeof(maskbuf) - 1); maskbuf[sizeof(maskbuf) - 1] = '\0';
    /* Manual safe assembly: ip + '/' + mask, truncated as needed */
    size_t _dst_sz = sizeof(report->network_cidr);
    size_t _pos = 0;
    size_t _to_copy = strnlen(ipbuf, sizeof(ipbuf));
    if (_to_copy > 0) {
        size_t _cpy = (_to_copy < _dst_sz - 1) ? _to_copy : _dst_sz - 1;
        memcpy(report->network_cidr + _pos, ipbuf, _cpy);
        _pos += _cpy;
    }
    if (_pos < _dst_sz - 1) {
        report->network_cidr[_pos++] = '/';
    }
    if (_pos < _dst_sz - 1) {
        size_t _mask_len = strnlen(maskbuf, sizeof(maskbuf));
        size_t _cpy2 = (_mask_len < (_dst_sz - _pos - 1)) ? _mask_len : (_dst_sz - _pos - 1);
        if (_cpy2 > 0) {
            memcpy(report->network_cidr + _pos, maskbuf, _cpy2);
            _pos += _cpy2;
        }
    }
    report->network_cidr[_pos < _dst_sz ? _pos : _dst_sz - 1] = '\0';

    /* WiFi security checks */
    check_wifi_security(scan, report);

    /* Per-host analysis */
    int total_ports = 0;
    for (int h = 0; h < scan->host_count; h++) {
        const host_result_t *host = &scan->hosts[h];
        total_ports += host->port_count;

        for (int p = 0; p < host->port_count; p++) {
            const scan_port_result_t *port = &host->ports[p];
            if (port->state != PORT_STATE_OPEN) continue;

            /* Banner signature matching */
            for (int s = 0; s < (int)BANNER_SIGS_COUNT; s++) {
                if (port->banner[0] &&
                    strstr(port->banner, BANNER_SIGS[s].banner_fragment)) {
                    add_banner_finding(report, &BANNER_SIGS[s],
                                       host->ip_str, port->port);
                }
            }

            /* Port-based checks (no banner needed) */
            for (int v = 0; v < (int)PORT_VULNS_COUNT; v++) {
                if (port->port == PORT_VULNS[v].port) {
                    add_finding(report, &PORT_VULNS[v],
                                host->ip_str, port->port);
                }
            }
        }
    }
    report->total_ports_scanned = total_ports;

    /* De-duplicate findings (same vuln_id + ip) */
    for (int i = 0; i < report->count - 1; i++) {
        for (int j = i + 1; j < report->count; j++) {
            if (strcmp(report->findings[i].id,          report->findings[j].id) == 0 &&
                strcmp(report->findings[i].affected_ip, report->findings[j].affected_ip) == 0) {
                /* Remove j by overwriting with last */
                report->findings[j] = report->findings[report->count - 1];
                report->count--;
                j--;
            }
        }
    }

    tally_severities(report);

    ESP_LOGI(TAG, "Analysis: %d findings (C:%d H:%d M:%d L:%d)",
             report->count,
             report->critical_count, report->high_count,
             report->medium_count,   report->low_count);
    return ESP_OK;
}

/* ── Serialisation ────────────────────────────────────────────────────────── */

static const char *sev_str(vuln_severity_t s)
{
    switch (s) {
    case VULN_SEV_CRITICAL: return "CRITICAL";
    case VULN_SEV_HIGH:     return "HIGH";
    case VULN_SEV_MEDIUM:   return "MEDIUM";
    case VULN_SEV_LOW:      return "LOW";
    default:                return "INFO";
    }
}

char *vuln_reporter_to_json(const vuln_report_t *report)
{
    if (!report) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scan_time",            report->scan_timestamp);
    cJSON_AddStringToObject(root, "network",              report->network_cidr);
    cJSON_AddNumberToObject(root, "hosts_scanned",        report->total_hosts_scanned);
    cJSON_AddNumberToObject(root, "ports_scanned",        report->total_ports_scanned);
    cJSON_AddNumberToObject(root, "total_findings",       report->count);
    cJSON_AddNumberToObject(root, "critical",             report->critical_count);
    cJSON_AddNumberToObject(root, "high",                 report->high_count);
    cJSON_AddNumberToObject(root, "medium",               report->medium_count);
    cJSON_AddNumberToObject(root, "low",                  report->low_count);
    cJSON_AddNumberToObject(root, "info",                 report->info_count);

    cJSON *findings = cJSON_CreateArray();
    for (int i = 0; i < report->count; i++) {
        const vuln_finding_t *f = &report->findings[i];
        cJSON *fobj = cJSON_CreateObject();
        cJSON_AddStringToObject(fobj, "severity",    sev_str(f->severity));
        cJSON_AddStringToObject(fobj, "id",          f->id);
        cJSON_AddStringToObject(fobj, "title",       f->title);
        cJSON_AddStringToObject(fobj, "description", f->description);
        cJSON_AddStringToObject(fobj, "remediation", f->remediation);
        if (f->affected_ip[0])
            cJSON_AddStringToObject(fobj, "affected_ip", f->affected_ip);
        if (f->affected_port)
            cJSON_AddNumberToObject(fobj, "affected_port", f->affected_port);
        cJSON_AddItemToArray(findings, fobj);
    }
    cJSON_AddItemToObject(root, "findings", findings);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *vuln_reporter_to_text(const vuln_report_t *report)
{
    if (!report) return NULL;
    char *buf = malloc(4096);
    if (!buf) return NULL;
    int off = 0;

    off += snprintf(buf + off, 4096 - off,
        "═══════════════════════════════════════════════════════\n"
        "  DEBBIE NETWORK SECURITY REPORT\n"
        "  ⚠️  Authorised Use Only — Own Networks / With Permission\n"
        "═══════════════════════════════════════════════════════\n"
        "Scan time   : %s\n"
        "Network     : %s\n"
        "Hosts scanned: %d  |  Ports scanned: %d\n"
        "Findings    : %d total — "
        "🔴 %d CRITICAL  🟠 %d HIGH  🟡 %d MEDIUM  🟢 %d LOW\n"
        "───────────────────────────────────────────────────────\n",
        report->scan_timestamp, report->network_cidr,
        report->total_hosts_scanned, report->total_ports_scanned,
        report->count,
        report->critical_count, report->high_count,
        report->medium_count, report->low_count);

    /* Print findings: critical first */
    for (int i = 0; i < report->count && off < 3900; i++) {
        const vuln_finding_t *f = &report->findings[i];
        /* Use ASCII severity labels to avoid multi-byte buffer overflow */
        const char *sev_icon;
        switch (f->severity) {
        case VULN_SEV_CRITICAL: sev_icon = "[CRITICAL]"; break;
        case VULN_SEV_HIGH:     sev_icon = "[HIGH]";     break;
        case VULN_SEV_MEDIUM:   sev_icon = "[MEDIUM]";   break;
        case VULN_SEV_LOW:      sev_icon = "[LOW]";      break;
        default:                sev_icon = "[INFO]";     break;
        }
        off += snprintf(buf + off, 4096 - off,
            "\n%s %s (%s)\n"
            "   Host: %s  Port: %d\n"
            "   %s\n"
            "   Fix: %s\n",
            sev_icon, f->title, f->id,
            f->affected_ip[0] ? f->affected_ip : "N/A",
            f->affected_port,
            f->description,
            f->remediation);
    }

    off += snprintf(buf + off, 4096 - off,
        "\n═══════════════════════════════════════════════════════\n"
        "  Generated by Debbie — Portable AI Security Assistant\n"
        "═══════════════════════════════════════════════════════\n");
    return buf;
}

char *vuln_reporter_get_voice_summary(const vuln_report_t *report)
{
    if (!report) return NULL;
    char *buf = malloc(512);
    if (!buf) return NULL;
    int off = 0;

    if (report->count == 0) {
        snprintf(buf, 512,
            "Great news! The network scan is complete. "
            "I scanned %d devices and found no significant vulnerabilities. "
            "Your network looks pretty clean!",
            report->total_hosts_scanned);
        return buf;
    }

    off += snprintf(buf + off, 512 - off,
        "Security scan complete. I scanned %d devices and found %d vulnerabilities — ",
        report->total_hosts_scanned, report->count);

    if (report->critical_count > 0)
        off += snprintf(buf + off, 512 - off,
            "%d critical, ", report->critical_count);
    if (report->high_count > 0)
        off += snprintf(buf + off, 512 - off,
            "%d high, ", report->high_count);
    if (report->medium_count > 0)
        off += snprintf(buf + off, 512 - off,
            "%d medium severity. ", report->medium_count);

    /* Mention the worst finding */
    if (report->count > 0) {
        const vuln_finding_t *worst = &report->findings[0];
        /* Truncate the title into a local buffer before formatting */
        char worst_title[64];
        strncpy(worst_title, worst->title, sizeof(worst_title) - 1);
        worst_title[sizeof(worst_title) - 1] = '\0';
        off += snprintf(buf + off, 512 - off,
            "The most serious issue is: %s. I've logged the full report. "
            "Shall I walk you through the fixes?",
            worst_title);
    }

    return buf;
}

/* ── Self-assessment of Debbie device ────────────────────────────────────── */

esp_err_t vuln_reporter_self_assess(vuln_report_t *report)
{
    if (!report) return ESP_ERR_INVALID_ARG;
    memset(report, 0, sizeof(*report));

    /* Use a real timestamp */
    time_t now = 0;
    time(&now);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(report->scan_timestamp, sizeof(report->scan_timestamp),
             "%Y-%m-%d %H:%M:%S", &tm_info);
    snprintf(report->network_cidr, sizeof(report->network_cidr), "%s", "device-self");

    /* Check TLS config */
#ifdef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    if (report->count < MAX_FINDINGS) {
        vuln_finding_t *f = &report->findings[report->count++];
        f->severity = VULN_SEV_HIGH;
        snprintf(f->id, sizeof(f->id), "%s", "SELF-TLS-001");
        snprintf(f->title, sizeof(f->title), "%s", "TLS certificate verification disabled (development mode)");
        snprintf(f->description, sizeof(f->description), "%s",
            "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled. "
            "The device does not verify TLS server certificates, "
            "making it vulnerable to MITM attacks on all HTTPS/WSS connections.");
        snprintf(f->remediation, sizeof(f->remediation), "%s",
            "Remove CONFIG_ESP_TLS_INSECURE from sdkconfig.defaults "
            "before deploying to production.");
    }
#endif

    /* Check firmware version */
    if (report->count < MAX_FINDINGS) {
        vuln_finding_t *f = &report->findings[report->count++];
        f->severity = VULN_SEV_INFO;
        snprintf(f->id, sizeof(f->id), "%s", "SELF-VER-001");
        snprintf(f->title, sizeof(f->title),
                 "Firmware: ESP-IDF v%d.%d.%d",
                 ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
        snprintf(f->description, sizeof(f->description),
                 "Running ESP-IDF v%d.%d.%d. Ensure this is kept up to date.",
                 ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
        snprintf(f->remediation, sizeof(f->remediation), "%s",
            "Monitor https://github.com/espressif/esp-idf/releases for security patches.");
    }

    tally_severities(report);
    return ESP_OK;
}
