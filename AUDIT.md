# Firmware review — 2026-10-16

Revision: 0.13.4-korta4. The MCU sources remain based on pinned ModRetro v4.2. FPGA work remains experimental and excluded from the installer.

## Changes

- Correct LVGL tick units (the old 1 ms callback advanced the clock by 1,000 ms).
- Keep the DMA framebuffer in aligned internal DRAM BSS, avoiding 46,080 zero bytes in flash. A runtime assertion checks DMA capability.
- Losslessly index 19 stock images, saving 127,222 image bytes. Every source pixel and alpha value is checked during conversion. A separate host run through the actual LVGL renderer confirmed all 19 originals and replacements produce identical framebuffers.
- Configure SPI callback completion without an unused transaction return queue.
- Preserve countdown time when paused; handle millisecond rollover; avoid nested alarm/timer return loops; restore flashlight brightness after alerts.
- Suppress buttons held when the overlay opens until released. Preserve the Notes keyboard preference while entering Wi-Fi passwords. Prevent volume-panel shortcuts from navigating the underlying view.
- Show all 120 note characters, clip labels before right-aligned values, and show storage failures in Notes and Files.
- Never erase NVS automatically on settings/radio initialization errors. Debounce preference writes, autosave note edits, and retain dirty preferences after failed writes. Stop treating a cached pre-power-off timestamp as a valid clock.
- Route USB clock updates through an atomic request to the UI task. Keep shell state owned by that task.
- Add Wi-Fi retry scheduling, join timeout, stale-event guards, complete 32-byte SSIDs, initialization cleanup, and atomic power snapshots.
- Wake periodically while alarms/timers are active; resume FPGA transmission on scheduled wake. This needs device validation and battery measurements.
- Disable the experimental continuous I2S task by default because stock FPGA 18.8 cannot mix its output. Experimental builds may define KORTA_EXPERIMENTAL_AUDIO=1; audio requests then use an atomic mailbox.
- Pin complete size-optimized build defaults. Add main/PR CI for host tests and an ESP-IDF v5.3 clean firmware build.
- Validate application partition capacity. Backups include a checksum and device MAC record; reject damaged or foreign backups before restore writes. Incomplete backups are removed and private permissions are used.

## Verification and limits

Host behavior tests pass under AddressSanitizer and UndefinedBehaviorSanitizer, including 20,000 randomized navigation actions and regression cases. Installer recovery tests cover backup failure, write ordering, corruption, foreign-device rejection, and successful verification. A clean upstream build passed before image optimization; the final optimized ESP32 build also passes and leaves about 24% of the 1 MiB factory partition free.

This is a source/build review, not proof of zero defects. This revision has not been flashed or tested on the handheld. Wi-Fi association/reconnect, scheduled sleep/wake, Menu/input ownership, power draw, and physical restoration still need hardware checks. SD access, BLE, IR, boot-to-Home, and independent interface audio remain unimplemented. The published FPGA experiment has not been synthesized or validated. Preference saves are debounced by one second; sudden power loss before commit can lose the latest edit.
