#include "desk_diagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static desk_diag_sample_t logged[32];
static desk_diag_kind_t kinds[32];
static unsigned count;

static void collect(desk_diag_kind_t kind, const desk_diag_sample_t *sample, void *ctx) {
    assert(ctx == &count && count < 32);
    kinds[count] = kind;
    logged[count++] = *sample;
}

// Synthetic fixtures: only raw/height pairs come from the user's observation.
// Command, scan, mode and checksum are test inputs, not captured preset bytes.
static desk_event_t parse(desk_parser_t *parser, unsigned raw, unsigned height,
                          uint8_t command, uint8_t scan, uint8_t mode) {
    uint8_t bytes[] = {0xAA, 0x09, (uint8_t)(raw >> 8), (uint8_t)raw,
                       command, 0x00, 0, scan, scan, mode, mode,
                       (uint8_t)height, (uint8_t)height};
    for (unsigned i = 1; i <= 5; ++i) bytes[6] += bytes[i];
    desk_event_t event = {0};
    // Every byte is a separate feed: state must survive arbitrary read boundaries.
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        assert(desk_feed_rx(parser, bytes[i], &event) == (i == sizeof(bytes) - 1));
    }
    assert(event.position_raw == raw && event.height_cm == height);
    assert(event.button == command && event.scan_code == scan && event.mode == mode);
    assert(memcmp(event.aa, bytes, 7) == 0);
    assert(memcmp(event.state, bytes + 7, 6) == 0);
    return event;
}

int main(void) {
    desk_parser_t parser = {0};
    desk_diagnostics_t diag = {0};
    desk_event_t event;
    int64_t now = 0;
    // Fill and wrap the ring. No first-message or steady-state anomaly.
    for (unsigned i = 0; i < 7; ++i) {
        event = parse(&parser, 6026, 109, 0, 0x62, 0);
        assert(!desk_diagnostics_feed(&diag, &event, now += 28000, collect, &count));
    }
    assert(count == 0);
    event = parse(&parser, 6032, 68, 0x10, 0x63, 3);
    assert(desk_diagnostics_feed(&diag, &event, now += 28000, collect, &count));
    assert(count == 5 && kinds[4] == DESK_DIAG_ANOMALY);
    for (unsigned i = 0; i < 4; ++i) {
        assert(kinds[i] == DESK_DIAG_BEFORE && logged[i].sequence == i + 4);
        assert(logged[i].event.height_cm == 109);
    }
    assert(logged[4].event.height_cm == 68 && logged[4].event.button == 0x10);
    assert(memcmp(logged[4].event.aa, event.aa, 7) == 0);
    assert(memcmp(logged[4].event.state, event.state, 6) == 0);
    event = parse(&parser, 6018, 108, 0, 0x64, 0);
    assert(desk_diagnostics_feed(&diag, &event, now += 28000, collect, &count));
    assert(count == 6 && kinds[5] == DESK_DIAG_ANOMALY && diag.anomalies == 2);
    // Return jump extends the window, without duplicating prior context.
    for (unsigned i = 0; i < 5; ++i) {
        event = parse(&parser, 6008 - i * 10, 108 - i, 0, 0x65, 0);
        assert(!desk_diagnostics_feed(&diag, &event, now += 28000, collect, &count));
    }
    assert(count == 10 && diag.following == 0);
    for (unsigned i = 6; i < count; ++i) assert(kinds[i] == DESK_DIAG_AFTER);
    assert(logged[9].sequence == 13);
    // Large raw change, stale gap, and TX events must not trigger the heuristic.
    event = parse(&parser, 4426, 67, 0, 0x62, 0);
    assert(!desk_diagnostics_feed(&diag, &event, now += 28000, collect, &count));
    event = parse(&parser, 4426, 109, 0, 0x62, 0);
    assert(!desk_diagnostics_feed(&diag, &event, now += 1000001, collect, &count));
    event.type = DESK_EVENT_TX_BUTTON;
    assert(!desk_diagnostics_feed(&diag, &event, now, collect, &count));
    assert(diag.anomalies == 2 && count == 10);
    // Exact heuristic boundaries and cancellation of a pending window on a gap.
    event = parse(&parser, 4458, 104, 0, 0x62, 0);
    assert(desk_diagnostics_feed(&diag, &event, now += 28000, NULL, NULL));
    assert(diag.following == 4);
    assert(!desk_diagnostics_feed(&diag, &event, now += 1000001, collect, &count));
    assert(diag.following == 0 && count == 10);
    puts("OK: raw bytes, 109->68->108, ring wrap, overlapping windows, normal motion, stale gap");
    return 0;
}
