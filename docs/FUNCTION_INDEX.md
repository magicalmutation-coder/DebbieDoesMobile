# Debbie Function Index

This index lists public module APIs in firmware/main for quick navigation.

## Runtime feature switches

### settings.h
- DEBBIE_ENABLE_BLE_RUNTIME (currently 0 in this profile)
- DEBBIE_ENABLE_CAMERA_RUNTIME (currently 0 in this profile)
- DEBBIE_ENABLE_SPOTIFY_RUNTIME (currently 0 in this profile)

## Core application

### main.c
- app_main
- set_state (internal helper)
- on_oai_event (OpenAI callback)
- on_notification (companion callback)
- on_audio_capture (audio capture callback)
- gpio_init / button_task / battery_task (runtime tasks)

Behavior note:
- Runtime now posts network summaries (AP URL, STA IP, saved SSIDs, AI state/provider) into chat on boot and link-status changes.
- Three-button launcher navigation is available on-device for network route help, hotspot setup guidance, vulnerability scan launch, and notification summary.
- On FNK0102 profiles using the right-side key ladder, button events are decoded from ADC readings on GPIO19 to map top/middle/bottom presses reliably.
- OpenAI runtime now surfaces a specific on-device error when upstream auth rejects the API key, so voice setup failures are easier to diagnose.
- Top button now always performs mic quick action (even if launcher menu is open); middle cycles menu items; bottom opens/activates selected menu item.
- On OpenAI connection, Debbie now enters listening-ready state immediately so speech-to-text can start without extra input.
- Right-lane ladder key hits are ignored in 3-button mode to reduce accidental top-button triggers from ADC transitions.

## Display

### display_manager.h
- display_manager_init
- display_manager_set_state
- display_manager_show_notification
- display_manager_show_text
- display_manager_show_camera_frame
- display_manager_set_notif_count
- display_manager_set_spotify_track
- display_manager_set_battery
- display_manager_set_connection_status
- display_manager_set_network_info

Behavior note:
- Backlight GPIO is held high after display init to prevent runtime pin-mode interference.
- When `DEBBIE_USE_ADC_NAV_LADDER=1`, LVGL keypad GPIO registration is skipped so GPIO19 ladder decoding in `main.c` is the only input path.
- Bottom footer now shows network diagnostics only (AP/STA/AI). Spotify now-playing footer visuals were removed.
- `display_manager_set_spotify_track` is retained as a compatibility no-op.

## Audio

### audio_manager.h
- audio_manager_init
- audio_manager_start_capture
- audio_manager_stop_capture
- audio_manager_play_pcm
- audio_manager_beep
- audio_manager_set_volume
- audio_manager_vad

## Camera

### camera_manager.h
- camera_manager_init
- camera_manager_capture_jpeg
- camera_manager_free_frame
- camera_manager_capture_base64
- camera_manager_set_power

## WiFi

### wifi_manager.h
- wifi_manager_init
- wifi_manager_is_connected
- wifi_manager_reconnect
- wifi_manager_set_credentials
- wifi_manager_get_sta_ip
- wifi_manager_get_ap_ip

Behavior note:
- `wifi_manager_reconnect` safely handles AP-only setup mode by moving to AP+STA before applying STA config.

## Bluetooth

### bluetooth_manager.h
- bluetooth_manager_init
- bluetooth_manager_deinit
- bluetooth_manager_notify
- bluetooth_manager_is_connected

Behavior note:
- Current ESP32-S3 profile is BLE-only and runtime-disabled by default (`DEBBIE_ENABLE_BLE_RUNTIME=0`); Bluetooth speaker audio output via classic A2DP is not available in this build.

## Storage and configuration

### storage_manager.h
- storage_init
- storage_save_config
- storage_factory_reset

Behavior note:
- NVS config now includes `ext_api_key` for companion external API bearer auth used by Debbie tool calls.

## OpenAI client

### openai_client.h
- openai_client_connect
- openai_client_disconnect
- openai_client_send_audio
- openai_client_commit_audio
- openai_client_send_text
- openai_client_send_image
- openai_client_send_function_result
- openai_client_is_connected

Behavior note:
- Local provider realtime URI construction now normalizes malformed base URLs (trims whitespace/trailing slash and collapses duplicate dots) before opening the WebSocket.
- Reconnect path now tears down existing websocket handles before creating a new client and returns errors instead of aborting the firmware on websocket start failures.
- Session tool registration now merges `AGENT_TOOLS_JSON` so self-agent tools (including `node_agent_query`) are available to the live runtime model.

### OpenAI events (openai_client.h)
- OAI_EVT_CONNECTED
- OAI_EVT_DISCONNECTED
- OAI_EVT_AUDIO_DELTA
- OAI_EVT_TRANSCRIPT
- OAI_EVT_USER_TRANSCRIPT
- OAI_EVT_FUNCTION_CALL
- OAI_EVT_SESSION_CREATED
- OAI_EVT_ERROR

## Notifications / companion server

### notification_client.h
- notification_client_init
- notification_client_deinit
- notification_client_is_connected
- notification_client_unread_count
- notification_client_clear
- notification_client_get_summary_json
- notification_client_spotify_command

Behavior note:
- Spotify command plumbing remains in the API for compatibility, but runtime Spotify controls are currently gated off.
- External HTTP integrations should use `Authorization: Bearer <external_api_key>` for `/api/external/*` routes; handoff details are documented in `docs/EXTERNAL_API_HANDOFF.md`.
- Companion server external API now includes `/api/external/health`, `/api/external/events`, `/api/external/query`, `/api/external/whatsapp/status`, plus `/api/external/key/{status,rotate}`.
- Companion `/api/external/query` now supports AGENT_URL in two modes: WebSocket bridge (`ws://` / `wss://`) and HTTP forwarding (`http://` / `https://` to downstream `/api/external/query`).

Integration artifacts:
- `docs/EXTERNAL_API_HANDOFF.md` (human-readable contract)
- `docs/EXTERNAL_API.postman_collection.json` (ready-to-import test collection)
- `external-api-handoff/README.md` (share-pack overview)
- `external-api-handoff/EXTERNAL_API.postman_collection.json` (partner-ready collection)
- `external-api-handoff/EXTERNAL_API.postman_environment.json` (partner-ready environment)

## Setup portal / web routes

### web_server.c
- GET `/status`
- POST `/configure`
- POST `/reset`
- GET `/snapshot`
- GET `/llm_models` (provider model discovery for Ollama / LM Studio / OpenAI)
- GET `/memory_stats`
- POST `/memory_clear`

Behavior note:
- Bluetooth controls remain visible in setup, but profiles built with `DEBBIE_ENABLE_BLE_RUNTIME=0` lock BLE enablement off and show guidance that Bluetooth speaker audio output is unsupported on this ESP32-S3 path.
- The Network tab now shows a small "BLE runtime off" badge when BLE runtime is compiled out on this profile.
- `/status` now includes `local_llm_model`, and `/configure` normalizes `local_llm_url` plus preserves local model selection separately from cloud-provider model fields.
- `/configure` now accepts `ext_api_key` for companion external API bearer auth and sanitizes optional pasted `Bearer ` prefix.
- `/configure` now sanitizes `openai_api_key` input (trims whitespace, strips accidental `Bearer ` prefixes, and truncates pasted trailing text).
- `/llm_models?provider=openai` performs an HTTPS request to OpenAI `/v1/models` using the configured `openai_api_key` and returns richer upstream failure detail for auth/setup troubleshooting.
- Spotify notification controls are hidden in setup UI while runtime Spotify support remains disabled.

## Web server

### web_server.h
- web_server_start
- web_server_stop

## Memory and RAG

### memory_manager.h
- memory_manager_init
- memory_manager_save
- memory_manager_clear
- memory_manager_add_turn
- memory_manager_get_turns
- memory_manager_turn_count
- memory_manager_save_fact
- memory_manager_get_facts
- memory_manager_build_context
- memory_manager_enrich_prompt
- memory_manager_query_rag
- memory_manager_sync_turn

## Network scanner

### network_scanner.h
- net_scanner_init
- net_scanner_wifi_scan
- net_scanner_arp_scan
- net_scanner_port_scan
- net_scanner_full_scan
- net_scanner_ping
- net_scanner_get_results
- net_scanner_results_to_json
- net_scanner_get_summary
- net_scanner_cancel

## Vulnerability reporting

### vuln_reporter.h
- vuln_reporter_analyse
- vuln_reporter_to_json
- vuln_reporter_to_text
- vuln_reporter_get_voice_summary
- vuln_reporter_self_assess

## Self-agent tools

### self_agent.h
- self_agent_get_system_info
- self_agent_http_get
- self_agent_dns_lookup
- self_agent_cve_lookup
- self_agent_handle_function_call
- AGENT_TOOLS_JSON (exported tool schema string)

Behavior note:
- Tool schema now includes `node_agent_query`, allowing Debbie to POST text/session payloads to companion `/api/external/query` with configured bearer auth.
- `node_agent_query` URL build now prefers `companion_url` and falls back to `agent_ws_url` when companion URL is not set, converting ws/wss schemes to http/https and trimming trailing `/login` when present.
