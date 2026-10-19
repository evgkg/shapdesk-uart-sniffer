#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Provisional desk-specific plausibility limits, not proven travel limits.
#define DESK_DISPLAY_MIN_CM 50
#define DESK_DISPLAY_MAX_CM 150

// Parsing is byte-oriented; no ESP-IDF dependencies, so it can be tested on a PC.
typedef enum {
    DESK_EVENT_NONE = 0,
    DESK_EVENT_RX_STATE,  // AA(7) followed by a six-byte state group
    DESK_EVENT_TX_POS,    // BB(6)
    DESK_EVENT_TX_BUTTON, // D8 D8 66 code code
} desk_event_type_t;

typedef struct {
    desk_event_type_t type;
    uint16_t position_raw;
    uint8_t height_cm;     // displayed cm; may transiently differ from physical height
    uint8_t button;        // DESK_EVENT_TX_BUTTON, or AA byte[4] for RX_STATE
    uint8_t scan_code;     // repeated first two bytes of RX state group
    uint8_t mode;          // repeated middle two bytes of RX state group
    uint8_t aa[7];         // exact validated RX bytes, not reconstructed
    uint8_t state[6];
} desk_event_t;

typedef enum {
    DESK_REJECT_AA_HEADER,
    DESK_REJECT_AA_CHECKSUM,
    DESK_REJECT_STATE,
    DESK_REJECT_DESYNC,
    DESK_REJECT_CAPACITY,
    DESK_REJECT_TX_VALIDATION,
    DESK_REJECT_TRANSPORT_PARTIAL,
    DESK_REJECT_REASON_COUNT
} desk_reject_reason_t;

#define DESK_STATE_BAD_PAIRS 1u
#define DESK_STATE_UNKNOWN_SCAN_POLICY 2u
#define DESK_STATE_HEIGHT_POLICY 4u

typedef struct {
    uint8_t bytes[13]; // observed stream context, not necessarily a whole packet
    uint8_t len;
    uint8_t decision_len; // bytes available when the decision was made
    uint8_t state_flags;
    bool closed; // full 13-byte context, or truncated at a transport discontinuity
    uint64_t offset; // 1-based stream byte offset, independent of UART read chunks
} desk_reject_sample_t;

typedef struct {
    uint8_t buf[13];
    uint8_t len;
    uint32_t good;
    uint32_t invalid;
    uint32_t skipped;
    uint32_t decode_errors; // invalid AA/BB/D8 or parser capacity recovery
    uint32_t invalid_state; // valid AA followed by an implausible STATE
    uint32_t state_pair_errors;
    uint32_t state_scan_errors;
    uint32_t state_height_errors;
    uint8_t last_rejected_state[6];
    uint32_t reject_counts[DESK_REJECT_REASON_COUNT];
    // One first example per reason per parser reset; bounded memory and logging.
    desk_reject_sample_t reject_samples[DESK_REJECT_REASON_COUNT];
    uint64_t bytes_fed;
    bool desync_active;
} desk_parser_t;

void desk_parser_reset(desk_parser_t *parser);
// Discard partial bytes after transport loss, preserving cumulative counters.
void desk_parser_discard_partial(desk_parser_t *parser);
const char *desk_reject_reason_name(desk_reject_reason_t reason);
// Each feed returns at most one event. Separate parsers per physical UART.
bool desk_feed_rx(desk_parser_t *parser, uint8_t byte, desk_event_t *event);
bool desk_feed_tx(desk_parser_t *parser, uint8_t byte, desk_event_t *event);
