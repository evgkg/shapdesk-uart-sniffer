# Agent instructions — Shapdesk UART integration

This repository is an **unofficial** reverse-engineering and ESP-IDF integration project for a Shapdesk height-adjustable desk. The project owner and another ChatGPT conversation have already captured serial traffic. **Do not assume you can see that conversation.** Read `PROJECT_CONTEXT.md`, `PROTOCOL.md`, `README.md` and the code before changing anything.

## Current scope

- First milestone: **read-only** UART sniffer, decoding desk position/height and handset button activity. The next milestone may be safe integration with Home Assistant.
- The physical board connected during the latest flashing attempt was identified by `esptool` as **ESP32-C3 (COM3)**, not ESP32-S3. Target `esp32c3` for this board. A previous `esp32s3` build succeeded, but it is **not** suitable for flashing onto the C3. Confirm the opened project's configuration before building.
- Windows 11, VS Code + ESP-IDF extension; ESP-IDF 6.1 at `C:\esp\v6.1\esp-idf` (paths may differ later). Do not assume ESP-IDF 6.1 compatibility is verified for the C3 variant until a successful build and flash are observed.

## Engineering rules

1. Prefer small, reviewable changes. State whether a result is **observed**, **inferred**, or **from third-party sources**; never silently promote a hypothesis to protocol fact.
2. Preserve existing `PROTOCOL.md` findings; reconcile new evidence against `.sr` captures if provided. Update documentation and `PROJECT_CONTEXT.md` when conclusions change.
3. Use `idf.py build` for compile verification when ESP-IDF environment is configured. Report actual results and errors. **Do not flash devices or transmit motor commands unless the user explicitly approves.**
4. Keep UART connections **read-only** until electrical levels, isolation, and communication ownership are understood. ESP32-C3 GPIO are **not 5-V tolerant**: use verified level translation on possible 5-V desk signals. Confirm the desk's low-voltage ground can be safely connected to a USB-powered PC before wiring.
5. Do not bypass the stock desk controller's travel limits, collision protection or other safeguards. Movement control must include loss-of-communication and watchdog stop behavior and preserve the stock handset.
6. No passwords, network proxies, tokens, personal data, or computer-specific paths in public commits; check new files before committing.
7. Before ending a development session, briefly record significant verified discoveries, design decisions and open questions in `PROJECT_CONTEXT.md`, so future Codex sessions can resume without copy/pasting chats. Keep the record concise and factual.

## Existing protocol conventions

- UART: **9600 baud, 8N1**, observed on the handset RX and TX lines.
- Handset labels: RX ~yellow wire, TX ~light-blue wire, 5V ~dark-blue wire; see board photos and confirm the ground point rather than trusting wire color.
- Button command family `D8 D8 66 XX XX` observed for `00` released, `01` down, `02` up; `80` appeared near display sleep (interpretation tentative).
- RX captures show a **7-byte `AA ...` packet followed by a 6-byte state group**. The final two bytes of the state group match the displayed height (e.g., `4D 4D` = 77 cm; `5A 5A` = 90 cm).
- TX captures include a `BB` position family; exact field definitions and any full-duplex bridge behavior still require checking.

See `PROJECT_CONTEXT.md` for examples, confidence levels, safety constraints and next experiments.
