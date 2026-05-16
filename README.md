# ✨ DebbieDoesMobile

> **Debbie** — Your Portable Personal AI Friend on the Freenove Media Kit ESP32-S3

Debbie is a full-featured personal AI companion that lives on the Freenove Media Kit for ESP32-S3 (FNK0102). She can hold voice conversations, see through the camera, read your WhatsApp and email, control Spotify, and connect to your own AI agent.

---

## 📸 Debbie's UI

```
┌─────────────────────────────────────────────────────────┐
│  📶 OK  🤖 AI  ║  ✨ Debbie  ║  🔋 85%  🔔 3           │  ← status bar
├──────────────────────────┬──────────────────────────────┤
│                          │                              │
│       ╭────────╮         │  💬 Hi! I'm Debbie 😊        │
│      ( ◉    ◉ )         │                              │
│      (    ‿    )        │  I can chat, see what my     │
│       ╰────────╯         │  camera sees, read your      │
│                          │  messages, and control music.│
│       Ready! 😊          │                              │
│                          │  Press centre button or      │
│                          │  just say something!         │
├──────────────────────────┴──────────────────────────────┤
│  🎵  Artist — Song Title                      ▶ ⏭      │  ← Spotify
└─────────────────────────────────────────────────────────┘
```

The face animates — eyes open wide when listening, half-close when thinking, and blink periodically when idle. 😊

---

## 🎯 Features

| Feature | Description |
|---------|-------------|
| 🗣️ **Voice Conversations** | Real-time bidirectional voice via OpenAI Realtime API (gpt-4o-realtime-preview) |
| 📷 **Camera Vision** | Capture photos; Debbie describes what she sees using gpt-4o vision |
| 💬 **WhatsApp Pager** | Receive WhatsApp messages as notifications on-device |
| 📧 **Email Monitor** | IMAP email monitoring — important emails pushed to Debbie |
| 🎵 **Spotify Control** | Play, pause, skip, search tracks by voice command |
| 🔔 **Agent Notifications** | Connect to your own AI agent (e.g. D3881E) for custom notifications |
| ⚙️ **Captive-Portal Setup** | First-run web UI at `192.168.4.1` for easy configuration |
| 🔋 **Battery Monitor** | Battery percentage shown in status bar |
| 🌈 **Animated LVGL UI** | Friendly face with expressions, blink animations, status bar |
| 🔄 **Auto-reconnect** | Automatically reconnects to WiFi and AI if connection drops |
| 📦 **OTA Updates** | Dual-partition layout for over-the-air firmware updates |

---

## 🛒 Hardware Requirements

| Item | Details |
|------|---------|
| **Main Board** | [Freenove Media Kit ESP32-S3 (FNK0102)](https://www.freenove.com/FNK0102) |
| **Display** | 3.5" 480×320 TFT (ST7796) or 1.14" 135×240 TFT (ST7789) — comes with kit |
| **Camera** | OV2640 — included in kit |
| **Microphone** | MEMS microphone — included |
| **Speaker** | 4Ω/3W (3.5") or 8Ω/1W (1.14") — included |
| **USB Cable** | USB-C for flashing |
| **WiFi** | 2.4 GHz access point |
| **PC/Server** | To run the companion server (any machine on the same network) |

---

## 🚀 Quick Start

### 1. Set Up the Firmware

#### Prerequisites
- [ESP-IDF v5.2+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)
- Python 3.8+, Git

```bash
# Clone this repository
git clone https://github.com/magicalmutation-coder/DebbieDoesMobile.git
cd DebbieDoesMobile/firmware

# Set target to ESP32-S3
idf.py set-target esp32s3

# Build
idf.py build

# Flash (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Windows:** Use `idf.py -p COM3 flash monitor`

### 2. First-Run Setup

1. After flashing, Debbie creates a WiFi hotspot called **"Debbie"** (no password).
2. Connect your phone or computer to **"Debbie"**.
3. Open a browser and navigate to **`http://192.168.4.1`**.
4. Fill in:
   - Your home WiFi SSID and password
   - Your [OpenAI API key](https://platform.openai.com/api-keys) (starts with `sk-`)
   - Companion server URL (optional — see below)
5. Click **Save & Connect**.

Debbie will reboot, connect to your WiFi, and be ready to chat! 🎉

### 3. Set Up the Companion Server (Optional but recommended)

The companion server enables WhatsApp, email, and Spotify integration.

```bash
cd companion-server

# Install dependencies
npm install

# Copy and edit environment variables
cp .env.example .env
nano .env   # fill in your credentials

# Start the server
npm start
```

The server runs on port 3001 by default. Enter its URL in Debbie's setup page:
`ws://YOUR_PC_IP:3001`

---

## ⚙️ Configuration

All settings are stored in flash (NVS) and can be updated via the web UI at `http://192.168.4.1/` (accessible via the Debbie AP, or on your local network at Debbie's IP address).

| Setting | Description | Default |
|---------|-------------|---------|
| `wifi_ssid` | Your home WiFi network name | — |
| `wifi_password` | WiFi password | — |
| `openai_api_key` | OpenAI API key (sk-...) | — |
| `agent_ws_url` | Custom agent WebSocket URL | — |
| `companion_url` | Companion server WebSocket URL | — |
| `debbie_name` | Name shown on screen | Debbie |
| `system_prompt` | Debbie's persona prompt | friendly AI companion |
| `speaker_volume` | Speaker volume 0–100 | 75 |

---

## 🎮 Controls

| Button | Action |
|--------|--------|
| **Centre** | Capture photo + ask Debbie what she sees |
| **Up** | Volume up |
| **Down** | Volume down |
| **Long press** | Read notifications aloud |
| **Voice** | Just speak naturally — Debbie listens automatically |

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Debbie ESP32-S3                           │
│                                                             │
│  ┌──────────┐  PCM24k   ┌────────────────────────────────┐ │
│  │   MEMS   │──────────►│   OpenAI Realtime API (WS)     │ │
│  │   Mic    │           │   gpt-4o-realtime-preview      │ │
│  └──────────┘           └───────────────┬────────────────┘ │
│                                          │ PCM24k           │
│  ┌──────────┐                            ▼                  │
│  │  Camera  │─ JPEG b64 ─► gpt-4o vision HTTP              │
│  └──────────┘                                               │
│                                                             │
│  ┌──────────┐  WebSocket ┌────────────────────────────────┐ │
│  │  Display │◄──────────►│  Companion Server (Node.js)    │ │
│  │  (LVGL)  │  notify    │  WhatsApp │ Email │ Spotify    │ │
│  └──────────┘            └────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Firmware Components

| File | Purpose |
|------|---------|
| `main/main.c` | App entry, state machine, event routing |
| `main/openai_client.c` | OpenAI Realtime API + Chat/Vision HTTP |
| `main/audio_manager.c` | I2S mic capture + speaker playback |
| `main/camera_manager.c` | OV2640 capture + JPEG/base64 encoding |
| `main/display_manager.c` | LVGL UI — Debbie's face + status |
| `main/notification_client.c` | WebSocket client for companion server |
| `main/wifi_manager.c` | WiFi STA+AP, auto-reconnect |
| `main/web_server.c` | HTTP config portal + REST API |
| `main/storage_manager.c` | NVS settings persistence |
| `main/settings.h` | All GPIO pin definitions |

### Companion Server Modules

| File | Purpose |
|------|---------|
| `server.js` | Express + WebSocket server, device registry |
| `whatsapp.js` | WhatsApp Web.js integration |
| `email_monitor.js` | IMAP email polling |
| `spotify.js` | Spotify Web API controller |
| `agent_bridge.js` | Custom agent WebSocket bridge |

---

## 🔌 Connecting Your Own Agent

If you have your own AI agent (e.g. a custom LLM server), connect it via:

1. **Set the companion server** to point to your agent:
   ```
   AGENT_URL=ws://your-agent-host:8080
   ```
   
2. **Or point Debbie directly** to your agent's WebSocket:
   ```
   Agent WS URL: ws://your-agent-host:8080
   ```
   When `use_custom_agent` is enabled, Debbie connects to your agent instead of OpenAI for conversations.

3. **Your agent should send notifications** in this format:
   ```json
   { "type": "agent", "sender": "Your Agent", "preview": "Message here" }
   ```

---

## 🎵 Spotify Setup

1. Create a Spotify developer app at https://developer.spotify.com/dashboard
2. Add redirect URI: `http://localhost:3001/spotify/callback`
3. Add your Client ID and Secret to `companion-server/.env`
4. Start the companion server and visit `http://localhost:3001/spotify/auth`
5. Log in and authorise — copy the refresh token to `.env`

**Voice commands:**
- *"Play some jazz"*
- *"Skip to the next song"*
- *"Turn the volume up"*
- *"What's playing?"*
- *"Pause the music"*

> **Audible:** Direct Audible API access is not publicly available. However, you can control Audible playback through your phone (it stays on the phone), and Debbie can serve as a voice remote via the companion server.

---

## 🔧 Customising Debbie's Personality

Edit the **System Prompt** field in the setup page:

```
You are Debbie, a warm and enthusiastic AI companion. You love helping with daily 
tasks and always find the bright side of things. You speak in a friendly, casual 
tone and use the occasional emoji. You remember what we've talked about and make 
connections between conversations.
```

---

## 📐 Pin Reference (FNK0102)

| Function | GPIO |
|----------|------|
| LCD MOSI | 35 |
| LCD CLK  | 36 |
| LCD CS   | 34 |
| LCD DC   | 37 |
| LCD RST  | 38 |
| LCD BL   | 33 |
| Mic SCK  | 14 |
| Mic WS   | 13 |
| Mic SD   | 12 |
| Spk BCK  | 17 |
| Spk LRCK | 16 |
| Spk DATA | 15 |
| Cam XCLK | 10 |
| Nav Centre | 45 |
| Nav Up   | 41 |
| Nav Down | 42 |
| RGB LED  | 40 |
| Battery ADC | 4 |

> For a different board variant, edit `firmware/main/settings.h`

---

## 🐛 Troubleshooting

| Problem | Solution |
|---------|---------|
| No audio | Check I2S pin assignments in `settings.h` |
| Camera black | Verify camera pins; check PSRAM is enabled |
| Can't connect to WiFi | Use 2.4 GHz (not 5 GHz); visit AP for reconfigure |
| OpenAI errors | Verify API key; check credit balance |
| No notifications | Ensure companion server is running and URL is correct |
| Display blank | Check SPI pins and `DEBBIE_DISPLAY_35` define |

Enable verbose logging:
```bash
idf.py monitor -b 115200
```

---

## 🔄 OTA Updates

Debbie supports over-the-air firmware updates via the dual-OTA partition layout. A REST endpoint for OTA is included in the web server for future implementation.

---

## 📄 Licence

MIT — see [LICENSE](LICENSE)

---

*Made with ❤️ for the Freenove Media Kit ESP32-S3 community.*