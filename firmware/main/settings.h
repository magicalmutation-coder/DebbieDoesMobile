#pragma once

/* =============================================================================
 * Debbie — Portable Personal AI Friend
 * Freenove Media Kit ESP32-S3 (FNK0102) — Hardware Pin Definitions
 * =============================================================================
 *
 * Change DEBBIE_DISPLAY_SIZE to match your kit variant:
 *   DEBBIE_DISPLAY_35 → 3.5" 480×320 (ST7796) — recommended
 *   DEBBIE_DISPLAY_114 → 1.14" 135×240 (ST7789)
 */

/* ── Display variant ──────────────────────────────────────────────────────── */
#define DEBBIE_DISPLAY_35   1   /* set to 0 to use 1.14" version */
#define DEBBIE_DISPLAY_114  0

/* ── SPI Display (ST7796 / ST7789) ─────────────────────────────────────────*/
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIN_MOSI        35
#define LCD_PIN_CLK         36
#define LCD_PIN_CS          34
#define LCD_PIN_DC          37
#define LCD_PIN_RST         38
#define LCD_PIN_BL          33
#define LCD_SPI_FREQ_HZ     (40 * 1000 * 1000)

#if DEBBIE_DISPLAY_35
#   define LCD_WIDTH        480
#   define LCD_HEIGHT       320
#   define LCD_DRIVER       "ST7796"
#else
#   define LCD_WIDTH        240
#   define LCD_HEIGHT       135
#   define LCD_DRIVER       "ST7789"
#endif

/* ── Camera (OV2640) ────────────────────────────────────────────────────── */
#define CAM_PIN_PWDN        -1
#define CAM_PIN_RESET       -1
#define CAM_PIN_XCLK        10
#define CAM_PIN_SIOD        21
#define CAM_PIN_SIOC        22
#define CAM_PIN_D7          9
#define CAM_PIN_D6          8
#define CAM_PIN_D5          47
#define CAM_PIN_D4          7
#define CAM_PIN_D3          6
#define CAM_PIN_D2          5
#define CAM_PIN_D1          4
#define CAM_PIN_D0          3
#define CAM_PIN_VSYNC       2
#define CAM_PIN_HREF        1
#define CAM_PIN_PCLK        0

/* ── I2S Microphone (MEMS, e.g. MSM261S4030H0) ─────────────────────────── */
#define I2S_MIC_PORT        I2S_NUM_0
#define I2S_MIC_SCK         14    /* bit clock */
#define I2S_MIC_WS          13    /* word select / LRCK */
#define I2S_MIC_SD          12    /* serial data in */

/* ── I2S Speaker (I2S DAC, e.g. MAX98357) ──────────────────────────────── */
#define I2S_SPK_PORT        I2S_NUM_1
#define I2S_SPK_BCK         17    /* bit clock */
#define I2S_SPK_LRCK        16    /* word select */
#define I2S_SPK_DATA        15    /* serial data out */
#define I2S_SPK_SD_EN       -1    /* shutdown pin (optional) */

/* ── Audio parameters ───────────────────────────────────────────────────── */
#define AUDIO_SAMPLE_RATE   24000 /* Hz — matches OpenAI Realtime API */
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      1
#define AUDIO_BUF_MS        60    /* ms per audio chunk */

/* ── WS2812 RGB LED ─────────────────────────────────────────────────────── */
#define RGB_LED_PIN         40
#define RGB_LED_COUNT       1

/* ── 5-way navigation switch ────────────────────────────────────────────── */
#define NAV_UP              41
#define NAV_DOWN            42
#define NAV_LEFT            43
#define NAV_RIGHT           44
#define NAV_CENTER          45    /* press to capture image / confirm */

/* ── Battery ADC ────────────────────────────────────────────────────────── */
#define BAT_ADC_PIN         4     /* ADC1_CH3 — check your board variant */
#define BAT_DIVIDER_RATIO   2.0f  /* voltage divider (R1=R2) */

/* ── SD Card (SPI) ──────────────────────────────────────────────────────── */
#define SD_PIN_CLK          36    /* shared SPI bus */
#define SD_PIN_MOSI         35
#define SD_PIN_MISO         39
#define SD_PIN_CS           46

/* ── Connectivity defaults ──────────────────────────────────────────────── */
#define DEBBIE_AP_SSID      "Debbie"
#define DEBBIE_AP_PASSWORD  ""    /* open network for first-run setup */
#define DEBBIE_AP_IP        "192.168.4.1"

/* ── OpenAI ─────────────────────────────────────────────────────────────── */
#define OPENAI_REALTIME_HOST   "api.openai.com"
#define OPENAI_REALTIME_PATH   "/v1/realtime?model=gpt-4o-realtime-preview"
#define OPENAI_CHAT_HOST       "api.openai.com"
#define OPENAI_CHAT_PATH       "/v1/chat/completions"
#define OPENAI_VISION_MODEL    "gpt-4o"

/* ── Agent / companion server defaults ──────────────────────────────────── */
#define AGENT_WS_DEFAULT_URL   "ws://YOUR_COMPANION_SERVER:3001"

/* ── Spotify (handled by companion server) ──────────────────────────────── */
/* The device sends commands to the companion server which calls Spotify API */

/* ── NVS namespace ──────────────────────────────────────────────────────── */
#define NVS_NAMESPACE          "debbie"

/* ── LVGL tick rate ─────────────────────────────────────────────────────── */
#define LVGL_TICK_MS           5

/* ── Default system prompt (includes network security capabilities) ───────── */
#define DEBBIE_DEFAULT_SYSTEM_PROMPT \
    "You are Debbie, a warm and knowledgeable AI companion running on a " \
    "portable ESP32-S3 device. You have a friendly, upbeat personality " \
    "and love helping with anything — from daily chat to network security.\n\n" \
    "Your capabilities include:\n" \
    "• Voice conversations (just talk to me!)\n" \
    "• Camera vision — ask me to look at something\n" \
    "• Reading WhatsApp messages and emails\n" \
    "• Controlling Spotify music playback\n" \
    "• Fetching information from the internet\n" \
    "• Network security scanning (say 'scan my network')\n" \
    "  - Discovering all devices on the local network\n" \
    "  - Checking for open ports and vulnerabilities\n" \
    "  - Looking up CVE security advisories\n" \
    "  - Generating security reports\n\n" \
    "IMPORTANT SECURITY ETHICS: Only ever scan networks you own or have " \
    "explicit written permission to test. Always remind the user of this " \
    "when they ask for network scanning. Never assist with illegal activity.\n\n" \
    "Speak naturally and concisely. For security findings, explain them " \
    "clearly in plain English with practical remediation steps."
