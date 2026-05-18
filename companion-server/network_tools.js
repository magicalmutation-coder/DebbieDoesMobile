/**
 * network_tools.js — Companion server network security tools
 *
 * ⚠️  AUTHORISED USE ONLY ⚠️
 * Only use on networks you own or have explicit written permission to test.
 * Unauthorised network scanning is illegal in most jurisdictions.
 *
 * Provides:
 *  • nmap wrapper (if installed) for advanced scanning
 *  • NVD CVE API queries
 *  • Shodan-style local device fingerprinting
 *  • HTML vulnerability report generation
 *  • MAC vendor OUI lookup
 *  • Network topology mapping
 */

const { execFile } = require('child_process');
const https        = require('https');
const path         = require('path');
const fs           = require('fs');
const os           = require('os');
const rateLimit    = require('express-rate-limit');

/* ── nmap wrapper ─────────────────────────────────────────────────────────── */

/**
 * Run nmap on the given target and return parsed JSON results.
 * Requires nmap to be installed: apt install nmap (Linux) or https://nmap.org
 *
 * @param {string} target  - IP, CIDR, or hostname (e.g. "192.168.1.0/24")
 * @param {string[]} flags - Extra nmap flags (e.g. ['-sV', '-O'])
 * @returns {Promise<object>} Parsed nmap XML results as JSON
 */
async function nmapScan(target, flags = []) {
    return new Promise((resolve, reject) => {
        const args = [
            '-oX', '-',           /* XML to stdout */
            '--stats-every', '5s',
            '-T4',                /* Aggressive timing */
            '--open',             /* Only show open ports */
            ...flags,
            '--',
            target,
        ];

        execFile('nmap', args, { maxBuffer: 5 * 1024 * 1024, timeout: 300000 },
            (err, stdout, stderr) => {
                if (err && !stdout) {
                    if (err.code === 'ENOENT') {
                        reject(new Error(
                            'nmap not found. Install with: sudo apt install nmap\n' +
                            'or download from https://nmap.org'));
                    } else {
                        reject(new Error(`nmap failed: ${err.message}`));
                    }
                    return;
                }
                try {
                    resolve(parseNmapXml(stdout));
                } catch (e) {
                    reject(new Error(`nmap XML parse failed: ${e.message}`));
                }
            }
        );
    });
}

/**
 * Parse nmap XML output to a clean JSON structure.
 */
function parseNmapXml(xml) {
    const hosts = [];
    /* Simple regex-based extraction — use xml2js for production */
    const hostRegex = /<host[^>]*>([\s\S]*?)<\/host>/g;
    let hostMatch;

    while ((hostMatch = hostRegex.exec(xml)) !== null) {
        const hostXml = hostMatch[1];

        /* State */
        const stateMatch = /<status state="([^"]+)"/.exec(hostXml);
        if (!stateMatch || stateMatch[1] !== 'up') continue;

        /* IP */
        const ipMatch = /<address addr="([^"]+)" addrtype="ipv4"/.exec(hostXml);
        const macMatch = /<address addr="([^"]+)" addrtype="mac"(?:[^>]*)vendor="([^"]*)"/.exec(hostXml);
        const hostnameMatch = /<hostname name="([^"]+)"/.exec(hostXml);

        const host = {
            ip:       ipMatch ? ipMatch[1] : '?',
            mac:      macMatch ? macMatch[1] : '',
            vendor:   macMatch ? macMatch[2] : '',
            hostname: hostnameMatch ? hostnameMatch[1] : '',
            ports:    [],
            os:       [],
        };

        /* Ports */
        const portRegex = /<port protocol="([^"]+)" portid="(\d+)">([\s\S]*?)<\/port>/g;
        let portMatch;
        while ((portMatch = portRegex.exec(hostXml)) !== null) {
            const portXml = portMatch[3];
            const pStateMatch = /<state state="([^"]+)"/.exec(portXml);
            if (!pStateMatch || pStateMatch[1] !== 'open') continue;

            const serviceMatch = /<service name="([^"]*)"(?:[^>]*)product="([^"]*)"(?:[^>]*)version="([^"]*)"/.exec(portXml);
            const bannerMatch  = /<script id="banner" output="([^"]*)"/.exec(portXml);

            host.ports.push({
                port:    parseInt(portMatch[2]),
                proto:   portMatch[1],
                service: serviceMatch ? serviceMatch[1] : '',
                product: serviceMatch ? serviceMatch[2] : '',
                version: serviceMatch ? serviceMatch[3] : '',
                banner:  bannerMatch ? bannerMatch[1] : '',
            });
        }

        /* OS detection */
        const osMatch = /<osmatch name="([^"]+)" accuracy="(\d+)"/.exec(hostXml);
        if (osMatch) {
            host.os.push({ name: osMatch[1], accuracy: parseInt(osMatch[2]) });
        }

        hosts.push(host);
    }

    const runMatch  = /<runstats>[\s\S]*?<finished time="(\d+)"/.exec(xml);
    const timeMatch = /<runstats>[\s\S]*?elapsed="([^"]+)"/.exec(xml);

    return {
        hosts,
        scan_time:    runMatch ? parseInt(runMatch[1]) : Date.now() / 1000,
        elapsed_secs: timeMatch ? parseFloat(timeMatch[1]) : 0,
    };
}

/* ── NVD CVE API ─────────────────────────────────────────────────────────── */

/**
 * Look up CVE details from the NIST NVD API v2.
 * Rate limit: 5 requests per 30 seconds without API key.
 *
 * @param {string} cveId - e.g. "CVE-2021-44228"
 */
function cve_lookup(cveId) {
    return new Promise((resolve, reject) => {
        const url = `https://services.nvd.nist.gov/rest/json/cves/2.0?cveId=${encodeURIComponent(cveId)}`;
        const options = {
            headers: {
                'User-Agent': 'Debbie-Companion/1.0',
                ...(process.env.NVD_API_KEY
                    ? { 'apiKey': process.env.NVD_API_KEY }
                    : {})
            }
        };

        https.get(url, options, (res) => {
            let data = '';
            res.on('data', chunk => { data += chunk; });
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    const vuln = json.vulnerabilities?.[0]?.cve;
                    if (!vuln) {
                        resolve({ error: 'CVE not found', id: cveId });
                        return;
                    }

                    const desc = vuln.descriptions?.find(d => d.lang === 'en');
                    const cvss31 = vuln.metrics?.cvssMetricV31?.[0]?.cvssData;
                    const cvss30 = vuln.metrics?.cvssMetricV30?.[0]?.cvssData;
                    const cvss = cvss31 || cvss30;

                    resolve({
                        id:          cveId,
                        description: desc?.value || 'No description',
                        cvss_score:  cvss?.baseScore,
                        severity:    cvss?.baseSeverity,
                        vector:      cvss?.vectorString,
                        published:   vuln.published,
                        modified:    vuln.lastModified,
                        references:  vuln.references?.slice(0, 5).map(r => r.url) || [],
                    });
                } catch (e) {
                    reject(new Error(`Parse error: ${e.message}`));
                }
            });
        }).on('error', reject);
    });
}

/* ── HTML report generator ───────────────────────────────────────────────── */

/** Escape special HTML characters to prevent XSS in generated reports. */
function escapeHtml(str) {
    if (typeof str !== 'string') return String(str);
    return str
        .replace(/&/g,  '&amp;')
        .replace(/</g,  '&lt;')
        .replace(/>/g,  '&gt;')
        .replace(/"/g,  '&quot;')
        .replace(/'/g,  '&#039;');
}

/**
 * Generate a full HTML vulnerability report from scan + vuln data.
 *
 * @param {object} scanData   - From net_scanner_results_to_json()
 * @param {object} vulnData   - From vuln_reporter_to_json()
 * @returns {string} HTML report
 */
function generateHtmlReport(scanData, vulnData) {
    const scan = typeof scanData === 'string' ? JSON.parse(scanData) : scanData;
    const vuln = typeof vulnData === 'string' ? JSON.parse(vulnData) : vulnData;

    const now = new Date().toLocaleString();

    const sevBadge = (sev) => {
        const allowed = { CRITICAL: '#e74c3c', HIGH: '#e67e22', MEDIUM: '#f39c12', LOW: '#27ae60', INFO: '#3498db' };
        const sevSafe = escapeHtml(sev || '');
        const col = allowed[sevSafe] || '#95a5a6';
        return `<span style="background:${col};color:#fff;padding:2px 8px;border-radius:4px;font-size:.8em;font-weight:700">${sevSafe}</span>`;
    };

    const hostsHtml = (scan.hosts || []).map(h => `
        <tr>
          <td><code>${escapeHtml(h.ip || '')}</code></td>
          <td>${escapeHtml(h.vendor || '?')}</td>
          <td>${escapeHtml(h.hostname || '')}</td>
          <td>${(h.open_ports || []).map(p =>
              `<span title="${escapeHtml(String(p.service || ''))}">${escapeHtml(String(p.port || ''))}</span>`).join(', ') ||
              (typeof h.open_ports === 'number' ? escapeHtml(String(h.open_ports)) + ' ports' : '—')}</td>
          <td>
            <div style="background:#2d3436;border-radius:4px;height:8px;width:100px;overflow:hidden">
              <div style="background:${h.risk_score > 60 ? '#e74c3c' : h.risk_score > 30 ? '#e67e22' : '#27ae60'};height:100%;width:${Math.min(Number(h.risk_score) || 0, 100)}%"></div>
            </div>
            ${Number(h.risk_score) || 0}/100
          </td>
        </tr>`).join('');

    const wifiHtml = (scan.wifi_networks || []).map(ap => `
        <tr>
          <td>${ap.ssid ? escapeHtml(ap.ssid) : '<em>hidden</em>'}</td>
          <td><code>${escapeHtml(ap.bssid || '')}</code></td>
          <td>${escapeHtml(String(ap.channel || ''))}</td>
          <td>${escapeHtml(String(ap.rssi || ''))} dBm</td>
          <td>${ap.security === 'OPEN' ?
              '<span style="color:#e74c3c;font-weight:700">OPEN (unencrypted)</span>' :
              ap.security === 'WEP' ?
              '<span style="color:#e74c3c;font-weight:700">WEP (BROKEN)</span>' :
              `<span style="color:#27ae60">${escapeHtml(ap.security || '')}</span>`}</td>
        </tr>`).join('');

    const findingsHtml = (vuln.findings || []).map(f => `
        <div style="border-left:4px solid ${
            f.severity === 'CRITICAL' ? '#e74c3c' :
            f.severity === 'HIGH'     ? '#e67e22' :
            f.severity === 'MEDIUM'   ? '#f39c12' : '#27ae60'
        };padding:12px 16px;margin:12px 0;background:#1a1a2e;border-radius:0 8px 8px 0">
          <div style="margin-bottom:6px">
            ${sevBadge(f.severity)}
            <code style="margin-left:8px;color:#aaa">${escapeHtml(f.id || '')}</code>
            ${f.affected_ip ? `<code style="margin-left:8px">${escapeHtml(f.affected_ip)}${f.affected_port ? ':' + escapeHtml(String(f.affected_port)) : ''}</code>` : ''}
          </div>
          <strong>${escapeHtml(f.title || '')}</strong>
          <p style="color:#ccc;margin:6px 0">${escapeHtml(f.description || '')}</p>
          <p style="color:#06d6a0"><strong>Fix:</strong> ${escapeHtml(f.remediation || '')}</p>
        </div>`).join('');

    return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
  <title>Debbie — Network Security Report</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:system-ui,sans-serif;background:#0d0d1a;color:#eee;line-height:1.6}
    .hero{background:linear-gradient(135deg,#0f3460,#533483);padding:40px;text-align:center}
    .hero h1{font-size:2rem;margin-bottom:8px}
    .warning{background:#4d1a00;border:1px solid #e67e22;padding:12px 20px;
      border-radius:8px;margin:16px auto;max-width:900px;font-size:.9rem}
    .section{max-width:960px;margin:24px auto;padding:0 20px}
    h2{color:#06d6a0;margin-bottom:16px;border-bottom:1px solid #333;padding-bottom:8px}
    .stats{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:24px}
    .stat-card{background:#12122a;border-radius:12px;padding:16px 24px;text-align:center;min-width:120px;flex:1}
    .stat-card .num{font-size:2rem;font-weight:700;margin-bottom:4px}
    .stat-card .lbl{color:#aaa;font-size:.85rem}
    .crit{color:#e74c3c} .high{color:#e67e22} .med{color:#f39c12} .low{color:#27ae60}
    table{width:100%;border-collapse:collapse;margin-bottom:24px;background:#12122a;border-radius:12px;overflow:hidden}
    th{background:#0f3460;padding:10px 14px;text-align:left;color:#aaa;font-size:.85rem}
    td{padding:10px 14px;border-bottom:1px solid #1a1a2e;font-size:.9rem}
    tr:last-child td{border-bottom:none}
    code{background:#1a1a2e;padding:2px 6px;border-radius:4px;font-size:.85em;color:#06d6a0}
    .footer{text-align:center;padding:32px;color:#555;font-size:.85rem}
  </style>
</head>
<body>
<div class="hero">
  <h1>Debbie — Network Security Report</h1>
  <p>Generated: ${escapeHtml(now)} | Network: ${escapeHtml(scan.own_ip || '?')} | Gateway: ${escapeHtml(scan.gateway_ip || '?')}</p>
</div>

<div class="warning">
  <strong>⚠️ Authorised Use Only</strong> — This report was generated for a network
  you own or have explicit written permission to test. Unauthorised network scanning
  is illegal under the Computer Misuse Act (UK), CFAA (US), and similar laws worldwide.
</div>

<div class="section">
  <div class="stats">
    <div class="stat-card"><div class="num">${vuln.total_findings || 0}</div><div class="lbl">Total Findings</div></div>
    <div class="stat-card"><div class="num crit">${vuln.critical || 0}</div><div class="lbl">Critical</div></div>
    <div class="stat-card"><div class="num high">${vuln.high || 0}</div><div class="lbl">High</div></div>
    <div class="stat-card"><div class="num med">${vuln.medium || 0}</div><div class="lbl">Medium</div></div>
    <div class="stat-card"><div class="num low">${vuln.low || 0}</div><div class="lbl">Low</div></div>
    <div class="stat-card"><div class="num">${scan.hosts?.length || vuln.hosts_scanned || 0}</div><div class="lbl">Hosts Scanned</div></div>
    <div class="stat-card"><div class="num">${scan.wifi_networks?.length || 0}</div><div class="lbl">WiFi Networks</div></div>
  </div>
</div>

<div class="section">
  <h2>🔴 Security Findings</h2>
  ${findingsHtml || '<p style="color:#888">No vulnerabilities found — network looks clean!</p>'}
</div>

<div class="section">
  <h2>🖥️ Discovered Hosts</h2>
  <table>
    <thead><tr><th>IP</th><th>Vendor</th><th>Hostname</th><th>Open Ports</th><th>Risk Score</th></tr></thead>
    <tbody>${hostsHtml || '<tr><td colspan="5" style="text-align:center;color:#888">No hosts found</td></tr>'}</tbody>
  </table>
</div>

<div class="section">
  <h2>📶 Visible WiFi Networks</h2>
  <table>
    <thead><tr><th>SSID</th><th>BSSID</th><th>Channel</th><th>Signal</th><th>Security</th></tr></thead>
    <tbody>${wifiHtml || '<tr><td colspan="5" style="text-align:center;color:#888">No networks found</td></tr>'}</tbody>
  </table>
</div>

<div class="footer">
  Generated by <strong>Debbie</strong> — Portable AI Security Assistant |
  Freenove Media Kit ESP32-S3 | ${now}
</div>
</body>
</html>`;
}

/* ── Last scan results stored server-side (avoids accepting raw HTML data from clients) */
let lastScanData  = null;
let lastVulnData  = null;

/**
 * Store scan results on the server for later report generation.
 * Called by the device WebSocket handler when scan data arrives.
 */
function storeScanResults(scan, vulns) {
    lastScanData = scan;
    lastVulnData = vulns;
}

function addNetworkRoutes(app, broadcast) {
    /* Rate limiter for scan endpoint */
    const scanLimiter = rateLimit({ windowMs: 60000, max: 3 });
    const cveLimiter  = rateLimit({ windowMs: 30000, max: 5 });

    /* POST /network/scan — trigger nmap if available */
    app.post('/network/scan', scanLimiter, async (req, res) => {
        const ip = req.socket.remoteAddress || 'unknown';
        const { target = '192.168.1.0/24', flags = [] } = req.body;

        /*
         * Validate target strictly: only allow IPs, CIDRs, and hostnames.
         * Reject anything that could be shell metacharacters.
         * Also disallow scanning public internet ranges from this endpoint.
         */
        if (!/^[0-9./a-zA-Z-]+$/.test(target) || target.length > 64) {
            return res.status(400).json({ error: 'Invalid target format' });
        }

        /* Only allow private ranges (RFC 1918) and loopback */
        const isPrivate = /^(10\.|172\.(1[6-9]|2\d|3[01])\.|192\.168\.|127\.|::1)/.test(target);
        if (!isPrivate && !target.endsWith('.local')) {
            return res.status(400).json({
                error: 'Only private network ranges (10.x, 172.16-31.x, 192.168.x) are permitted. ' +
                       'This tool is for authorised testing of your own network only.'
            });
        }

        /* Only allow safe nmap flags */
        const ALLOWED_FLAGS = new Set(['-sV', '-O', '-sn', '-p', '--open', '-T3', '-T4', '-A']);
        if (!Array.isArray(flags) || flags.some(f => !ALLOWED_FLAGS.has(f) && !/^-p[\s\d,-]+$/.test(f))) {
            return res.status(400).json({ error: 'Invalid nmap flags' });
        }

        try {
            const results = await nmapScan(target, flags);
            storeScanResults(results, null);
            broadcast({
                type:    'agent',
                sender:  'Network Scanner',
                preview: `nmap scan of ${target}: ${results.hosts.length} hosts found`,
            });
            res.json({ ok: true, results });
        } catch (e) {
            res.status(500).json({ error: e.message });
        }
    });

    /* GET /network/cve/:id */
    app.get('/network/cve/:id', cveLimiter, async (req, res) => {
        const cveId = req.params.id;
        /* Enforce valid CVE format: year 1999–current+1, 4+ digit sequence */
        if (!/^CVE-(199[9]|2[0-9]{3})-\d{4,}$/i.test(cveId)) {
            return res.status(400).json({ error: 'Invalid CVE ID format (e.g. CVE-2021-44228)' });
        }
        try {
            const result = await cve_lookup(cveId.toUpperCase());
            res.json(result);
        } catch (e) {
            res.status(500).json({ error: e.message });
        }
    });

    /* POST /network/results — store scan results from device */
    app.post('/network/results', (req, res) => {
        const { scan, vulns } = req.body;
        if (!scan) return res.status(400).json({ error: 'scan data required' });
        storeScanResults(scan, vulns || null);
        res.json({ ok: true });
    });

    /* GET /network/report — generate HTML report from last stored scan results */
    app.get('/network/report', (req, res) => {
        if (!lastScanData) {
            return res.status(404).json({ error: 'No scan results available. Run a scan first.' });
        }
        const html = generateHtmlReport(lastScanData, lastVulnData || {});
        res.setHeader('Content-Type', 'text/html; charset=utf-8');
        res.setHeader('X-Content-Type-Options', 'nosniff');
        res.setHeader('X-Frame-Options', 'DENY');
        res.send(html);
    });

    console.log('[network] Routes registered: /network/scan, /network/cve/:id, /network/results, /network/report');
}

module.exports = { nmapScan, cve_lookup, generateHtmlReport, addNetworkRoutes, storeScanResults };
