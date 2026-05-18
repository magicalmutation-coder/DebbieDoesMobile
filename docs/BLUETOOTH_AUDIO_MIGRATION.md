# Bluetooth Speaker Output Migration Path

This project currently runs on ESP32-S3 hardware where classic Bluetooth (BR/EDR) is not available. That means A2DP speaker output is not possible on this board profile.

## Current state

- BLE support in firmware is profile-gated and currently disabled by default (`DEBBIE_ENABLE_BLE_RUNTIME=0`) on this setup.
- Bluetooth speaker output (classic A2DP source to a speaker) is not available in the current runtime path.

## Practical options

### Option A: Keep existing hardware (recommended short term)

- Keep wired I2S speaker output on Debbie.
- Use phone hotspot as mobile upstream.
- Keep music playback on phone and let Debbie handle voice, notifications, and scanning.

Why: no hardware redesign, lowest risk, immediate usability.

### Option B: Migrate to a classic-Bluetooth capable ESP32 target

- Move to an ESP32 variant that supports classic BT (BR/EDR).
- Port display, audio, camera, and button pin maps for the new board.
- Implement A2DP source path for speaker pairing and stream routing.

Typical firmware work items:

- Add classic BT stack init/deinit lifecycle.
- Add pair/connect/disconnect state machine and persistence.
- Bridge PCM output path from existing audio pipeline to A2DP source encoder path.
- Add setup-portal pairing status and controls.

### Option C: External audio bridge module

- Keep ESP32-S3 main board.
- Add an external module dedicated to Bluetooth audio transmitter role.
- Feed audio from Debbie via I2S/UART bridge to the external module.

Why: preserves existing Debbie hardware and display path, but adds BOM and integration complexity.

## Suggested phased execution

1. Stabilize current mobile workflow (hotspot route, LM Studio/OpenAI provider reliability).
2. Validate desired Bluetooth UX (pairing flow, reconnect behavior, fallback behavior).
3. Choose migration track (B or C) based on PCB constraints and BOM target.
4. Implement audio path integration and setup UX.
5. Run battery and latency profiling before enabling by default.

## Acceptance criteria for Bluetooth speaker support

- Device can pair and auto-reconnect to a known speaker after reboot.
- Runtime can recover from speaker disconnect without reboot.
- Audio latency remains acceptable for voice responses.
- Setup portal clearly reports Bluetooth audio state and errors.
- Existing wired speaker path can be retained as fallback mode.
