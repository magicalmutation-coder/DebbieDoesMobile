#pragma once
/*
 * self_agent.h — Debbie's Full Agent Capabilities
 *
 * ✅ Self-introspection (hardware, memory, WiFi, uptime)
 * ✅ Internet access (HTTP GET/POST)
 * ✅ DNS lookups
 * ✅ CVE database queries (NVD API)
 * ✅ Network security scanning tools
 * ✅ Vulnerability reporting
 * ✅ OpenAI function-call dispatcher
 */

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief  Get a JSON string with Debbie's hardware and runtime status.
 *         Includes CPU, memory, WiFi, uptime, enabled features.
 *         Caller must free() the returned pointer.
 */
char *self_agent_get_system_info(void);

/**
 * @brief  Fetch a URL and return the response body.
 *         Caller must free() the returned pointer.
 *
 * @param url       Full URL (http:// or https://)
 * @param max_bytes Maximum bytes to return (0 = default 4096)
 */
char *self_agent_http_get(const char *url, int max_bytes);

/**
 * @brief  Resolve a hostname to IP addresses.
 *         Returns a JSON string. Caller must free().
 */
char *self_agent_dns_lookup(const char *hostname);

/**
 * @brief  Look up a CVE in the NVD database.
 *         Returns a JSON string with description and CVSS score.
 *         Caller must free().
 */
char *self_agent_cve_lookup(const char *cve_id);

/**
 * @brief  Handle a function call dispatched by the AI model.
 *         Handles: system_info, web_fetch, dns_lookup, cve_lookup,
 *                  network_scan, get_vuln_report.
 *
 * @return true if the function was handled, false if unknown.
 */
bool self_agent_handle_function_call(const char *fn_name,
                                     const char *args_json,
                                     const char *call_id);

/**
 * @brief  JSON array string of additional tool definitions to register
 *         with the OpenAI session (network_scan, web_fetch, dns_lookup, etc.)
 */
extern const char *AGENT_TOOLS_JSON;
