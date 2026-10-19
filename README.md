# Shapdesk UART sniffer (ESP32-S3 / ESP-IDF)

**Experimental, unofficial, read-only firmware.** Tested at the protocol-parser level against observed Shapdesk UART bytes, **not tested on the actual desk/ESP32 hardware yet**. Intended for ESP-IDF 5.3+ (target `esp32s3`). No Wi-Fi, no motor movement, no commands transmitted.

## What it reads

The documented UART interface uses **9600 baud, 8N1, non-inverted**. UART port names below are **labels printed on the desk handset PCB**, not the ESP32 UART direction.

- **Desk RX pad (yellow):** 7-byte `AA` frame followed by a 6-byte duplicated-pair status. Height = last duplicated byte of status, e.g. `4D 4D` = 77 cm or `5A 5A` = 90 cm. Parsed on ESP32-S3 **UART1 RX GPIO4**.
- **Desk TX pad (light blue):** 6-byte `BB` position updates and 5-byte `D8 D8 66` button states. Parsed on ESP32-S3 **UART2 RX GPIO5**.
- Both UARTs are **receivers only**. No TX GPIO is routed and no call to `uart_write_bytes` is made.

The program logs height **only when it changes**, button events **only on changes**, and a status heartbeat every 5 seconds. The `RX live=no (stale)` indicator means it hasn't received a valid AA+status for >1 second. `SLEEP?`, `PRESET_*?`, `MEMORY?` are hypotheses based on compatible controllers, not confirmed on this physical desk.

## Wiring — important safety prerequisites

**DO NOT connect a 5 V desk UART signal directly to any ESP32-S3 GPIO.** The desk's supply was measured near 4.74 V; the actual high levels of the UART wires haven't been established with a calibrated oscilloscope. Multimeter readings are averages. The USB logic analyzer's alleged 5 V tolerance does NOT extend to the ESP32.

Use a verified **SN74LVC245A**-based buffer (actual chip model matters), powered at **3.3 V**, **A → B** direction, `/OE` active (LOW), with a 100 nF ceramic decoupling capacitor between VCC and GND near the IC. TI specifies inputs tolerant to **5.5 V** and supplies from 1.65 to 3.6 V. Use one common buffer for both lines:

```text
SHAPDESK HANDSET PCB     SN74LVC245A (A -> B)       ESP32-S3

RX (yellow) ------------ A1 ---> B1 -------------- GPIO4 (UART1 RX)
TX (light blue) -------- A2 ---> B2 -------------- GPIO5 (UART2 RX)
                        VCC = ESP32 3V3
                        DIR = 3V3  (A -> B)
                        /OE = GND   (outputs enabled)
                        GND = common signal GND*   ESP32 GND*

NO GPIO output goes to desk RX or TX. No desk 5V to ESP32 3V3.
```

For **SN74LVC245AN PDIP-20** the pins are: DIR=1, A1=2, A2=3, GND=10, B2=17, B1=18, /OE=19, VCC=20. Check the exact pinout of any other package or module before wiring; never infer from breakout labels.

**\*Critical:** Joining desk GND to an ESP32 plugged into a PC via USB is not automatically safe. The mains isolation, ground reference and transient exposure of the desk controller have **not been verified**. Do not connect the grounds or run this setup until you establish that the controller UART is SELV/galvanically isolated from mains and safe to connect to grounded USB equipment; otherwise use a purpose-built galvanically isolated serial interface. Checking DC voltage alone does not prove isolation. Power off and secure insulated probes before making any connections. Do not modify mains/motor circuits. Avoid moving the desk during the first electrical bring-up.

## Flashing (Windows / ESP-IDF)

1. Install [Espressif ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html) with the ESP-IDF Windows installer and open **ESP-IDF PowerShell**.
2. Connect the ESP32-S3 board to USB. Use the correctly labeled pins `GPIO4`, `GPIO5`, `3V3`, `GND`, not physical header positions 4/5.
3. In the project root:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

Replace `COM7` with the port for *your* ESP32-S3. If logs don't appear over the native USB port, configure the console for **USB Serial/JTAG** under `idf.py menuconfig` → `Component config` → `ESP System Settings` → console output, or use your board's USB-to-UART port. Do this before wiring the desk so the boot and monitoring setup is verified independently.

**Expected logs with the desk connected and a valid signal:**

```text
I (...) shapdesk: PASSIVE SNIFFER: 9600 8N1, desk RX -> GPIO4, desk TX -> GPIO5
I (...) shapdesk: HEIGHT 90 cm | raw=5309 | ctrl_command=0x00 | mode=0x00
I (...) shapdesk: PANEL BUTTON UP (0x02)
I (...) shapdesk: PANEL BUTTON RELEASED (0x00)
I (...) shapdesk: STATUS height=90 cm | RX AA+state=... invalid=0 | TX BB+D8=... invalid=0 | panel_button=RELEASED | RX live=yes
```

Without a signal, `STATUS` reports zero good frames. If the desk screen is asleep, this firmware may still receive AA status; that hasn't been verified across every state.

## Run parser tests on a regular computer (no ESP-IDF needed)

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_protocol.c tests/test_parser.c -o test_parser
./test_parser
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
