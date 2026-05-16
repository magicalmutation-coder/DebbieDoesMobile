#pragma once
/*
 * vuln_reporter.h — Vulnerability Assessment & Reporting
 *
 * ⚠️  AUTHORISED USE ONLY ⚠️
 *
 * Provides:
 *  • Device self-assessment (own open services, firmware version)
 *  • CVE lookup via companion server (NVD API)
 *  • Known default-credential checks for common embedded devices
 *  • TLS/SSL configuration analysis
 *  • Formatted vulnerability reports (text + JSON)
 */

#include "esp_err.h"
#include "network_scanner.h"
#include <stdbool.h>

typedef enum {
    VULN_SEV_INFO     = 0,
    VULN_SEV_LOW,
    VULN_SEV_MEDIUM,
    VULN_SEV_HIGH,
    VULN_SEV_CRITICAL,
} vuln_severity_t;

typedef struct {
    vuln_severity_t severity;
    char            id[24];        /* e.g. "CVE-2021-12345" or "LOCAL-001" */
    char            title[96];
    char            description[256];
    char            remediation[200];
    char            affected_ip[16];
    uint16_t        affected_port;
} vuln_finding_t;

#define MAX_FINDINGS 64

typedef struct {
    vuln_finding_t  findings[MAX_FINDINGS];
    int             count;
    int             critical_count;
    int             high_count;
    int             medium_count;
    int             low_count;
    int             info_count;
    char            scan_timestamp[32];
    char            network_cidr[20];
    int             total_hosts_scanned;
    int             total_ports_scanned;
} vuln_report_t;

/**
 * @brief  Analyse scan results and build a vulnerability report.
 *         Checks for:
 *           - Plaintext protocols (Telnet, FTP, unencrypted MQTT)
 *           - Exposed management interfaces (Docker API, Redis, MongoDB)
 *           - Weak WiFi encryption (WEP, open APs)
 *           - Default credential indicators (via banner matching)
 *           - Known-vulnerable service signatures
 *           - Dangerous industrial protocol exposure (Modbus, S7)
 */
esp_err_t vuln_reporter_analyse(const scan_results_t *scan,
                                vuln_report_t        *report);

/**
 * @brief  Serialise report to a JSON string.
 *         Caller must free() the returned pointer.
 */
char *vuln_reporter_to_json(const vuln_report_t *report);

/**
 * @brief  Generate a human-readable text report.
 *         Caller must free() the returned pointer.
 */
char *vuln_reporter_to_text(const vuln_report_t *report);

/**
 * @brief  Generate a concise summary for AI voice output.
 *         Caller must free() the returned pointer.
 */
char *vuln_reporter_get_voice_summary(const vuln_report_t *report);

/**
 * @brief  Perform a self-assessment of the Debbie device itself.
 *         Checks NVS, open ports, TLS configuration, firmware version.
 */
esp_err_t vuln_reporter_self_assess(vuln_report_t *report);
