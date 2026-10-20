# Shapdesk UART — project handoff and research notes

Last consolidated from the ChatGPT conversation on **2026-10-19**. This file is a **summary of observations**, not a full raw-log archive. Refer to `PROTOCOL.md`, original PulseView `.sr` recordings, and project source for more detail. Some earlier interpretations were corrected after new captures.

## Goal

Create an **unofficial, open-source** ESP-based Wi-Fi/Home Assistant integration for a height-adjustable Shapdesk, keeping physical buttons, stock motor controller and safety behavior. Prefer digital UART integration over soldering switches in parallel. Immediate milestone: passively monitor traffic and reliably parse height and handset actions.

Current scope update (2026-10-20): the owner explicitly authorized bounded GPIO5 emulation of the physical UP contact over USB commands. UART remains strictly receive-only; earlier read-only-only milestones below are historical. No Wi-Fi/MQTT/HA or device flashing is authorized in this implementation task.

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

- Current AA checksum: full uint16 sum of offsets 1..4, transmitted big-endian in offsets 5..6. Examples 0100/0102 identify offset 5 as the high checksum byte. Earlier eight-bit/unknown-byte interpretations in the dated history below are superseded by the latest 16-bit update.
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

## 2026-10-20 — Bounded physical UP emulation over USB

- Owner-reported hardware: GPIO5 through 4.7k to 2N3904 base, base to GND through 100k; collector UP CH2, emitter UP CH1. HIGH presses, LOW releases. Owner physically verified raising/waking with this circuit; the agent has not tested hardware, isolation or timing.
- Added desk_button_logic.c/.h (strict bounded command framing and portable press/deadline policy), desk_button.c/.h (GPIO5 initialization, independent GPTimer ISR, USB reader, bounded event logging), tests/test_button.c. Integrated with main.c, component dependencies and sdkconfig.defaults. Parser, height/preset/motion logic and UART1 RX GPIO4 are unchanged; no desk UART TX exists.
- Commands: up 50..1000 ms, stop, status. Initial latch LOW precedes output enable; no automatic press at boot/reconnect. Only one active press, busy UP cannot extend deadline. Hardware timer checks absolute expiry every 1 ms, sets LOW before queuing logs, using IRAM/cache-safe code. Runtime tick quantization and ISR latency still need measurement. This is independent of command/log task progress, not a physical guarantee under total interrupt/CPU/electrical failure.
- Safety guards: valid AA+STATE silence >=500 ms (500 ms grace from press start) or UART transport error releases; USB host SOF loss releases when command task detects it. A long wake press can end early if RX stays silent. Closing a terminal alone may retain USB connection; timeout remains effective. stop releases only the emulated UP contact, not physical buttons or stock automatic presets.
- USB command task yields under flood and never prints synchronously. Existing logger drains button-event queue, reports begin/end reasons and loss count. Main owns desk snapshots; status requests use task notification. Shutdown handler releases on normal software reset; external base pulldown is relied on before application startup/during reset.
- Verified: ESP-IDF v6.1 esp32c3 idf.py build succeeded (exit 0), image 0x2FF00 (196352) bytes, app partition 81% free. Both GPTIMER_ISR_CACHE_SAFE and GPIO_CTRL_FUNC_IN_IRAM enabled in defaults and local ignored config; build fails if absent. ELF symbol inspection places guard_alarm, desk_button_policy_tick, gpio_set_level, esp_timer_get_time and ISR queue send in IRAM, policy in DRAM.
- Tests added for input framing/overflow/control characters, command bounds, busy non-extension, timeout and RX watchdog. Portable C11 strict syntax checks passed; test execution remains unverified because installed ESP clang only supports RISC-V/Xtensa and no native host compiler has been established. No flash, COM-port access, motor movement, wiring change or UART transmission performed by agent. Next: operator manual verification per README (boot LOW, short pulse, stop/busy, errors and continued UART reception).

## 2026-10-19 — AA sixth-byte checksum hypothesis

- User supplied rejected `AA 09 11 E6 02 01 02`: observed sum with offset 5 is 03; excluding it is 02, matching the wire. Older documented examples all have offset 5 = 00 and cannot distinguish the formulas. User reports 63 checksum refusals, skipped 762 (=63*12+6), no other packet refusal causes or hardware UART errors. Only one new raw AA is available; the other refusals and extra six skipped bytes are not explained conclusively.
- Decision: diagnostic-only, retain historical acceptance sum over offsets 1..5 until more nonzero-offset-5 samples discriminate coverage. Do not accept either formula opportunistically. Offset 5 remains unknown, not a proven motion/state flag. PROTOCOL.md 3.1 now lists every AA byte and explicitly qualifies checksum coverage.
- Added both checksum helpers, per-value (all 256) seen/match-with/match-without histogram and total counts before checksum/STATE validation, counted once per AA09 stream offset. External static stats avoid copying the 3 KiB histogram in log records. Periodic AA_SUM and at most 8 rotating AA_SUM_BYTE5 rows; bounded REJECT_AA examples show both sums and matches. No height/preset/motion logic changed.
- Added tests/test_checksum.c for three old examples, the exact new seven-byte observation (still rejected in diagnostic mode), synthetic following STATE, acceptance of historical formula, both/neither matches, modulo overflow, all 256 byte values and deduplication while awaiting STATE. Portable strict C11 syntax checks pass for all five test sources; tests not executed on this Windows host.
- Observed successful `idf.py build` (exit 0, "Project build complete") on ESP-IDF v6.1, target esp32c3, after approved outside-sandbox execution. Generated build/shapdesk_uart_sniffer.bin size 0x2C050 (180304) bytes; app partition 83% free. IDF Kconfig/dependency warnings did not prevent compilation/linking. This supersedes prior blocked build status; new checksum diagnostics still need runtime verification. No flashing or UART transmission.

## 2026-10-19 — AA rejection classification and bounded raw evidence

- User reports 15-second deltas: bytes 6840, good 490, decode_errors 36, skipped 432, zero UART overflow/frame/parity/break counters. Source analysis: decode_errors increments only for AA header/checksum refusal, defensive parser capacity, or offline TX validation. STATE refusal increments invalid_state separately; the running firmware feeds only RX. An AA refusal drops its start byte at length 7; remaining non-AA bytes are skipped, explaining 12 skipped bytes per rejected 13-byte sequence without requiring UART loss. Header versus checksum remains unproven pending captured examples.
- Added reason counters and bounded raw stream-context samples in desk_protocol.c/.h. First example per seven reasons per reset; up to 13 observed bytes, decision length, byte offset and STATE reason flags. Prefix collection does not delay parsing or change acceptance. Partial contexts close on transport gaps. main.c prints counters and examples only with periodic status through the existing log queue (max prefix/final per reason); logger stack increased to 6144 bytes for larger snapshots. Unknown scan is labelled policy-unknown, not proven corruption; prior acceptance rules preserved.
- Added tests/test_rejections.c: synthetic 490 good +36 checksum-rejected sequences +2 partial AA bytes reproduce exact 6840/490/36/432; header/checksum precedence, STATE flags, unknown-scan policy, bounded samples, recovery, capacity and TX branches covered. Existing height/preset/motion code unchanged. PROTOCOL.md 3.7 and README document evidence and limits.
- Verified: ESP clang C11 syntax checks with -Wall -Wextra -Werror -pedantic pass for portable modules and all four test sources. Tests were not executed (installed compiler targets RISC-V, no host compiler established). ESP-IDF v6.1 environment and esp32c3/USB console configuration confirmed; idf.py build again fails at subprocess pipe creation with sandbox WinError 5. Request for outside-sandbox build was rejected by user. New full build/runtime remain unverified; no flash or UART transmission performed.

## 2026-10-19 — STATE validation, physical/display tracking and UART telemetry

- User reports diagnostic confirmation: PRESET_1 command `0x04` temporarily displays stored 68 cm while raw corresponds to 112 cm, then display returns to physical height and automatic motion finishes at 67. This establishes an observed preset/display association per the user, not an exact motor target or new raw-to-cm formula. Full new trace was not attached.
- Verified by source inspection: the old three-pair check accepts `FF FF 60 60 03 03` as height 3. Parser now requires matching pairs, provisional scans `60..67`/`98`, and provisional display range 50..150 cm. Invalid events do not reach tracking. Mode remains unrestricted because its legal domain is unknown. Parser counters split decode/STATE rejection, with pair/scan/height reasons and last rejected bytes.
- Added portable `desk_tracking.c/.h`: display height separate from last plausible current height and preset candidate. Large display/raw mismatches hold current; return within 2 cm resumes it. Candidate for observed command 0x04 remains separate; startup during a preset leaves current unknown. Motion uses accumulated raw delta >=8, independent of command, with 500 ms inactivity timeout. >1 s receive gap or UART error invalidates certainty. All thresholds are heuristics, not physical calibration.
- Static investigation: synchronous diagnostic USB logging previously ran in the same loop as UART reads, with a 2048-byte ring and no driver error queue. This could delay reception, but has not been demonstrated as the cause of movement-related invalid/skipped growth. New main.c uses an 8192-byte RX ring, 64 UART events, separate nonblocking 64-record log queue/task, error/peak/read-gap/log-duration/drop telemetry, and recovery flush counts. Driver errors flush input and reset partial parsing/history/tracking. Event queue saturation can hide errors; counters cannot prove losslessness.
- Added `tests/test_tracking.c` covering 112->68->112->111, command-zero motion, repeated preset display, held/current/candidate separation, invalid heights 0/3, exact bad STATE, scans, boundary policy, error classification/recovery and transport gaps. Fixtures beyond reported values are synthetic. Updated PROTOCOL.md and README with interpretation, counters and limitations.
- Per the user's final instruction "do not build", no firmware build, test compilation/execution or flash was attempted in this session. Source/diff review only; new code requires compile and runtime verification when authorized. Passive UART1 RX GPIO4, USB console and electrical wiring remain unchanged. No motor control or UART transmission added.

## 2026-10-19 — Preset display transient diagnostics

- User-reported observation: `(height_cm, position_raw)` changes `(109,6026) -> (68,6032) -> (108,6018)`, then decreases toward `(67,4426)` on preset recall; stock display briefly shows 68. Raw capture and command/scan/mode values for this episode were not supplied. Treat height as displayed cm, not always physical height. Meaning of 68 as a target remains a hypothesis.
- `desk_protocol.c/.h` now preserve exact validated AA/state bytes in each RX event. New portable `desk_diagnostics.c/.h` detect heuristic jumps (height delta >=5, raw delta <=32), retain four previous samples, emit four following samples and extend overlapping windows. Comparisons reset after >1 s without a valid sample. No conversion, filtering or motion logic was added.
- `main.c` logs full diagnostic samples, command changes independently of height, and STATUS command/invalid/skipped/anomaly counters. UART1 RX GPIO4 and USB console are unchanged; no device flashing, UART transmission or wiring changes performed.
- Added synthetic tests for 109->68->108, exact byte retention, ring wrap/order, overlapping windows, ordinary motion and stale gaps. Command/scan/mode fixture values are invented test inputs, not protocol findings. Updated component sources and README test instructions; PROTOCOL.md section 3.4 separates observation from hypothesis.
- Verification: ESP clang compiled both portable test programs with C11 and strict warnings, but produced RISC-V binaries that cannot run on this Windows host; tests are not claimed passed. `idf.py build` with the installed ESP-IDF 6.1 profile was blocked before CMake startup by sandbox WinError 5; the outside-sandbox request was rejected. The earlier successful user build predates this diagnostic change. New firmware build and runtime verification remain pending.

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

## 2026-10-19 - Confirmed AA checksum correction

- User-reported evidence: of 1227 AA, 1165 match the old offsets-1..5 sum and all 1227 match offsets 1..4; all 62 old rejections have offset 5 = 01; UART-driver errors zero. This supersedes the diagnostic-only decision above. Confirmed checksum for the observed protocol is `(aa[1]+aa[2]+aa[3]+aa[4]) & 0xFF`.
- Parser now uses only corrected desk_aa_checksum. Offset 5 is exposed as aa_byte5 with unknown semantics and retained in raw AA; DIAG logs it. Removed temporary two-formula helpers, external histogram, AA_SUM/AA_SUM_BYTE5 snapshots and logs. Bounded refusal logs retain one calculated checksum. Height/preset/motion modules are unchanged.
- Replaced tests/test_checksum.c with acceptance tests for byte5=00/01 (exact reported AA plus synthetic STATE), all 256 unknown-field values, modulo wrap, refusal of the old sum, recovery and unchanged STATE validation. Strict C11 syntax checks pass for portable code and five test sources; tests were not executed on a host CPU.
- Observed: idf.py build succeeded (exit 0, Project build complete), ESP-IDF v6.1, esp32c3, after approved outside-sandbox run. Binary build/shapdesk_uart_sniffer.bin is 0x2BD00 (179456) bytes; app partition 83% free. No flashing or UART transmission performed; new binary runtime remains to be checked.

## 2026-10-19 - Full 16-bit AA checksum

- New user-provided AA examples: AA 09 11 E4 02 01 00 => 0100; AA 09 11 E6 02 01 02 => 0102; AA 09 16 CC 00 00 EB => 00EB. These identify offsets 5..6 as big-endian uint16 checksum, correcting the earlier unknown-byte and low-byte-only interpretations. The 1227-packet diagnostic established the sum operands but did not independently validate the high byte.
- desk_protocol.c/.h now compare the full uint16 sum of offsets 1..4 with offsets 5..6. Removed aa_byte5; accepted events retain aa_checksum plus exact raw AA. main.c DIAG and bounded REJECT_AA logs use four-digit checksums. Temporary AA_SUM statistics were already removed and remain absent. Rejection counters, STATE validation and byte-wise resynchronization are unchanged; height/preset/motion modules and GPIO are untouched.
- Updated tests/test_checksum.c with 00EB/0100/0102, high/low byte corruption, recovery, old correct frames and 0306 (no truncation). Synthetic STATE accompanies the new observed seven-byte AA samples. Strict portable C11 syntax checks pass; tests not executed because no native host C compiler was found. Test data generators in test_diagnostics.c and test_tracking.c now encode full big-endian checksum. No UART transmission or flashing.

- Verification for the 16-bit change: idf.py build completed successfully (exit 0) on ESP-IDF v6.1, esp32c3; image 0x2BD10 (179472) bytes, 83% app partition free. Strict C11 syntax checks pass for portable sources and all five test sources. Runtime test execution remains unverified; no native compiler was found in the inspected MSYS2/LLVM/Visual Studio locations. No device was flashed.

## 2026-10-20 - UP duration extended to 5 seconds

- User explicitly requested up 5000. Raised DESK_UP_MAX_MS from 1000 to 5000; command help and README reflect 50..5000 ms. Existing stop, busy non-extension, GPTimer expiry, USB/UART fault and 500 ms RX silence guards unchanged. No GPIO/protocol/tracking changes.
- Added test cases accepting 1001/5000, rejecting 5001, exercising the five-second deadline with continuing valid RX and early RX-loss release. Strict portable C11 syntax check passes; unit test execution remains unverified on this host.
- idf.py build succeeded for ESP32-C3 with ESP-IDF v6.1 (exit 0); binary 0x2FF00 bytes, app partition 81% free. No flash or movement performed. New firmware must be flashed by the operator before up 5000 is available.

## 2026-10-20 - Temporary button-only test mode

- Observed user log: up 5000 begins at 78454204 us and releases at 78954543 us (about 500 ms), reason RX loss; UART bytes/good both zero. User explicitly requested removing UART feedback dependence while testing only the physical button.
- Added DESK_UP_REQUIRE_UART_FEEDBACK, default 0, in desk_button_logic.h. Disabled RX-silence expiry and UART-error-triggered button release in this mode; set to 1 and rebuild to restore both interlocks. Sniffer/error statistics continue independently. Absolute 5-second deadline, stop, USB loss, boot LOW and busy non-extension remain active. README and boot log identify the mode; no GPIO or protocol changes.
- Expanded policy tests for five seconds with no RX; strict syntax checks pass with feedback disabled and enabled. Tests not executed on host. ESP-IDF v6.1 esp32c3 build succeeded (exit 0), image 0x2FF50 bytes, 81% app partition free. No flashing or movement performed; operator needs to flash this binary to remove the observed RX cutoff.
