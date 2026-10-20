# Shapdesk UART sniffer (ESP32-C3 / ESP-IDF)

**Experimental, unofficial firmware: passive UART monitoring plus bounded UP-button emulation.** ESP32-C3 / ESP-IDF 6.1. Explicit USB `up` commands can move the desk using GPIO5 and the stock handset's UP contact. UART remains receive-only; no Wi-Fi or UART commands. The new control firmware has compiled but has not been flashed or physically tested by the agent.

## UP contact and USB commands

The owner reports this circuit physically tested, including wake from sleep:

```text
GPIO5 -- 4.7 kOhm -- 2N3904 base -- 100 kOhm -- GND
2N3904 collector -> handset UP CH2
2N3904 emitter   -> handset UP CH1
```

HIGH presses UP, LOW releases the electronic contact. This is separate from the UART wiring below. Preserve stock buttons and controller protections. `stop` releases only the emulated contact: it cannot override a physically held button or guarantee stopping a stock automatic preset.

Commands are case-sensitive; terminate with Enter (CR/LF/CRLF). There is no input echo/line editor. Control characters, including backspace, invalidate that line: submit it and retype.

| Command | Behavior |
|---|---|
| `up 300` | Press for nominally 300 ms, then automatically release |
| `up 5000` | Press for up to 5 seconds, then release (guards may release earlier) |
| `up 50` / `up 5000` | Inclusive duration boundaries (50 ms to 5 seconds) |
| `stop` | Immediately release when this received line is processed; safe when idle |
| `status` | Existing desk/parser/UART summary plus GPIO5 level, active state, remaining time and dropped button-log count |

Invalid durations, signs, extra arguments, unknown commands and lines longer than 31 bytes are rejected. A second `up` during a press is rejected as busy without extending its deadline. No movement is queued or generated on boot/reconnect.

GPIO5's output latch is LOW before output is enabled, as the first application initialization step. The external 100 kOhm pulldown holds the base off before app_main/during reset. A normal software restart also invokes a release handler.

A GPTimer ISR checks the absolute deadline every 1 ms and sets GPIO5 LOW before queuing an end log, independently of USB reads, command processing and log printing. Cache-safe GPTimer and IRAM GPIO functions are mandatory build settings. Timing has 1 ms quantization plus interrupt latency; pulse width and reset behavior require hardware verification. Software cannot guarantee release during total CPU/interrupt failure or transistor/electrical failure; a hard bound under those faults requires an external hardware one-shot/watchdog.

**Current button-only test mode:** UART feedback interlocks are disabled at the owner's request (`DESK_UP_REQUIRE_UART_FEEDBACK=0` in `desk_button_logic.h`). No RX bytes or UART errors will shorten `up 5000`; the sniffer and its error counters continue operating independently. Set the switch to 1 and rebuild to restore release on 500 ms valid-RX silence (with initial wake grace) and UART transport errors. This bench mode provides no desk-feedback interlock. USB disconnect release, `stop`, the 5-second absolute deadline, busy-command rejection and boot LOW remain active. Closing only the serial terminal may leave USB SOF packets present; the deadline remains effective.

USB input runs in its own task, yields under command floods, and does not block UART RX. `status` notifies the UART owner to take the existing snapshot. Button events use a bounded nonblocking queue consumed by the existing logger; dropped logs are counted, and release does not depend on logging.

## Operator build, flash and manual check

From the installed **ESP-IDF 6.1 PowerShell**, in the repository root:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash
idf.py -p COM3 monitor
```

`COM3` is the previously detected native USB Serial/JTAG port; replace it if it changed. Close other monitors before flashing. Flash commands above are **operator instructions**, not actions executed by the agent.

1. Before UP, verify no motion on boot/reset. Enter `status`; expect `BUTTON GPIO5=0 active=no`.
2. Enter `up 49`, `up 5001`, `up -300`, `up 300 extra` separately: rejection, no movement.
3. With travel clearance and the handset accessible, enter `up 300`: expect `UP BEGIN ... duration=300`, then `UP END ... reason=timeout`. Confirm physical release and GPIO5=0 through `status`.
4. Enter `up 1000`, promptly another `up 300`, then `stop`: the second UP is busy, stop releases early. Use a terminal capable of sending prepared whole lines if typing is too slow.
5. In the current button-only mode, `up 5000` must release with `reason=timeout` after approximately 5 seconds even with UART bytes=0. If UART feedback is re-enabled, missing valid RX should instead cause early `RX loss`. Check UART/parser counters independently if the tap is connected.
6. Verify release after closing the monitor. If separately testing cable loss with independent board power, expect disconnect detection or the original timeout; cable-powered boards release through power loss/pulldown. Do not change energized power/ground wiring.

Exit monitor with **Ctrl+]**. New button control still needs physical validation. No automatic test movement runs after flashing.

## What it reads

The documented UART interface uses **9600 baud, 8N1, non-inverted**. UART port names below are **labels printed on the desk handset PCB**, not the ESP32 UART direction.

- **Desk RX pad (yellow):** 7-byte `AA` frame followed by a 6-byte duplicated-pair status. Height = last duplicated byte of status, e.g. `4D 4D` = 77 cm or `5A 5A` = 90 cm. Parsed on ESP32-C3 **UART1 RX GPIO4**.
- **Desk TX pad (light blue):** leave disconnected in this version. Its `BB` position updates and `D8` button states are not monitored; the reusable parser still supports these formats for offline tests.
- Only **UART1 RX GPIO4** is initialized. No TX GPIO is assigned and no UART transmission API is called. ESP32-C3 has no UART2.

The program logs display-height changes, `CTRL_COMMAND` independently on command changes, raw-motion transitions, and a status heartbeat every 5 seconds. `RX_live=no` means no current valid observation. STATUS separates `display`, inferred `current` (with a `held` flag) and `preset_target_candidate`. PARSER and UART lines separate decode errors, invalid STATE, valid display transitions and transport/logging overload. The raw AA command field is not a complete handset button decoder.

`height_cm` follows the displayed value and may briefly differ from physical height during preset activation. Suspected jumps produce `DIAG BEFORE/ANOMALY/AFTER` lines with exact AA/state bytes and all decoded fields. The default heuristic is a height jump of at least 5 cm with raw-position change at most 32, with four preceding and four following valid messages. STATE must have three duplicated pairs, scan `0x60..0x67`/`0x98` and height 50..150 cm; this is a provisional acceptance policy, not measured travel limits. A PRESET_1 (`0x04`) display transient is retained as a candidate separately from the last plausible physical height. Motion uses raw-position changes independently of command. See PROTOCOL.md sections 3.4–3.6 for limits and counters.

UART reception uses an 8192-byte ring and 64-event queue; USB logs go through a separate 64-record queue. Watch `fifo_overflow`, `ring_full`, `frame`, `rx_peak`, `event_peak`, `log_dropped` and `log_max_us` when investigating errors during movement. New transport errors discard partial input and invalidate physical certainty. The corrected checksum version built successfully for ESP32-C3 with ESP-IDF 6.1 on 2026-10-19: binary `0x2BD10` bytes, 83% app partition free. It has not been flashed by the agent; new diagnostics still require runtime verification.

`REJECT_COUNTS` distinguishes AA header/checksum refusal, STATE validation, desynchronization runs, internal capacity recovery, offline TX validation and transport partial discards. `REJECT_SAMPLE` prints only the first observed context per reason (up to 13 bytes), at most once while incomplete and once when closed. Samples preserve decision length, stream offset and STATE policy flags; later errors update statistics without replacing samples. Unknown scans remain rejected by the existing provisional policy, but are **not established protocol errors**. See PROTOCOL.md section 3.7 for interpreting `decode_errors` and the 12 skipped bytes after an AA refusal.

AA checksum is the full **16-bit sum of byte indices 1-4**, transmitted **big-endian in bytes 5-6**. Byte 5 is the high checksum byte, not an unknown flag. `AA 09 11 E4 02 01 00` and `AA 09 11 E6 02 01 02` carry sums `0x0100` and `0x0102`. Parsed events/DIAG expose `aa_checksum`; raw AA bytes are preserved. `REJECT_AA` shows full received/calculated sums. Temporary `AA_SUM` comparison statistics are removed. See PROTOCOL.md section 3.1.

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
I (...) shapdesk: CTRL_COMMAND 0x00
I (...) shapdesk: HEIGHT display=90 current=90 held=no preset_target_candidate=unknown raw=5309
I (...) shapdesk: STATUS display=90 current=90 held=no preset_target_candidate=unknown ctrl_command=0x00 raw=5309 moving=no direction=0 RX_live=yes
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
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_protocol.c main/desk_tracking.c tests/test_tracking.c -o test_tracking
./test_tracking
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_protocol.c tests/test_rejections.c -o test_rejections
./test_rejections
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_protocol.c tests/test_checksum.c -o test_checksum
./test_checksum
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I main main/desk_button_logic.c tests/test_button.c -o test_button
./test_button
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
