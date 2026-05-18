# Debbie Device Bring-up and Troubleshooting

This guide captures the validated bring-up flow for the Freenove FNK0102 board profile used in this repository, plus the exact failure signatures seen during live debugging.

## 1. Known-good display profile (FNK0102B 3.5")

Current firmware profile:
- Panel: ST7796-compatible init sequence via ESP-IDF ST7789 panel driver
- Resolution: 480x320 (landscape)
- SPI host: SPI2_HOST (shows as host=1 in logs)
- Pins:
  - MOSI: 21
  - SCLK: 47
  - DC: 45
  - RST: 20
  - BL: 2
  - CS: -1
- Orientation flags:
  - swap_xy=true
  - mirror_x=false
  - mirror_y=false
  - invert=true

Expected boot markers:
- display profile: driver=ST7796 host=1 mosi=21 sclk=47 dc=45 rst=20 bl=2 cs=-1 freq=40000000
- Display ready — ST7796 480x320

## 2. Build and flash workflow (Windows)

From firmware folder:

```powershell
. C:\esp\v6.0.1\esp-idf\export.ps1
idf.py build
idf.py -p COM8 flash
idf.py -p COM8 monitor
```

If port is unknown:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

## 3. Runtime state checklist

After power-on or reset, verify these in monitor output:
- Display initializes without esp_lcd errors
- No panic/assert lines
- WiFi mode is expected for your setup:
  - First run: softAP with http://192.168.4.1
  - Configured WiFi: station connected with DHCP IP

## 4. Confirmed issues and signatures

### 4.1 Screen appears to flash/reboot loop

Observed signature:
- BTU_StartUp Unable to allocate resources for bt_workqueue
- assert failed: list_begin list.c:262 (list != NULL)
- Rebooting...

Root cause:
- BLE stack startup instability on this hardware/IDF combination in current runtime profile.

Current mitigation in firmware:
- BLE default disabled
- Runtime BLE init guarded by DEBBIE_ENABLE_BLE_RUNTIME=0

How to confirm mitigation is active:
- Log contains: BLE runtime disabled (DEBBIE_ENABLE_BLE_RUNTIME=0)
- No BTU_StartUp/assert lines

### 4.2 Center button does nothing

Observed signature:
- display_manager_init: skipping NAV_CENTER pin 45 (reserved/conflict)

Root cause:
- NAV_CENTER previously mapped to a conflicted pin.

Current mitigation in firmware:
- NAV_CENTER set to GPIO19 in settings profile.

How to confirm mitigation is active:
- No NAV_CENTER conflict warning in boot logs

### 4.3 Camera init fails

Observed signature:
- camera: Camera probe failed with error 0x102(ESP_ERR_INVALID_ARG)
- sccb init err
- Screen can appear normal at boot, then blank shortly after camera init starts

Likely cause:
- Camera pin map overlaps display/backlight pins in the active 3.5" profile
  (for example camera signals currently include GPIO21/47/2, which are also
  used by LCD MOSI/SCLK/BL).

Current mitigation in firmware:
- Runtime camera init/capture guarded by DEBBIE_ENABLE_CAMERA_RUNTIME=0.
- Backlight GPIO is driven high and held (`gpio_hold_en`) after display init to prevent runtime pin interference.

How to confirm mitigation is active:
- Log contains: Camera requested but runtime is disabled (DEBBIE_ENABLE_CAMERA_RUNTIME=0)
- No camera probe/SCCB init error lines during normal boot.

### 4.4 Crash after saving WiFi credentials from setup portal

Observed signature:
- ESP_ERROR_CHECK failed: esp_err_t 0x3005 (ESP_ERR_WIFI_MODE)
- Backtrace points to try_connect in wifi_manager.c

Root cause:
- Reconnect path attempted STA config while WiFi was still in AP-only mode.

Current mitigation in firmware:
- try_connect now checks current mode and transitions AP -> APSTA before STA config.
- Runtime WiFi calls now return/log errors instead of aborting with ESP_ERROR_CHECK.

How to confirm mitigation is active:
- After saving credentials, no ESP_ERR_WIFI_MODE panic/reboot.
- If connect fails, device remains responsive and logs a warning instead of abort.

## 5. Setup portal behavior

If no WiFi credentials are saved, firmware intentionally enters setup mode:
- AP SSID: Debbie
- URL: http://192.168.4.1

This is expected and not a crash condition.

### 5.1 If you can join Debbie SSID but portal does not open

Try these checks in order:
1. Disable mobile data and VPN on the phone while connected to Debbie SSID.
2. Open plain HTTP URL exactly: `http://192.168.4.1/` (not HTTPS).
3. Verify phone WiFi details show gateway `192.168.4.1` and client IP `192.168.4.x`.
4. Forget Debbie SSID and reconnect, then retry.

Runtime assist:
- Display footer now shows current AP URL, STA IP, and AI/provider status.

### 5.2 Voice/LLM appears offline while WiFi is connected

Observed signature:
- `mbedtls_ssl_setup returned -0x008D`
- `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED`

Current mitigations in firmware:
- OpenAI key input is trimmed (handles pasted trailing whitespace/newlines).
- WebSocket TLS now explicitly attaches ESP certificate bundle.
- OpenAI connect path logs current heap/largest block to aid memory-related TLS diagnosis.

## 6. Operational test script

After flashing:
1. Watch first 15-20 seconds of monitor output.
2. Confirm display ready line appears.
3. Confirm no panic/assert/reboot cycle.
4. Press center button and verify state transition output/UI change.
5. Configure WiFi from portal if running AP-only mode.
6. Save WiFi credentials and confirm there is no ESP_ERR_WIFI_MODE abort.

## 7. Recovery quick actions

If device boot loops:
1. Flash latest firmware from this repo.
2. Erase NVS only if needed:
   ```powershell
   idf.py -p COM8 erase-flash
   idf.py -p COM8 flash
   ```
3. Re-test with monitor attached.

If display is corrupted:
1. Re-check display profile line in logs.
2. Confirm wiring/pin profile in main/settings.h.
3. Reflash and retest.
