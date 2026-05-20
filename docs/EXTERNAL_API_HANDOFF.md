# DebbieBot External API Handoff (ESP32 / IoT)

This document is the integration contract for external clients that need to call DebbieBot over HTTP.

## 1. Base URL

Use the DebbieBot public ingress, then append the external base path.

- Canonical HTTPS base (port 443): https://magic-nas-02.myqnapcloud.com
- HTTP base (port 80, redirect/cutover): http://magic-nas-02.myqnapcloud.com
- Base path: /api/external
- Example: https://magic-nas-02.myqnapcloud.com/api/external
- Port 3000 is internal transport and should not be used by remote partner integrations.

## 2. Authentication (Conditional)

Authentication depends on server configuration:

- If `EXTERNAL_API_KEY` is set, all `/api/external/*` endpoints require that key.
- If `EXTERNAL_API_KEY` is empty, `/api/external/*` runs in open mode (no auth required).

Preferred header format:

Authorization: Bearer YOUR_EXTERNAL_API_KEY

Current accepted fallbacks (for backward compatibility):

- x-api-key: YOUR_EXTERNAL_API_KEY
- x-debbie-key: YOUR_EXTERNAL_API_KEY
- Query parameter: ?key=YOUR_EXTERNAL_API_KEY

Recommended for all new authenticated integrations: use Authorization Bearer only.

## 3. Endpoint Contract

### 3.1 Health

- Method: GET
- Path: /health
- Full URL example: https://magic-nas-02.myqnapcloud.com/api/external/health
- Purpose: quick liveness and runtime uptime check

Typical response fields:

- ok
- time
- uptimeSec
- sessionId

### 3.2 Event Feed (polling)

- Method: GET
- Path: /events
- Full URL example: https://magic-nas-02.myqnapcloud.com/api/external/events
- Query params:
  - since: unix epoch milliseconds (optional)
  - limit: number of events, max 200 (optional)
  - topics: comma-separated topic filters (optional)

Typical response fields:

- ok
- now
- count
- events[]

Event topics commonly include:

- mail.incoming
- mail.sent
- whatsapp.incoming
- whatsapp.outgoing
- agent.command
- agent.response
- agent.error

### 3.3 Agent Query

- Method: POST
- Path: /query
- Full URL example: https://magic-nas-02.myqnapcloud.com/api/external/query
- Content-Type: application/json

Request body:

{
  "text": "Summarize unread Outlook mail and WhatsApp alerts",
  "sessionId": "optional-existing-session-id"
}

Notes:

- text is required
- If sessionId is omitted or invalid, server uses active/default session

Typical response fields:

- ok
- sessionId
- content
- stats
- id

### 3.4 WhatsApp Status

- Method: GET
- Path: /whatsapp/status
- Full URL example: https://magic-nas-02.myqnapcloud.com/api/external/whatsapp/status
- Purpose: read current WhatsApp bridge state

## 4. API Key Management (Debbie Web Session Auth)

These routes are for Debbie authenticated web users (not external API key auth):

- GET /api/external/key/status
- POST /api/external/key/rotate

Operational flow:

1. Debbie admin checks key status.
2. Debbie admin rotates key when required.
3. New key is distributed securely to external clients.

## 5. Error Contract

- 401 Unauthorized: key missing or key mismatch (only when auth is enabled)
- 503 Service Unavailable: Debbie session auth not configured for `/api/external/key/*` routes
- 404 Not Found: external endpoint not found
- 400 Bad Request: invalid payload (for example missing text in /query)

## 6. Minimal Client Request Examples

### 6.1 Health Check

curl https://magic-nas-02.myqnapcloud.com/api/external/health

If auth is enabled:

curl -H "Authorization: Bearer YOUR_EXTERNAL_API_KEY" https://magic-nas-02.myqnapcloud.com/api/external/health

### 6.2 Event Poll

curl "https://magic-nas-02.myqnapcloud.com/api/external/events?limit=50&topics=mail,whatsapp"

If auth is enabled:

curl -H "Authorization: Bearer YOUR_EXTERNAL_API_KEY" "https://magic-nas-02.myqnapcloud.com/api/external/events?limit=50&topics=mail,whatsapp"

### 6.3 Agent Query

curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"text":"Ping from remote agent"}' \
  https://magic-nas-02.myqnapcloud.com/api/external/query

If auth is enabled:

curl -X POST \
  -H "Authorization: Bearer YOUR_EXTERNAL_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"text":"Ping from remote agent"}' \
  https://magic-nas-02.myqnapcloud.com/api/external/query

## 7. Security Guidance for Integrators

- Treat the API key as a secret and store it in secure config, not source code.
- Prefer Authorization Bearer over query string key usage.
- Rotate keys on operator change or suspected leakage.
- Use HTTPS in production environments.

## 8. Server Reference (Implementation)

Current contract is implemented in v1/server.js at:

- getExternalApiKey
- getProvidedApiKey
- requireExternalApiKey
- /api/external/key/status
- /api/external/key/rotate
- /api/external/* route handler
