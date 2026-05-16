/**
 * agent_bridge.js — Bridge to your custom AI agent (D3881E or similar)
 *
 * Connects to your agent's WebSocket endpoint and forwards messages
 * to connected Debbie devices.
 *
 * The agent should send JSON messages in the format:
 *   { "type": "agent", "sender": "Agent", "preview": "Message text" }
 *
 * Or standard notification format:
 *   { "type": "notification", "title": "...", "body": "..." }
 */

const { WebSocket } = require('ws');

class AgentBridge {
    constructor(agentUrl, onNotification) {
        this.agentUrl       = agentUrl;
        this.onNotification = onNotification;
        this.ws             = null;
        this.reconnectTimer = null;
        this.shouldConnect  = true;
    }

    connect() {
        if (!this.agentUrl) return;
        console.log(`[agent] Connecting to ${this.agentUrl}...`);

        this.ws = new WebSocket(this.agentUrl, {
            handshakeTimeout: 10000,
            headers: {
                'User-Agent': 'Debbie-Companion/1.0',
            },
        });

        this.ws.on('open', () => {
            console.log('[agent] ✅ Connected to agent');
            /* Announce ourselves */
            this.ws.send(JSON.stringify({
                type:   'register',
                client: 'debbie-companion',
            }));
        });

        this.ws.on('message', (data) => {
            try {
                const msg = JSON.parse(data.toString());

                /* Normalise to Debbie notification format */
                const notification = {
                    type:    'agent',
                    sender:  msg.sender || msg.title || 'Agent',
                    preview: msg.preview || msg.body || msg.text || msg.message
                             || JSON.stringify(msg),
                };
                this.onNotification(notification);
            } catch (e) {
                console.error('[agent] Parse error:', e.message);
            }
        });

        this.ws.on('close', () => {
            console.warn('[agent] Disconnected — reconnecting in 15s...');
            if (this.shouldConnect) {
                this.reconnectTimer = setTimeout(() => this.connect(), 15000);
            }
        });

        this.ws.on('error', (err) => {
            console.error('[agent] WebSocket error:', err.message);
        });
    }

    disconnect() {
        this.shouldConnect = false;
        clearTimeout(this.reconnectTimer);
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    /**
     * Send a message or command to your agent.
     */
    send(message) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(typeof message === 'string'
                ? message : JSON.stringify(message));
        }
    }
}

module.exports = { AgentBridge };
