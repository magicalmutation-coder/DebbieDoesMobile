# Debbie Companion Server

The companion server bridges external services (WhatsApp, Email, Spotify, your custom AI agent) to your Debbie ESP32-S3 device over WebSocket.

## Quick Start

```bash
npm install
cp .env.example .env
# Edit .env with your credentials
npm start
```

The server listens on `ws://0.0.0.0:3001` by default.

## Enabling Integrations

### WhatsApp
No additional setup required — on first run, a QR code will appear in the terminal. Scan it with WhatsApp on your phone (Settings → Linked Devices → Link a Device).

### Email
Set the following in `.env`:
```
EMAIL_HOST=imap.gmail.com   # or your mail server
EMAIL_USER=you@gmail.com
EMAIL_PASSWORD=your-app-password
```

**Gmail:** Use an [App Password](https://myaccount.google.com/apppasswords), not your main password.

### Spotify
1. Create an app at https://developer.spotify.com/dashboard
2. Add `http://localhost:3001/spotify/callback` as a redirect URI
3. Set `SPOTIFY_CLIENT_ID` and `SPOTIFY_CLIENT_SECRET` in `.env`
4. Start the server and visit `http://localhost:3001/spotify/auth`
5. Copy the printed refresh token into `.env` as `SPOTIFY_REFRESH_TOKEN`

### Custom Agent
Set `AGENT_URL` in `.env` to either:

- `ws://` or `wss://` endpoint (WebSocket bridge mode)
- `http://` or `https://` base URL (external API forward mode)

When using HTTP mode, the companion server forwards `/api/external/query`
calls to `AGENT_URL + /api/external/query`.
If auth is required, set `AGENT_EXTERNAL_API_KEY` (or rely on `EXTERNAL_API_KEY`).

Your agent should emit JSON in this format:
```json
{ "type": "agent", "sender": "Agent Name", "preview": "Notification text" }
```

## REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/health` | Server status |
| POST | `/notify` | Send notification to all devices |
| POST | `/spotify` | Spotify command |

## External API (`/api/external/*`)

These endpoints are intended for partner projects and remote integrations.

Authentication for all `/api/external/*` routes:

- Preferred: `Authorization: Bearer YOUR_EXTERNAL_API_KEY`
- Also accepted: `x-api-key`, `x-debbie-key`, or `?key=`

Configured by `.env`:

- `EXTERNAL_API_KEY` (required for external routes)

Endpoints:

- `GET /api/external/health`
- `GET /api/external/events?since=UNIX_MS&limit=50&topics=mail,whatsapp`
- `POST /api/external/query` with body `{ "text": "...", "sessionId": "optional" }`
- `GET /api/external/whatsapp/status`

Key management routes (Debbie session cookie auth):

- `GET /api/external/key/status`
- `POST /api/external/key/rotate`

To enable key-management auth, set `DEBBIE_SESSION_COOKIE` in `.env`,
and send `Cookie: dbt=<value>` on those routes.

## Production Deployment

For a persistent server (e.g. on a Raspberry Pi or home server):

```bash
npm install -g pm2
pm2 start server.js --name debbie-companion
pm2 save
pm2 startup
```
