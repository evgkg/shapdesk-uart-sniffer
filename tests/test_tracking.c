#include "desk_tracking.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Synthetic wire fixtures: raw values/scan/mode are not a captured preset trace.
static bool feed(desk_parser_t *p, desk_tracking_t *s, unsigned raw, unsigned height,
                 uint8_t command, uint8_t scan, int64_t now) {
    uint8_t bytes[] = {0xAA, 0x09, (uint8_t)(raw >> 8), (uint8_t)raw, command, 0, 0,
                      scan, scan, 3, 3, (uint8_t)height, (uint8_t)height};
    uint16_t sum = (uint16_t)bytes[1] + bytes[2] + bytes[3] + bytes[4];
    bytes[5] = (uint8_t)(sum >> 8);
    bytes[6] = (uint8_t)sum;
    desk_event_t event = {0};
    bool got = false;
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        if (desk_feed_rx(p, bytes[i], &event)) {
            assert(!got);
            got = true;
            desk_tracking_feed(s, &event, now);
        }
    }
    return got;
}

int main(void) {
    desk_parser_t p = {0};
    desk_tracking_t s = {0};
    assert(feed(&p, &s, 6200, 112, 0, 0x60, 28000));
    assert(s.display_height == 112 && s.current_height == 112 && s.current_known);
    assert(feed(&p, &s, 6206, 68, 4, 0x61, 56000));
    assert(s.display_height == 68 && s.current_height == 112 && s.current_held);
    assert(s.candidate_known && s.preset_target_candidate == 68 && !s.moving);
    // Repeated candidate and release of command must not promote 68 to current.
    assert(feed(&p, &s, 6206, 68, 0, 0x62, 84000));
    assert(s.current_held && s.current_height == 112);
    assert(feed(&p, &s, 6204, 112, 0, 0x63, 112000));
    assert(!s.current_held && s.current_height == 112);
    assert(feed(&p, &s, 6164, 111, 0, 0x64, 140000));
    assert(s.moving && s.motion_direction == -1 && s.ctrl_command == 0);
    assert(s.current_height == 111 && s.preset_target_candidate == 68);
    assert(s.display_changes == 3 && s.display_transients == 1);

    desk_tracking_t before = s;
    assert(!feed(&p, &s, 6164, 0, 0, 0x60, 168000));
    assert(!feed(&p, &s, 6164, 3, 0, 0x60, 196000));
    assert(!feed(&p, &s, 6164, 111, 0, 0xFF, 224000));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(p.invalid_state == 3 && p.decode_errors == 0 && p.good == 5);
    // Exact observed bad STATE: FF FF 60 60 03 03, after a checksum-valid AA.
    const uint8_t bad[] = {0xAA, 9, 0x18, 0x14, 0, 0, 0x35,
                           0xFF, 0xFF, 0x60, 0x60, 3, 3};
    desk_event_t untouched = {.height_cm = 111}, event = untouched;
    for (unsigned i = 0; i < sizeof(bad); ++i) assert(!desk_feed_rx(&p, bad[i], &event));
    assert(memcmp(&event, &untouched, sizeof(event)) == 0 && p.invalid_state == 4);
    assert(p.state_height_errors == 3 && p.state_scan_errors == 2 && p.state_pair_errors == 0);
    assert(memcmp(p.last_rejected_state, bad + 7, 6) == 0);
    // Recover immediately, including the special observed scan code.
    assert(feed(&p, &s, 6150, 111, 0, 0x98, 252000));
    assert(p.good == 6 && p.invalid == p.invalid_state + p.decode_errors);
    uint32_t invalid_state = p.invalid_state;
    const uint8_t bad_aa[] = {0xAA, 9, 0x18, 0x14, 0, 0, 0};
    for (unsigned i = 0; i < sizeof(bad_aa); ++i) assert(!desk_feed_rx(&p, bad_aa[i], &event));
    assert(p.decode_errors == 1 && p.invalid_state == invalid_state);
    desk_tracking_tick(&s, 752001);
    assert(!s.moving && s.motion_direction == 0);
    desk_tracking_tick(&s, 1252001);
    assert(!s.known && !s.current_known && !s.candidate_known);
    // Starting the observation while a preset value is displayed: current unknown.
    assert(feed(&p, &s, 6200, 68, 4, 0x60, 1300000));
    assert(!s.current_known && s.candidate_known);
    assert(feed(&p, &s, 6200, 68, 0, 0x60, 1328000));
    assert(!s.current_known);
    assert(feed(&p, &s, 6200, 112, 0, 0x60, 1356000));
    assert(s.current_known && s.current_height == 112);
    // Partial transport data must not survive a known UART loss.
    assert(!desk_feed_rx(&p, 0xAA, &event));
    uint32_t skipped = p.skipped;
    desk_parser_discard_partial(&p);
    desk_tracking_discontinuity(&s);
    assert(p.len == 0 && p.skipped == skipped + 1 && p.decode_errors == 1);
    assert(!s.current_known && !s.moving);
    // All provisional scans, both plausibility boundaries, and both rejects.
    for (uint8_t scan = 0x60; scan <= 0x67; ++scan)
        assert(feed(&p, &s, 6200, 112, 0, scan, 1400000));
    assert(feed(&p, &s, 6200, DESK_DISPLAY_MIN_CM, 0, 0x98, 1428000));
    assert(feed(&p, &s, 6200, DESK_DISPLAY_MAX_CM, 0, 0x98, 1456000));
    assert(!feed(&p, &s, 6200, DESK_DISPLAY_MIN_CM - 1, 0, 0x60, 1484000));
    assert(!feed(&p, &s, 6200, DESK_DISPLAY_MAX_CM + 1, 0, 0x60, 1512000));
    assert(!feed(&p, &s, 6200, 112, 0, 0x68, 1540000));
    const uint8_t broken_pair[] = {0xAA, 9, 0x18, 0x14, 0, 0, 0x35,
                                   0x60, 0x61, 0, 0, 112, 112};
    for (unsigned i = 0; i < sizeof(broken_pair); ++i)
        assert(!desk_feed_rx(&p, broken_pair[i], &event));
    assert(p.state_pair_errors == 1);
    puts("OK: 112->68->112->111, preset/current separation, invalid STATE, raw motion, transport gaps");
    return 0;
}
