# Shapdesk UART — project handoff and research notes

Last consolidated from the ChatGPT conversation on **2026-10-19**. This file is a **summary of observations**, not a full raw-log archive. Refer to `PROTOCOL.md`, original PulseView `.sr` recordings, and project source for more detail. Some earlier interpretations were corrected after new captures.

## Goal

Create an **unofficial, open-source** ESP-based Wi-Fi/Home Assistant integration for a height-adjustable Shapdesk, keeping physical buttons, stock motor controller and safety behavior. Prefer digital UART integration over soldering switches in parallel. Immediate milestone: passively monitor traffic and reliably parse height and handset actions.

## Physical setup and tools

- Desk handset: 3-digit display, seven buttons (up/down, memory presets 1–4, M), separate MCU, cable to desk control unit.
- Observed markings near the handset cable: `SLEEP`, `GND`, `RX`, `TX`, `5V` (and other pads). User identified **RX yellow**, **TX light-blue**, **5V dark-blue**. Verify electrical GND rather than inferring from color.
- Multimeter readings relative to marked/verified ground in powered state: `5V` ≈ **4.74 V**; `RX` ≈ **3.28 V**; `TX` fluctuates ≈ **3.9–4.28 V**. A multimeter averages fast pulses: these are **not reliable peak-voltage measurements**.
- Logic analyzer: inexpensive Saleae Logic-compatible **USB 24 MHz / 8-channel** clone. Its connector marks ground `CND` (likely a vendor typo); PulseView identifies it as Saleae Logic. The product listing advertises **5.25 V max input**, but actual board rev/input protection was not independently verified. This is **not** a rating for ESP GPIO.
- Capture software: PulseView, decoder UART **9600 baud, 8 data bits, no parity, 1 stop bit, non-inverted, LSB first**. Typical recordings at 1 MHz sampling.
- Originally planned development board: ESP32-S3-N16R8. Actual board connected at COM3 detected by `esptool` as **ESP32-C3**. A separate C3 project variant was generated to use one desk UART receiver. Confirm actual board and current project variant.
- ESP-IDF 6.1 installed on Windows, integrated into VS Code. The original S3 variant built successfully after disabling CMake Tools interference; C3 build/flash has not been reported successful in the conversation at handoff.

## Observed UART data

### Button events on line labeled TX

The following **five-byte** frames were observed in the logic captures:

```text
D8 D8 66 00 00    released / no button held
D8 D8 66 01 01    down held
D8 D8 66 02 02    up held
D8 D8 66 80 80    observed near a sleep transition, exact meaning unverified
```

`D8` frames during hold/release repeated on the order of ~256 ms (verify against captures). Similar third-party TiMOTION-compatible projects suggest `0x04/0x08/0x10/0x20` for presets 1–4 and `0x40` for M, **not yet verified on this handset**.

### `BB` family on line labeled TX

Example observed:

```text
BB 01 12 C4 00 D7
```

- Position-like big-endian field `12 C4` (=4804 decimal), increasing on up, decreasing on down.
- `00 D7` matches `0x01 + 0x12 + 0xC4 = 0x00D7` (16-bit sum). Many packets were checked earlier; revalidate as new variants appear.
- Do **not** treat any earlier rough conversion of this counter to centimetres as proven. Initially suggested `P/64 + 2`, later disproved; subsequently `P/40 - 43` appeared approximate and should **not** substitute for the explicit height byte in the other packet family.

### RX: 7-byte `AA` followed by a separate 6-byte state group

Examples:

```text
AA 09 11 4A 00 00 64    62 62 00 00 43 43
AA 09 12 B6 00 00 D1    60 60 03 03 4D 4D
AA 09 14 BD 00 00 DA    62 62 00 00 5A 5A
```

- For the seven-byte packet, the last byte matches the sum of bytes 1 through 5 after `AA` modulo 256; e.g. `09 + 12 + B6 + 00 + 00 = D1` modulo 256.
- Two-byte field in `AA` (e.g. `12 B6`) changes with position. The button/activity field (e.g. `00` vs `02`) changes during up; more mapping needed.
- A **separate six-byte group** follows, whose first duplicated byte can cycle among `60,61,62,63,64,65,66,67,98`; purpose unknown. The duplicated middle byte (`00` / `03`) changes with state; do not call it simply 'motor moving' without confirmation.
- Last duplicated state byte directly matches display centimetres in observed captures: `43 43` (67), `4D 4D` (77), `5A 5A` (90). A capture raising from ~67 cm ended with exactly **77** on the physical display; another at **90** yielded stable `5A 5A`. This is the strongest confirmed decoding result.
- In one RX idle capture `AA 09 11 4A 00 00 64` and state groups repeated approximately every 28 ms.

### Display sleep

- User reported: UART traffic continues after release while display remains lit; physical screen eventually goes dark. From sleep, holding ↑ wakes it and motion begins with a small delay; no second press required.
- In a ~40-s capture, `D8 D8 66 80 80` appeared around 28.136 s and ordinary `D8` stopped by around 28.645 s; `BB`-type frames continued to the end of capture. The user believed the screen went off around that time, **not timed precisely**.
- Sleep semantics, directionality and the exact controller/handset responsibilities remain open.

## Relevant outside implementations (similarity ≠ exact compatibility)

- https://github.com/smartalock/TiMotionAnalyzer — third-party UART decoder for TiMOTION-like desks.
- https://github.com/Stefichen5/AutonomousControl — ESP implementation for an Autonomous desk with very similar command framing.
- https://github.com/tjhorner/wifi-desk-controller — discussion of multi-transmitter UART collision problems.

Use their code and documentation only after checking their licenses and comparing packets against *this desk's* captures. Avoid copying closed-source firmware or protected schematics.

## Safety and engineering constraints

1. **Never connect potential 5-V desk signals directly to ESP32-C3/S3 GPIO.** Choose an appropriate verified 5-V-tolerant-to-3.3-V buffer or proper voltage divider, and ensure all levels and logic thresholds are safe.
2. A cheap USB logic analyzer is not necessarily galvanically isolated. Before connecting desk electronics to a USB-powered PC/ESP, establish that the desk controller's low-voltage return can safely share ground with the computer. Multimeter DC averages alone do not prove this.
3. Keep the sniffer **receive-only**: ESP UART transmit pins should not be connected to the desk. No movement/replay experiments until bus arbitration/ownership and fail-safe stop behavior are designed and explicitly approved.
4. Protect the stock controller's safety limits and collision detection. Never leave a latched 'up' or 'down' command in the event of network/firmware faults.

## Current task / status

- User installed VS Code ESP-IDF extension and ESP-IDF 6.1.
- User confirmed `esp32s3` target was configured and the S3 firmware build reported success, but flashing failed because the connected COM3 chip was **ESP32-C3**.
- C3-specific firmware archive (`shapdesk-uart-sniffer-esp32c3`) was provided for a one-line receive-only sniffer; build/flash and hardware verification remain pending.
- User has set up the Codex extension in VS Code. Local network/proxy settings are outside the repository and should not be committed.
- Proposed next task: inspect checked-out source to confirm whether it is the C3 project variant; configure `idf.py set-target esp32c3`, run `idf.py build`, confirm COM3 and flash method UART, and **ask before flashing**. Then verify read-only parsing while electrically isolated from the desk.

## Update protocol

After each substantial implementation/experiment, append a concise **dated** note here with: verified result, evidence (capture or command output), changed files, remaining unknowns. Keep `PROTOCOL.md` as the technical packet reference. Avoid pasting the entire chat or long build logs.

## 2026-10-19 ? ESP32-C3 single-line adaptation

- Observed in this checkout: local configuration already selected `esp32c3`, but `main/main.c` still initialized S3-style UART1 and UART2. Removed the second UART, panel state handling and TX-line logs. Only UART1 RX on GPIO4 at 9600 8N1 is initialized; no desk TX pin is assigned and no commands are sent.
- Preserved `desk_protocol.c/.h` unchanged, including byte-stream AA checksum validation, the following six-byte duplicated-pair state group, height decoding and resynchronization. Offline BB/D8 parser support remains available; this firmware does not monitor the handset TX line.
- Added the C3 default target in `CMakeLists.txt` and native USB Serial/JTAG console defaults in `sdkconfig.defaults`; updated local ignored `sdkconfig` and README wiring/build/log instructions. Native USB reserves GPIO18/19; actual board connector wiring and live console output remain unverified.
- Observed: installed EIM PowerShell profile provides ESP-IDF v6.1. Standalone export could not resolve this EIM tool layout. With the installed profile, `idf.py build` reached CMake launch but failed with sandbox `PermissionError: [WinError 5]` creating a subprocess pipe. The request to run outside the sandbox was rejected by the user. C3 compile/link, live USB logs and desk decoding are therefore still unverified; no flash or UART transmission was performed.
- Next: run `idf.py build` in the installed ESP-IDF 6.1 terminal, resolve any actual compiler errors, then request explicit approval before flashing. Confirm USB wiring and electrical interface before hardware validation.

## 2026-10-19 ? User-provided C3 build and device log

- Observed in the user's subsequent VS Code ESP-IDF 6.1 output: target `esp32c3`, successful application link and image generation, `shapdesk_uart_sniffer.bin` size `0x29d20` bytes, 84% app partition free. This supersedes the earlier unverified compilation status.
- Device detection on COM3 reports ESP32-C3 revision v0.4, embedded 4 MB flash and native USB Serial/JTAG. No device identifiers are retained here.
- The extension reports `Flash Done`, but detailed flash write/verification output is absent from the supplied text. No runtime monitor output was supplied; successful boot, USB application logs and live desk decoding remain unverified. The agent did not flash or send UART commands.
- Kconfig and component dependency warnings shown originate in ESP-IDF components and did not prevent this build. Next verification: inspect ESP-IDF Monitor output for the passive sniffer startup and five-second STATUS logs.
