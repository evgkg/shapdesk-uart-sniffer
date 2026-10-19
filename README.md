# Shapdesk UART sniffer (ESP32-C3 / ESP-IDF)

**Experimental, unofficial, read-only firmware.** Tested at the protocol-parser level against observed Shapdesk UART bytes, **not tested on the actual desk/ESP32 hardware yet**. Configured for ESP-IDF 6.1 (target `esp32c3`). A subsequent user-provided VS Code ESP-IDF log confirms a successful ESP32-C3 build; live firmware operation remains unverified. No Wi-Fi, no motor movement, no commands transmitted.

## What it reads

The documented UART interface uses **9600 baud, 8N1, non-inverted**. UART port names below are **labels printed on the desk handset PCB**, not the ESP32 UART direction.

- **Desk RX pad (yellow):** 7-byte `AA` frame followed by a 6-byte duplicated-pair status. Height = last duplicated byte of status, e.g. `4D 4D` = 77 cm or `5A 5A` = 90 cm. Parsed on ESP32-C3 **UART1 RX GPIO4**.
- **Desk TX pad (light blue):** leave disconnected in this version. Its `BB` position updates and `D8` button states are not monitored; the reusable parser still supports these formats for offline tests.
- Only **UART1 RX GPIO4** is initialized. No TX GPIO is assigned and no UART transmission API is called. ESP32-C3 has no UART2.

The program logs height **only when it changes**, `CTRL_COMMAND` independently on command changes, and a status heartbeat every 5 seconds. The `RX live=no (stale)` indicator means it hasn't received a valid AA+status for >1 second. STATUS includes the last command, parser `invalid`/`skipped` counters and diagnostic anomaly count. The raw AA command field is not a complete handset button decoder.

`height_cm` follows the displayed value and may briefly differ from physical height during preset activation. Suspected jumps produce `DIAG BEFORE/ANOMALY/AFTER` lines with exact AA/state bytes and all decoded fields. The default heuristic is a height jump of at least 5 cm with raw-position change at most 32, with four preceding and four following valid messages. See PROTOCOL.md section 3.4 for limits and interpretation; the firmware does not classify the transient value as a target or suppress it.

## Wiring — important safety prerequisites

**DO NOT connect a 5 V desk UART signal directly to any ESP32-C3 GPIO.** The desk's supply was measured near 4.74 V; the actual high levels of the UART wires haven't been established with a calibrated oscilloscope. Multimeter readings are averages. The USB logic analyzer's alleged 5 V tolerance does NOT extend to the ESP32.

Use a verified **SN74LVC245A**-based buffer (actual chip model matters), powered at **3.3 V**, **A → B** direction, `/OE` active (LOW), with a 100 nF ceramic decoupling capacitor between VCC and GND near the IC. TI specifies inputs tolerant to **5.5 V** and supplies from 1.65 to 3.6 V. Use one buffer channel for the RX tap; keep unused buffer inputs at defined levels according to the component datasheet:

```text
SHAPDESK HANDSET PCB     SN74LVC245A (A -> B)       ESP32-C3

RX (yellow) ------------ A1 ---> B1 -------------- GPIO4 (UART1 RX)
TX (light blue) -------- leave disconnected
                        VCC = ESP32 3V3
                        DIR = 3V3  (A -> B)
                        /OE = GND   (outputs enabled)
                        GND = common signal GND*   ESP32 GND*

NO GPIO output goes to desk RX or TX. No desk 5V to ESP32 3V3.
```

For **SN74LVC245AN PDIP-20** the pins are: DIR=1, A1=2, A2=3, GND=10, B2=17, B1=18, /OE=19, VCC=20. Check the exact pinout of any other package or module before wiring; never infer from breakout labels.

**\*Critical:** Joining desk GND to an ESP32 plugged into a PC via USB is not automatically safe. The mains isolation, ground reference and transient exposure of the desk controller have **not been verified**. Do not connect the grounds or run this setup until you establish that the controller UART is SELV/galvanically isolated from mains and safe to connect to grounded USB equipment; otherwise use a purpose-built galvanically isolated serial interface. Checking DC voltage alone does not prove isolation. Power off and secure insulated probes before making any connections. Do not modify mains/motor circuits. Avoid moving the desk during the first electrical bring-up.

## Build and USB console (Windows / ESP-IDF 6.1)

Open the **ESP-IDF 6.1 PowerShell environment installed by your ESP-IDF installer**. With the EIM installer, use its generated environment profile rather than assuming a standalone `export.ps1` will locate the tools. In the project root:

```powershell
idf.py set-target esp32c3
idf.py build
```

`CMakeLists.txt` defaults to `esp32c3`. Tracked `sdkconfig.defaults` selects the native **USB Serial/JTAG** console. Existing `sdkconfig` files can retain old settings; `set-target` regenerates configuration from defaults. Verify `CONFIG_IDF_TARGET="esp32c3"` and `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` before building.

Use the board connector wired to native USB (C3 GPIO18 = D?, GPIO19 = D+); leave these pins available for USB. A connector wired only to a USB-to-UART bridge will not carry this console. The user-provided device detection log confirms USB Serial/JTAG on COM3. After a separately approved flash, monitor the native USB serial port with `idf.py -p <PORT> monitor`; the previously observed COM3 is not guaranteed to be that port. Verify boot logs before connecting the desk.

**No flashing is authorized by these build steps.** Firmware compilation and on-board USB/desk operation are separate checks. On 2026-10-19, the agent's initial build attempt was blocked by the sandbox. A subsequent user-provided VS Code ESP-IDF 6.1 log confirms successful C3 compilation, linking and image generation: `shapdesk_uart_sniffer.bin`, size `0x29d20` bytes, with 84% of the app partition free. Device detection identified an ESP32-C3 revision v0.4 with 4 MB embedded flash and USB Serial/JTAG on COM3. The extension reports `Flash Done`; detailed flash write/verification output and runtime monitor logs were not included. Live console and desk decoding still require verification.

**Expected logs with the desk connected and a valid signal:**

```text
I (...) shapdesk: PASSIVE SNIFFER: 9600 8N1, desk RX -> UART1 GPIO4
I (...) shapdesk: HEIGHT 90 cm | raw=5309 | ctrl_command=0x00 | mode=0x00
I (...) shapdesk: STATUS height=90 cm | RX AA+state=... invalid=0 | RX live=yes
```

Without a signal, `STATUS` reports zero good frames. If the desk screen is asleep, this firmware may still receive AA status; that hasn't been verified across every state.

## Run parser tests on a regular computer (no ESP-IDF needed)

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_protocol.c tests/test_parser.c -o test_parser
./test_parser
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_protocol.c main/desk_diagnostics.c tests/test_diagnostics.c -o test_diagnostics
./test_diagnostics
```

The parser is implemented in plain C, independent of ESP-IDF, and rejects frames with invalid checksums or broken duplicated pairs. After stream errors, it scans for the next start byte. `AA` (7 bytes) and status (6 bytes) are handled as one **13-byte parsing unit** because the real captures have them consecutively; this is a convenience for parsing, not an assertion that there is no gap between UART transmissions.

## Next steps

- Test live passive decoding with proper electrical interfacing.
- Capture more button presets and wake-up behavior; decode frame fields.
- Design a separate, fail-safe UART bridge **before** implementing any motor commands. A second transmitter must never be wired onto the stock TX line in parallel.
- Only then add ESPHome / Home Assistant support.

This repo is not affiliated with Shapdesk, TiMOTION, or Espressif. The code is provided without warranty and does not replace any desk safety system.

## Replaying the seven recorded captures (development verification)

The parser was additionally exercised on decoded UART bytes from the user's seven `.sr` captures. Counts recovered from the files:

- `первая кнопка стол вверх`: 175 `BB` and 17 `D8` packets.
- `вторая кнопка стол вниз`: 176 `BB` and 17 `D8` packets.
- `077 090`: 352 `BB` and 34 `D8` packets.
- `дисплей офф`: 1413 `BB` and 107 `D8` packets.
- `rx в покое`: 353 paired `AA`/state messages, stable at 67 cm.
- `rx вверх`: 345 paired messages, height 67→77 cm; seven malformed candidates were rejected/recovered after decoding the raw waveform.
- `rx90`: 176 paired messages, stable at 90 cm.

These counts verify **the parser with captured streams**, not ESP-IDF firmware compilation or electrical operation on the desk. The original `.sr` files are **not redistributed** in this project.
