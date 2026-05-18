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
        this.pendingQueries = new Map();
    }

    _resolvePending(requestId, payload) {
        const entry = this.pendingQueries.get(requestId);
        if (!entry) return false;
        clearTimeout(entry.timer);
        this.pendingQueries.delete(requestId);
        entry.resolve(payload);
        return true;
    }

    _rejectAllPending(reason) {
        for (const [requestId, entry] of this.pendingQueries.entries()) {
            clearTimeout(entry.timer);
            entry.reject(new Error(reason || `Agent query ${requestId} failed`));
        }
        this.pendingQueries.clear();
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

                const correlationId = msg.requestId || msg.correlationId || msg.queryId || msg.id;
                if (correlationId) {
                    this._resolvePending(String(correlationId), msg);
                } else if (this.pendingQueries.size === 1) {
                    /* Some agents omit correlation IDs. If one query is in-flight,
                     * treat the next structured response message as that result. */
                    const looksLikeResponse =
                        msg.type === 'query_result' ||
                        msg.type === 'agent_response' ||
                        msg.type === 'response' ||
                        !!(msg.content || msg.text || msg.message || msg.preview);
                    if (looksLikeResponse) {
                        const firstPending = this.pendingQueries.keys().next();
                        if (!firstPending.done) {
                            this._resolvePending(firstPending.value, msg);
                        }
                    }
                }

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
            this._rejectAllPending('Agent disconnected');
            if (this.shouldConnect) {
                this.reconnectTimer = setTimeout(() => this.connect(), 15000);
            }
        });

        this.ws.on('error', (err) => {
            console.error('[agent] WebSocket error:', err.message);
            this._rejectAllPending(`Agent WebSocket error: ${err.message}`);
        });
    }

    disconnect() {
        this.shouldConnect = false;
        clearTimeout(this.reconnectTimer);
        this._rejectAllPending('Agent bridge disconnected');
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    isConnected() {
        return !!(this.ws && this.ws.readyState === WebSocket.OPEN);
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

    sendQuery(text, sessionId = 'default', timeoutMs = 20000) {
        if (!this.isConnected()) {
            return Promise.reject(new Error('Agent bridge is not connected'));
        }
        if (!text || typeof text !== 'string') {
            return Promise.reject(new Error('text is required'));
        }

        const requestId = `q_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`;
        const payload = {
            type: 'query',
            requestId,
            sessionId,
            text,
            source: 'debbie-companion',
        };

        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pendingQueries.delete(requestId);
                reject(new Error('Agent query timed out'));
            }, timeoutMs > 1000 ? timeoutMs : 20000);

            this.pendingQueries.set(requestId, { resolve, reject, timer });

            try {
                this.ws.send(JSON.stringify(payload));
            } catch (err) {
                clearTimeout(timer);
                this.pendingQueries.delete(requestId);
                reject(err);
            }
        });
    }
}

module.exports = { AgentBridge };
