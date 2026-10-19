#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef struct {
    uint8_t buf[13];
    uint8_t len;
    uint32_t good;
    uint32_t invalid;
    uint32_t skipped;
} desk_parser_t;

void desk_parser_reset(desk_parser_t *parser);
// Each feed returns at most one event. Separate parsers per physical UART.
bool desk_feed_rx(desk_parser_t *parser, uint8_t byte, desk_event_t *event);
bool desk_feed_tx(desk_parser_t *parser, uint8_t byte, desk_event_t *event);
