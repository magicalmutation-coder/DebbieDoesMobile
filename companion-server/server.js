/**
 * Debbie Companion Server
 *
 * Bridges notifications and services to the Debbie ESP32-S3 device.
 *
 * Architecture:
 *   ┌──────────────┐     WebSocket      ┌─────────────────┐
 *   │  Debbie ESP32│ ◄────────────────► │ Companion Server│
 *   └──────────────┘     push/cmd       └────────┬────────┘
 *                                                 │
 *               ┌──────────────────────────────┐  │
 *               │  WhatsApp Web  │  IMAP Email  │  │
 *               │  Spotify API   │  Custom Agent│  │
 *               └──────────────────────────────┘  │
 */

require('dotenv').config();
const express    = require('express');
const http       = require('http');
const { WebSocketServer, WebSocket } = require('ws');
const cors       = require('cors');
const rateLimit  = require('express-rate-limit');
const crypto     = require('crypto');

const { startWhatsApp } = require('./whatsapp');
const { startEmailMonitor } = require('./email_monitor');
const { SpotifyController } = require('./spotify');
const { AgentBridge } = require('./agent_bridge');
const { addNetworkRoutes, storeScanResults } = require('./network_tools');
const { addMemoryRoutes } = require('./memory_store');

const app    = express();
const server = http.createServer(app);
const wss    = new WebSocketServer({ server });

app.use(cors());
app.use(express.json());

/* ── Connected Debbie devices ─────────────────────────────────────────── */
const devices = new Set();

/* ── External API runtime state ───────────────────────────────────────── */
const MAX_EXTERNAL_EVENTS = 500;
const externalEvents = [];
let externalEventSeq = 0;
let activeSessionId = 'default';
let runtimeExternalApiKey = '';

let agentBridge = null;
const whatsappState = {
    enabled: process.env.WHATSAPP_ENABLED !== 'false',
    ready: false,
    connected: false,
    lastError: '',
    lastReadyAt: 0,
};

function addExternalEvent(topic, payload) {
    externalEventSeq += 1;
    externalEvents.push({
        id: `evt_${externalEventSeq}`,
        ts: Date.now(),
        topic: topic || 'system.notice',
        data: payload || {},
    });
    if (externalEvents.length > MAX_EXTERNAL_EVENTS) {
        externalEvents.splice(0, externalEvents.length - MAX_EXTERNAL_EVENTS);
    }
}

function inferTopic(message) {
    if (!message || typeof message !== 'object') return 'system.notice';
    if (message.type === 'whatsapp') return 'whatsapp.incoming';
    if (message.type === 'email') return 'mail.incoming';
    if (message.type === 'agent') return 'agent.response';
    if (message.type === 'spotify') return 'spotify.update';
    if (message.type === 'system') return 'system.notice';
    return `${String(message.type || 'system')}.event`;
}

function broadcast(message) {
    const payload = typeof message === 'string' ? message : JSON.stringify(message);
    for (const ws of devices) {
        if (ws.readyState === WebSocket.OPEN) {
            ws.send(payload);
        }
    }
    if (typeof message === 'object' && message) {
        addExternalEvent(inferTopic(message), message);
    }
}

function getExternalApiKey() {
    return runtimeExternalApiKey ||
           process.env.EXTERNAL_API_KEY ||
           process.env.DEBBIE_EXTERNAL_API_KEY ||
           '';
}

function getProvidedApiKey(req) {
    const auth = req.headers.authorization;
    if (typeof auth === 'string' && auth.trim()) {
        const trimmed = auth.trim();
        if (/^bearer\s+/i.test(trimmed)) {
            return trimmed.replace(/^bearer\s+/i, '').trim();
        }
        return trimmed;
    }

    const apiKeyHeader = req.headers['x-api-key'];
    if (typeof apiKeyHeader === 'string' && apiKeyHeader.trim()) {
        return apiKeyHeader.trim();
    }

    const debbieKeyHeader = req.headers['x-debbie-key'];
    if (typeof debbieKeyHeader === 'string' && debbieKeyHeader.trim()) {
        return debbieKeyHeader.trim();
    }

    const queryKey = req.query && req.query.key;
    if (typeof queryKey === 'string' && queryKey.trim()) {
        return queryKey.trim();
    }

    return '';
}

function requireExternalApiKey(req, res, next) {
    const expected = getExternalApiKey();
    if (!expected) {
        return res.status(503).json({ error: 'External API key not configured' });
    }

    const provided = getProvidedApiKey(req);
    if (!provided || provided !== expected) {
        return res.status(401).json({ error: 'Unauthorized' });
    }
    return next();
}

function getCookieValue(req, cookieName) {
    const raw = req.headers.cookie;
    if (!raw || typeof raw !== 'string') return '';

    const parts = raw.split(';');
    for (const part of parts) {
        const idx = part.indexOf('=');
        if (idx < 0) continue;
        const key = part.slice(0, idx).trim();
        const val = part.slice(idx + 1).trim();
        if (key === cookieName) return val;
    }
    return '';
}

function requireDebbieSession(req, res, next) {
    const expected = process.env.DEBBIE_SESSION_COOKIE || process.env.DBT_SESSION_COOKIE;
    if (!expected) {
        return res.status(503).json({ error: 'Debbie session auth not configured' });
    }
    const provided = getCookieValue(req, 'dbt');
    if (!provided || provided !== expected) {
        return res.status(401).json({ error: 'Unauthorized' });
    }
    return next();
}

function normalizeAgentResponse(result) {
    const content = result?.content || result?.preview || result?.text ||
                    result?.message || result?.body || JSON.stringify(result || {});
    const stats = result?.stats || { provider: 'agent-bridge' };
    const id = result?.id || result?.requestId || result?.correlationId || null;
    return { content, stats, id };
}

/* ── Spotify instance ─────────────────────────────────────────────────── */
let spotify = null;

/* ── WebSocket handler ────────────────────────────────────────────────── */
wss.on('connection', (ws, req) => {
    const ip = req.socket.remoteAddress;
    console.log(`[ws] Debbie device connected from ${ip}`);
    devices.add(ws);

    const hello = {
        type: 'system',
        sender: 'companion',
        preview: `Connected to Debbie companion server v${require('./package.json').version}`,
    };
    ws.send(JSON.stringify(hello));
    addExternalEvent('system.notice', { event: 'device.connected', ip, devices: devices.size });

    ws.on('message', async (data) => {
        try {
            const msg = JSON.parse(data.toString());
            console.log('[ws] Received:', msg);

            if (msg.type === 'scan_results') {
                /* Device sends scan results — store for HTML report generation */
                storeScanResults(msg.scan || null, msg.vulns || null);
                console.log('[ws] Stored scan results from device');
            } else if (msg.type === 'spotify_command') {
                if (spotify) {
                    const result = await spotify.handleCommand(msg.action);
                    ws.send(JSON.stringify({
                        type: 'spotify',
                        sender: 'Spotify',
                        preview: result,
                    }));
                } else {
                    ws.send(JSON.stringify({
                        type: 'spotify',
                        sender: 'Spotify',
                        preview: 'Spotify not configured. Add SPOTIFY_* vars to .env',
                    }));
                }
            }
        } catch (e) {
            console.error('[ws] Message parse error:', e.message);
        }
    });

    ws.on('close', () => {
        console.log(`[ws] Device disconnected: ${ip}`);
        devices.delete(ws);
        addExternalEvent('system.notice', { event: 'device.disconnected', ip, devices: devices.size });
    });

    ws.on('error', (err) => {
        console.error('[ws] Error:', err.message);
        devices.delete(ws);
    });
});

/* ── Rate limiters ────────────────────────────────────────────────────── */
const notifyLimiter = rateLimit({ windowMs: 60000, max: 30 });
const spotifyLimiter = rateLimit({ windowMs: 60000, max: 60 });

/* ── REST API ────────────────────────────────────────────────────────── */

/* Health check */
app.get('/health', (req, res) => {
    res.json({
        status: 'ok',
        devices: devices.size,
        uptime: process.uptime(),
        version: require('./package.json').version,
    });
});

/* Send a manual notification to all connected devices */
app.post('/notify', notifyLimiter, (req, res) => {
    const { type = 'system', sender = 'Server', preview = '' } = req.body;
    broadcast({ type, sender, preview });
    res.json({ ok: true, sent_to: devices.size });
});

/* Spotify control via REST */
app.post('/spotify', spotifyLimiter, async (req, res) => {
    if (!spotify) return res.status(503).json({ error: 'Spotify not configured' });
    const { action } = req.body;
    const result = await spotify.handleCommand(action);
    broadcast({ type: 'spotify', sender: 'Spotify', preview: result });
    res.json({ ok: true, result });
});

/* ── External API routes ─────────────────────────────────────────────── */

const externalLimiter = rateLimit({ windowMs: 60000, max: 180 });
const externalRouter = express.Router();
const externalKeyRouter = express.Router();

externalRouter.use(externalLimiter);

externalRouter.get('/health', (req, res) => {
    res.json({
        ok: true,
        time: new Date().toISOString(),
        uptimeSec: Math.floor(process.uptime()),
        sessionId: activeSessionId,
        devices: devices.size,
        agentConnected: !!(agentBridge && agentBridge.isConnected()),
    });
});

externalRouter.get('/events', (req, res) => {
    const since = Number.parseInt(req.query.since, 10) || 0;
    const limitRaw = Number.parseInt(req.query.limit, 10) || 50;
    const limit = Math.max(1, Math.min(200, limitRaw));
    const topics = typeof req.query.topics === 'string'
        ? req.query.topics.split(',').map((t) => t.trim()).filter(Boolean)
        : [];

    let events = externalEvents.filter((e) => e.ts >= since);
    if (topics.length > 0) {
        const set = new Set(topics);
        events = events.filter((e) => set.has(e.topic));
    }
    if (events.length > limit) {
        events = events.slice(events.length - limit);
    }

    res.json({
        ok: true,
        now: Date.now(),
        count: events.length,
        events,
    });
});

externalRouter.post('/query', async (req, res) => {
    const text = typeof req.body?.text === 'string' ? req.body.text.trim() : '';
    const sessionId = typeof req.body?.sessionId === 'string' && req.body.sessionId.trim()
        ? req.body.sessionId.trim()
        : activeSessionId;

    if (!text) {
        return res.status(400).json({ error: 'text required' });
    }

    activeSessionId = sessionId || 'default';
    addExternalEvent('agent.command', {
        sessionId: activeSessionId,
        text,
    });

    if (!agentBridge || !agentBridge.isConnected()) {
        return res.status(503).json({
            error: 'Agent bridge unavailable',
            sessionId: activeSessionId,
        });
    }

    try {
        const result = await agentBridge.sendQuery(text, activeSessionId, 20000);
        const normalized = normalizeAgentResponse(result);
        addExternalEvent('agent.response', {
            sessionId: activeSessionId,
            id: normalized.id,
            content: normalized.content,
        });
        return res.json({
            ok: true,
            sessionId: activeSessionId,
            content: normalized.content,
            stats: normalized.stats,
            id: normalized.id,
        });
    } catch (err) {
        addExternalEvent('agent.error', {
            sessionId: activeSessionId,
            error: err.message,
        });
        return res.status(502).json({
            error: err.message,
            sessionId: activeSessionId,
        });
    }
});

externalRouter.get('/whatsapp/status', (req, res) => {
    res.json({
        ok: true,
        enabled: whatsappState.enabled,
        connected: whatsappState.connected,
        ready: whatsappState.ready,
        lastReadyAt: whatsappState.lastReadyAt || null,
        lastError: whatsappState.lastError || null,
    });
});

externalRouter.use((req, res) => {
    res.status(404).json({ error: 'unknown external endpoint' });
});

externalKeyRouter.get('/status', requireDebbieSession, (req, res) => {
    const key = getExternalApiKey();
    res.json({
        ok: true,
        configured: !!key,
        source: runtimeExternalApiKey ? 'runtime' : (process.env.EXTERNAL_API_KEY ? 'env' : 'none'),
        masked: key ? `${key.slice(0, 4)}...${key.slice(-4)}` : null,
    });
});

externalKeyRouter.post('/rotate', requireDebbieSession, (req, res) => {
    runtimeExternalApiKey = crypto.randomBytes(24).toString('hex');
    addExternalEvent('system.notice', {
        event: 'external.key.rotated',
        ts: Date.now(),
    });
    res.json({
        ok: true,
        key: runtimeExternalApiKey,
        note: 'Runtime key rotated. Persist to EXTERNAL_API_KEY for restart durability.',
    });
});

externalKeyRouter.use((req, res) => {
    res.status(404).json({ error: 'unknown external endpoint' });
});

app.use('/api/external/key', externalKeyRouter);
app.use('/api/external', requireExternalApiKey, externalRouter);

/* ── Start all integrations ──────────────────────────────────────────── */

async function main() {
    const PORT = process.env.PORT || 3001;

    /* Spotify */
    if (process.env.SPOTIFY_CLIENT_ID && process.env.SPOTIFY_CLIENT_SECRET) {
        spotify = new SpotifyController();
        await spotify.init();
        console.log('[spotify] Controller ready');
    } else {
        console.warn('[spotify] No credentials in .env — Spotify disabled');
    }

    /* WhatsApp */
    if (process.env.WHATSAPP_ENABLED !== 'false') {
        try {
            const whatsappClient = startWhatsApp((notification) => {
                console.log('[whatsapp] Notification:', notification.sender);
                broadcast(notification);
            });
            if (whatsappClient) {
                whatsappClient.on('ready', () => {
                    whatsappState.ready = true;
                    whatsappState.connected = true;
                    whatsappState.lastReadyAt = Date.now();
                    whatsappState.lastError = '';
                    addExternalEvent('whatsapp.status', { event: 'ready' });
                });
                whatsappClient.on('disconnected', (reason) => {
                    whatsappState.connected = false;
                    whatsappState.lastError = reason ? String(reason) : 'disconnected';
                    addExternalEvent('whatsapp.status', { event: 'disconnected', reason: whatsappState.lastError });
                });
                whatsappClient.on('auth_failure', (message) => {
                    whatsappState.connected = false;
                    whatsappState.lastError = message ? String(message) : 'auth_failure';
                    addExternalEvent('whatsapp.status', { event: 'auth_failure', reason: whatsappState.lastError });
                });
            }
        } catch (e) {
            console.warn('[whatsapp] Could not start:', e.message);
            whatsappState.connected = false;
            whatsappState.lastError = e.message;
        }
    }

    /* Email */
    if (process.env.EMAIL_HOST && process.env.EMAIL_USER) {
        try {
            startEmailMonitor((notification) => {
                console.log('[email] New email from:', notification.sender);
                broadcast(notification);
            });
        } catch (e) {
            console.warn('[email] Could not start:', e.message);
        }
    } else {
        console.warn('[email] No IMAP config in .env — email disabled');
    }

    /* Custom agent bridge */
    if (process.env.AGENT_URL) {
        agentBridge = new AgentBridge(process.env.AGENT_URL, (notification) => {
            broadcast(notification);
        });
        agentBridge.connect();
    } else {
        console.warn('[agent] AGENT_URL not set — external query route will return 503');
    }

    /* Network security tools */
    addNetworkRoutes(app, broadcast);

    /* Memory & RAG store */
    addMemoryRoutes(app);

    server.listen(PORT, '0.0.0.0', () => {
        console.log(`\n🤖 Debbie Companion Server running on port ${PORT}`);
        console.log(`   Devices can connect to: ws://<YOUR_IP>:${PORT}`);
        console.log(`   Health check: http://localhost:${PORT}/health\n`);
    });
}

main().catch(console.error);
