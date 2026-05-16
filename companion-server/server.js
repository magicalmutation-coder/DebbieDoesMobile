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

function broadcast(message) {
    const payload = typeof message === 'string' ? message : JSON.stringify(message);
    for (const ws of devices) {
        if (ws.readyState === WebSocket.OPEN) {
            ws.send(payload);
        }
    }
}

/* ── Spotify instance ─────────────────────────────────────────────────── */
let spotify = null;

/* ── WebSocket handler ────────────────────────────────────────────────── */
wss.on('connection', (ws, req) => {
    const ip = req.socket.remoteAddress;
    console.log(`[ws] Debbie device connected from ${ip}`);
    devices.add(ws);

    ws.send(JSON.stringify({
        type: 'system',
        sender: 'companion',
        preview: `Connected to Debbie companion server v${require('./package.json').version}`,
    }));

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
            startWhatsApp((notification) => {
                console.log('[whatsapp] Notification:', notification.sender);
                broadcast(notification);
            });
        } catch (e) {
            console.warn('[whatsapp] Could not start:', e.message);
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
        const bridge = new AgentBridge(process.env.AGENT_URL, (notification) => {
            broadcast(notification);
        });
        bridge.connect();
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
