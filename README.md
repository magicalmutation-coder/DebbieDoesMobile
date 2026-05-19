# ✨ DebbieDoesMobile

> **Debbie** — Your Portable Personal AI Friend on the Freenove Media Kit ESP32-S3

Debbie is a full-featured personal AI companion that lives on the Freenove Media Kit for ESP32-S3 (FNK0102). She can hold voice conversations, read your WhatsApp and email, run on-device security scans, and connect to your own AI agent, with setup support for OpenAI and LM Studio providers.

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
│       ╰────────╯         │  network looks like, read     │
│                          │  your messages, and help you. │
│       Ready! 😊          │                              │
│                          │  Press centre button or      │
│                          │  just say something!         │
└──────────────────────────┴──────────────────────────────┘
```

The UI now focuses on practical status and text output: WiFi/AI state, notifications, and setup/network diagnostics.
Bottom footer is network diagnostics only (AP/STA/AI); Spotify now-playing strip is removed from runtime UI.

---

## 🎯 Features

| Feature | Description |
|---------|-------------|
| 🗣️ **Voice Conversations** | Real-time bidirectional voice via provider-aware runtime (OpenAI Realtime or LM Studio local endpoint) |
| 📷 **Camera Vision** | OpenAI vision path is available in code, currently runtime-disabled by default on the 3.5" profile until a non-conflicting camera pin map is validated |
| 💬 **WhatsApp Pager** | Receive WhatsApp messages as notifications on-device |
| 📧 **Email Monitor** | IMAP email monitoring — important emails pushed to Debbie |
| 🧪 **Local LLM Provider (LM Studio)** | Select LM Studio in setup and auto-detect available local models |
| ✅ **Provider Model Probe** | `/llm_models` can now query model lists for Ollama, LM Studio, and OpenAI (when API key is configured) |
| 🔐 **OpenAI Key Guardrails** | Setup now sanitizes pasted OpenAI keys (trims whitespace, strips accidental `Bearer ` prefixes/text) and shows a specific on-device warning when key auth is rejected |
| 🧩 **Node Agent Bridge Tool** | Debbie now exposes `node_agent_query` so the live voice agent can call companion `/api/external/query` with Bearer auth |
| 🎵 **Spotify Control** | Temporarily paused in runtime while connectivity and launcher UX are stabilised |
| 🔔 **Agent Notifications** | Connect to your own AI agent (e.g. D3881E) for custom notifications |
| ⚙️ **Captive-Portal Setup** | First-run web UI at `192.168.4.1` for easy configuration |
| 🔋 **Battery Monitor** | Battery percentage shown in status bar |
| 🌈 **Practical LVGL UI** | Full-width status/text layout with connection indicators and diagnostics |
| 🎙️ **Speech-ready Boot** | When AI connects, Debbie enters listening-ready mode automatically so speech-to-text starts without extra setup |
| 🧭 **On-device Launcher Menu** | Three-button navigation for network route help, hotspot setup, scanner, and notifications |
| 🔄 **Auto-reconnect** | Automatically reconnects to WiFi and AI if connection drops |
| 🌐 **On-screen Network Info** | Shows AP URL, STA IP, and AI/provider status on the display footer and posts hotspot-route summaries in chat |
| 📦 **OTA Updates** | Dual-partition layout for over-the-air firmware updates |

---

## 📚 Documentation

- [Bring-up and troubleshooting guide](docs/DEVICE_BRINGUP_AND_TROUBLESHOOTING.md)
- [Firmware function index](docs/FUNCTION_INDEX.md)
- [Bluetooth speaker migration path](docs/BLUETOOTH_AUDIO_MIGRATION.md)
- [External API handoff contract](docs/EXTERNAL_API_HANDOFF.md)
- [Postman collection for external API](docs/EXTERNAL_API.postman_collection.json)
- [External API share folder README](external-api-handoff/README.md)
- [External API Postman environment](external-api-handoff/EXTERNAL_API.postman_environment.json)

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

## 🖥️ Display Driver Profile (FNK0102B 3.5")

The firmware now uses the same core profile as Freenove's LVGL Picture example for the 3.5" panel:

- Driver family: ST7796-compatible panel init (via ESP-IDF ST7789 panel driver + vendor init sequence)
- SPI host: `SPI2_HOST` (HSPI-equivalent mapping on ESP32-S3)
- Pins: MOSI=21, SCLK=47, DC=45, RST=20, BL=2, CS=-1
- Orientation: landscape 480x320 (`swap_xy=true`, `mirror_x=false`, `mirror_y=false`)
- Inversion: enabled (`invert=true`)

If the screen shows noise or random dots, check the boot log for `display profile:` and verify these values match your board.

Normal runtime uses the full Debbie LVGL interface by default (the temporary `HELLO` panel test mode is disabled).

BLE note: BLE runtime is currently disabled by default in this profile due a reproducible BT stack assert on this hardware/IDF combination. See the troubleshooting guide for details.

Camera note: camera runtime is currently disabled by default on this profile (`DEBBIE_ENABLE_CAMERA_RUNTIME=0`) because the current camera pin map overlaps active display pins and can blank the panel after boot.

WiFi setup note: AP-only to AP+STA transitions are now handled without panic during setup saves (no `ESP_ERR_WIFI_MODE` abort expected on credential apply).

Display runtime note: backlight GPIO is now held high in firmware (`gpio_hold_en`) after panel init to prevent runtime pin-mode interference from blanking the screen.

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
2. Connect your phone or computer to **"Debbie"** for setup.
3. Open a browser and navigate to **`http://192.168.4.1`**.
4. Fill in:
   - Your hotspot/home WiFi SSID and password (Debbie joins this as a client for internet access)
   - LLM provider (`openai` or `lmstudio`)
   - Model name and local URL for LM Studio (if selected)
   - Your [OpenAI API key](https://platform.openai.com/api-keys) (starts with `sk-`, required only for OpenAI provider)
   - Companion server URL (optional — see below)
5. Click **Save & Connect**.

Debbie will reboot, connect to your hotspot/WiFi as a client (STA), and be ready to chat on the move. 🎉

### 3. Set Up the Companion Server (Optional but recommended)

The companion server enables WhatsApp, email, memory/RAG sync, and custom agent integrations.

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

External HTTP integration note: companion projects consuming `/api/external/*`
should send `Authorization: Bearer YOUR_EXTERNAL_API_KEY` on every call.
See [External API handoff contract](docs/EXTERNAL_API_HANDOFF.md) for endpoint and error contracts.

Node bridge note: to let Debbie's on-device agent tool call Node `/api/external/query`, set:
1. `EXTERNAL_API_KEY` in `companion-server/.env`
2. `Companion Server URL` and `External API Key` in Debbie setup page (`http://192.168.4.1`)
3. `AGENT_URL` in `companion-server/.env` if you want `/api/external/query` to forward into D3881E:
   - `ws://` or `wss://` for WebSocket bridge mode
   - `http://` or `https://` base URL for external API forwarding mode (for example `https://magic-nas-02.myqnapcloud.com`)
4. Optional: set `AGENT_EXTERNAL_API_KEY` in `companion-server/.env` when HTTP forwarding target requires a different bearer key

Share-pack note: the full handoff packet is available in [external-api-handoff](external-api-handoff/README.md) and can be shared as-is with partner projects.

---

## ⚙️ Configuration

All settings are stored in flash (NVS) and can be updated via the web UI at `http://192.168.4.1/` (accessible via the Debbie AP, or on your local network at Debbie's IP address).

Settings storage note: configuration is persisted in internal ESP32 NVS flash (not on SD card).

| Setting | Description | Default |
|---------|-------------|---------|
| `wifi_ssid` | Your home WiFi network name | — |
| `wifi_password` | WiFi password | — |
| `llm_provider` | AI provider (`openai`, `lmstudio`, etc.) | `openai` |
| `llm_model` | Requested model identifier | `gpt-4o` |
| `local_llm_url` | Local provider base URL (LM Studio/Ollama style) | `http://192.168.1.100:1234` |
| `local_llm_model` | Local model name | `llama3` |
| `openai_api_key` | OpenAI API key (sk-...) | — |
| `agent_ws_url` | Custom agent WebSocket URL | — |
| `companion_url` | Companion server WebSocket URL | — |
| `companion_external_api_key` | Bearer token Debbie uses for companion `/api/external/*` routes | — |
| `debbie_name` | Name shown on screen | Debbie |
| `system_prompt` | Debbie's persona prompt | friendly AI companion |
| `speaker_volume` | Speaker volume 0–100 | 75 |

Local provider note: `local_llm_url` is normalized on save (trims whitespace/trailing slash and collapses accidental duplicate dots) to avoid malformed host strings breaking realtime connection.

Node query note: Debbie tool `node_agent_query` calls companion `/api/external/query` using `companion_url`; if that is empty, it falls back to `agent_ws_url`. URL inputs are normalized to trim trailing slashes and strip a trailing `/login` suffix.

Model probe note: `GET /llm_models?provider=openai` uses the configured OpenAI API key and fetches model IDs directly from OpenAI so you can verify cloud connectivity from the device. Error responses now include upstream detail (for example invalid/unauthorized key) to make setup troubleshooting faster.

---

## 🎮 Controls

| Button (right side) | Action |
|--------|--------|
| **Top (mic icon)** | Mic quick action (listen now / AI status prompt) |
| **Middle (arrow icon)** | Cycle launcher menu items |
| **Bottom (power icon)** | Open launcher / activate selected item |
| **Long press** | Read notifications aloud (or show summary if AI is offline) |
| **Voice** | Just speak naturally — Debbie listens automatically |

Freenove FNK0102 note: center/button input is routed via GPIO19 on this firmware profile. Using GPIO45 for center conflicts with the 3.5" LCD DC pin.

Input note: on this board profile, right-side top/middle/bottom keys are decoded from a resistor-ladder input on GPIO19 (ADC) rather than separate GPIO interrupts.

Button mapping note: this build is tuned for three-button use (top mic, middle joystick select, bottom power/action). In ladder mode, right-lane ADC hits are ignored to reduce accidental false top-button triggers.

Startup note: when AI is connected, Debbie now enters listening-ready state on boot/reconnect and immediately accepts speech input.

Input stack note: when ADC ladder mode is enabled, LVGL keypad GPIO polling is disabled to avoid electrical/configuration interference on GPIO19.

Bluetooth audio note: this ESP32-S3 board path is BLE-only and does not provide classic A2DP speaker output in current firmware, so direct Bluetooth speaker audio output is not available yet.

Setup portal note: when BLE runtime is compiled out on this profile, the Network tab shows a small "BLE runtime off" badge and BLE enable control is locked.

Spotify note: Spotify notification controls are intentionally hidden in setup UI while voice and launcher behavior are being stabilised.

Migration note: see [Bluetooth speaker migration path](docs/BLUETOOTH_AUDIO_MIGRATION.md) for practical hardware options and implementation phases.

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Debbie ESP32-S3                           │
│                                                             │
│  ┌──────────┐  PCM24k   ┌────────────────────────────────┐ │
│  │   MEMS   │──────────►│ LLM Realtime Provider Router   │ │
│  │   Mic    │           │ OpenAI Realtime / LM Studio    │ │
│  └──────────┘           └───────────────┬────────────────┘ │
│                                          │ PCM24k           │
│  ┌──────────┐                            ▼                  │
│  │  Camera  │─ JPEG b64 ─► OpenAI vision HTTP (optional)   │
│  └──────────┘                                               │
│                                                             │
│  ┌──────────┐  WebSocket ┌────────────────────────────────┐ │
│  │  Display │◄──────────►│  Companion Server (Node.js)    │ │
│  │  (LVGL)  │  notify    │  WhatsApp │ Email │ Agent/RAG  │ │
│  └──────────┘            └────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Firmware Components

| File | Purpose |
|------|---------|
| `main/main.c` | App entry, state machine, event routing |
| `main/openai_client.c` | Provider-aware realtime client (OpenAI + LM Studio) and OpenAI chat/vision HTTP path |
| `main/audio_manager.c` | I2S mic capture + speaker playback |
| `main/camera_manager.c` | OV2640 capture + JPEG/base64 encoding |
| `main/display_manager.c` | LVGL UI — status, text panel, and diagnostics footer |
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
| `spotify.js` | Spotify Web API controller (runtime paused in current firmware) |
| `agent_bridge.js` | Custom agent WebSocket bridge |

---

## 🔒 Network Security Testing

> ⚠️ **AUTHORISED USE ONLY** — These tools are for testing **your own networks** or networks you have **explicit written permission** to test. Unauthorised network scanning is illegal under the Computer Misuse Act (UK), CFAA (US), and similar laws worldwide.

Debbie includes a full ethical network security toolkit, voice-commanded through the AI.

### What Debbie Can Scan

| Tool | Description |
|------|-------------|
| 🔍 **WiFi Scanner** | Discover all visible networks, check encryption (flags WEP/open APs) |
| 📡 **ARP Host Discovery** | Find all live devices on your subnet |
| 🚪 **Port Scanner** | Scan 65+ common service ports per host |
| 🏷️ **Service Fingerprinting** | Identify running services from banners |
| 🕵️ **Vulnerability Analysis** | Check against 20+ known-dangerous configurations |
| 📊 **Risk Scoring** | 0–100 risk score per device with remediation |
| 🌐 **Web Probe** | Detect HTTP/HTTPS, grab server headers and page titles |
| 🔬 **CVE Lookup** | Query the NVD database for specific CVE details |
| 🗺️ **DNS Lookup** | Resolve hostnames from the device |

### Checks Performed

| Check | Severity |
|-------|---------|
| Open WiFi networks | HIGH |
| WEP encryption (crackable in seconds) | CRITICAL |
| Telnet exposed (plaintext credentials) | HIGH |
| Docker API on port 2375 (no auth) | CRITICAL |
| Redis/MongoDB/Elasticsearch (often unauthenticated) | HIGH |
| Modbus/S7 industrial protocols exposed | CRITICAL |
| VNC remote desktop exposed | HIGH |
| MQTT broker without authentication | MEDIUM |
| SNMP with default community strings | MEDIUM |
| HTTP without HTTPS | LOW |
| OpenSSH version fingerprinting | MEDIUM |

### Voice Commands

Just say it to Debbie — no typing needed:

- *"Scan my network"* → Full scan (WiFi + all devices + ports)
- *"What devices are on my network?"* → Quick host discovery
- *"Scan the WiFi networks nearby"* → WiFi-only scan
- *"Check 192.168.1.1 for vulnerabilities"* → Port scan a specific device
- *"What's the risk score for my router?"* → Risk assessment
- *"Look up CVE-2021-44228"* → Log4Shell CVE details
- *"Give me the security report"* → Full vulnerability summary
- *"What are my highest-risk devices?"* → Prioritised findings
- *"How do I fix the Redis vulnerability?"* → Remediation guidance

### Companion Server nmap Integration

For more advanced scanning, install nmap on the companion server host:

```bash
# Linux
sudo apt install nmap

# macOS
brew install nmap
```

Then request a scan via REST:
```bash
curl -X POST http://localhost:3001/network/scan \
  -H "Content-Type: application/json" \
  -d '{"target": "192.168.1.0/24", "flags": ["-sV", "-O"]}'
```

Generate an HTML report at `http://localhost:3001/network/report`.

### Self-Assessment

Debbie also audits herself:
- TLS certificate verification status
- ESP-IDF firmware version
- Enabled features and potential risks

Say *"Check yourself for vulnerabilities"* or *"Run a self-assessment"*.

---



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

## 🎵 Spotify Status

Spotify integration remains in the companion server and API plumbing for compatibility, but device-side runtime controls are currently paused while core launcher and connectivity flows are stabilised.

If you need mobile audio today, route playback through your phone and keep Debbie focused on conversation, notifications, and network tooling.

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
| LCD MOSI | 21 |
| LCD CLK  | 47 |
| LCD CS   | -1 |
| LCD DC   | 45 |
| LCD RST  | 20 |
| LCD BL   | 2 |
| Mic SCK  | 14 |
| Mic WS   | 13 |
| Mic SD   | 12 |
| Spk BCK  | 17 |
| Spk LRCK | 16 |
| Spk DATA | 15 |
| Cam XCLK | 10 |
| Nav Centre | 19 |
| Nav Up   | 41 |
| Nav Down | 42 |
| Nav Left | 43 (reserved in current profile) |
| Nav Right | 44 (reserved in current profile) |
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
| Bluetooth speaker output | Not supported on ESP32-S3 BLE-only path (no classic A2DP output) |
| Top/bottom buttons not responding | Ensure firmware includes ADC key-ladder decode on GPIO19 and that NAV_CENTER remains mapped to GPIO19 |
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