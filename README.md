# KortaMod for ModRetro Chromatic

KortaMod adds a compact app launcher and utilities to the Chromatic's Menu screen. This release is for the **Chromatic v4.2 firmware generation with the ESP32 MCU app `0.13.4` and stock FPGA `18.8`**. It has been installed on one physical Chromatic. The installer checks the USB device, MCU version, and partition layout before writing.

## Install in three steps

1. Download the ZIP for your laptop from [Releases](https://github.com/zkortam/KortaMod/releases/latest): **macOS Apple Silicon**, **macOS Intel**, **Windows**, or **Linux**. Unzip it.
2. Plug your Chromatic into the laptop with a USB data cable. Close any serial monitor or updater using it.
3. Run **Install.command** (Mac), **Install.bat** (Windows), or **Install.sh** (Linux). Keep the window open until it says **Installed**. On macOS, if Gatekeeper blocks the unsigned community installer, Control-click the blocked executable and choose Open. On Windows, SmartScreen may show **More info → Run anyway**. On Linux, your account may need serial port permission; the installer will report a port error if access is denied.

The installer saves a complete backup to `Documents/KortaMod Backups` **before** flashing. Keep that backup private; it can contain your settings and Wi-Fi credentials. Installation writes only the MCU application at `0x10000` and verifies it. It leaves the bootloader, partition table, FPGA, and your saved settings in place. A full backup takes a few minutes; do not unplug during the process.

Press the physical **Menu** button on the Chromatic to open KortaMod after it restarts. The firmware lives in the Menu overlay, so the normal cartridge display is still the default outside Menu.

## Controls

| Control | Action |
| --- | --- |
| D-pad | Move through pages, apps, menus, and the keyboard |
| A | Open or select |
| B | Back; from Home, return to ModRetro's Status tab |
| Start | Quick Controls, or finish text input |
| Left from the first Home tile | Explore/Search |
| Right from Explore | Return to Home |
| Volume wheel, then A while its bar is visible | Open the volume panel |
| Start → Volume | Open the volume panel without using the wheel |

The wheel controls **Media**. The volume panel's Left/Right controls adjust **System** or **Ring** independently; Up/Down chooses the row. Media is shown for reference and follows the wheel. B closes the panel.

Home has two pages. Page 1 has Game, Notes, Clock, Calc, Files, and Settings. Page 2 has Dice, Coin, Count, Flash, Convert, Color, and Player. Notes persist on the device. Explore searches apps with the on-screen keyboard.

## Restore your Chromatic

Use the backup made by the installer **for that same physical unit**. On Mac, open **Restore.command** and select the backup. On Windows, drag the backup `.bin` file onto **Restore.bat**. On Linux, run `./Restore.sh path/to/backup.bin`.

The equivalent terminal command is:

```sh
./KortaMod-Installer --restore "$HOME/Documents/KortaMod Backups/chromatic-mcu-YYYYMMDD-HHMMSS.bin"
```

On Windows, run `KortaMod-Installer.exe --restore "C:\Users\YOUR_NAME\Documents\KortaMod Backups\chromatic-mcu-YYYYMMDD-HHMMSS.bin"` in Command Prompt. This rewrites and verifies the full 4 MiB MCU flash, including that unit's original settings. Keep the handheld plugged in until completion.

## Compatibility and present limits

- The public image is **MCU only**. FPGA `18.8` remains stock. KortaMod temporarily mutes the shared analog game output while its Menu screen is open. True independent game and interface sound mixing requires the unfinished FPGA work in `source/fpga/`.
- The stock FPGA owns the physical Menu and cartridge path. KortaMod is a Menu tab, not a boot replacement.
- Files and Player are early interfaces. SD card file access, cartridge save management, Bluetooth, IR, and boot-to-Home are not yet implemented.
- The app version and partition checks stop installation on unknown firmware. A future release must be built and tested for each additional hardware and firmware combination.

## Build from source

`source/mcu/changes.patch` contains the tracked changes to [ModRetro's MCU `v4.2` source](https://github.com/ModRetro/oss-chromatic-console-mcu/tree/v4.2); `source/mcu/extra/` contains the added KortaMod C files. The exact upstream MCU commit is `6c94613da72420f6a364e0024681401bd69184ba`. With [ESP-IDF v5.3](https://github.com/espressif/esp-idf/tree/v5.3) installed and activated, run:

```sh
bash source/mcu/build.sh
```

The output is `source/mcu/build-tree/build/mcu_fw.bin`. `source/mcu/test_core.c` contains host-side behavior tests:

```sh
cc -std=c11 -Wall -Wextra -Werror -I source/mcu/extra/components/osd source/mcu/extra/components/osd/kortamod_core.c source/mcu/test_core.c -o /tmp/kortamod-test
/tmp/kortamod-test
```

`source/fpga/` holds experimental, **unbuilt and unflashed** audio mixer changes against ModRetro's FPGA commit `a01aa375147943cbcd4a41f41585220d55e0c28a`. They are published for development, not included in the installer. Building that project requires the Gowin toolchain and license.

The prebuilt MCU app SHA-256 is `855460c8b49dafe0f93ad48025d43ec802cf0f7799afaa66ad01e792a4f9e822`. The installer pins and verifies this hash. `firmware/partition-table.bin` is used for a read-only compatibility comparison.

## Work on the installer

The installer is Python and uses Espressif's `esptool`. To run it from source:

```sh
python3 -m pip install -r requirements.txt
python3 installer.py --check
python3 installer.py
```

Use `--port PORT` only when more than one Chromatic is connected. `--check` performs no write. Release ZIPs are built with PyInstaller by `.github/workflows/release.yml` from this repository.

## License and credits

GPL-3.0, matching the upstream ModRetro MCU firmware. See [LICENSE](LICENSE). KortaMod is an independent community modification and is not an official ModRetro firmware release. Thanks to ModRetro for publishing the Chromatic MCU and FPGA source.
