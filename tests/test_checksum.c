#include "desk_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned feed(desk_parser_t *p, const uint8_t *bytes, size_t len,
                     desk_event_t *event) {
    unsigned got = 0;
    for (size_t i = 0; i < len; ++i) got += desk_feed_rx(p, bytes[i], event);
    return got;
}

int main(void) {
    desk_parser_t p = {0};
    desk_event_t event = {0};
    const uint8_t known[][13] = {
        {0xAA,9,0x11,0x4A,0,0,0x64,0x62,0x62,0,0,67,67},
        {0xAA,9,0x12,0xB6,0,0,0xD1,0x60,0x60,3,3,77,77},
        {0xAA,9,0x14,0xBD,0,0,0xDA,0x62,0x62,0,0,90,90},
    };
    for (unsigned i = 0; i < 3; ++i) {
        assert(desk_aa_checksum(known[i]) == known[i][6]);
        assert(feed(&p, known[i], 13, &event) == 1);
        assert(event.aa_checksum == known[i][6]);
    }
    // Observed AA bytes, followed by synthetic STATE (not supplied in this capture).
    uint8_t packet[] = {0xAA,9,0x11,0xE6,2,1,2,0x60,0x60,3,3,112,112};
    assert(desk_aa_checksum(packet) == 0x0102);
    // AA alone cannot emit a height event; retain it until STATE completes.
    assert(feed(&p, packet, 7, &event) == 0);
    assert(p.decode_errors == 0 && p.len == 7);
    assert(feed(&p, packet + 7, 6, &event) == 1);
    assert(event.aa_checksum == 0x0102 && event.position_raw == 0x11E6);
    assert(event.height_cm == 112 && event.button == 2 && event.scan_code == 0x60 && event.mode == 3);
    assert(memcmp(event.aa, packet, 7) == 0 && memcmp(event.state, packet + 7, 6) == 0);
    packet[6] = 3; // old sum including byte5 must now be rejected
    assert(feed(&p, packet, 13, &event) == 0);
    assert(p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 1 && p.skipped == 12);
    packet[6] = 2;
    assert(feed(&p, packet, 13, &event) == 1); // recovery
    // Corrupt only the high byte; low-byte-only validation would miss this.
    packet[5] = 0;
    assert(feed(&p, packet, 13, &event) == 0);
    assert(p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 2);
    packet[5] = 1;
    assert(feed(&p, packet, 13, &event) == 1);
    const uint8_t carry_zero[] = {0xAA,9,0x11,0xE4,2,1,0,0x60,0x60,3,3,112,112};
    assert(desk_aa_checksum(carry_zero) == 0x0100);
    assert(feed(&p, carry_zero, 13, &event) == 1 && event.aa_checksum == 0x0100);
    const uint8_t no_carry[] = {0xAA,9,0x16,0xCC,0,0,0xEB,0x62,0x62,0,0,90,90};
    assert(desk_aa_checksum(no_carry) == 0x00EB);
    assert(feed(&p, no_carry, 13, &event) == 1 && event.aa_checksum == 0x00EB);
    // Sum can exceed 0x01FF; never truncate it to a byte.
    const uint8_t large[] = {0xAA,9,0xFF,0xFF,0xFF,3,6,0x98,0x98,0,0,90,90};
    assert(desk_aa_checksum(large) == 0x0306 && feed(&p, large, 13, &event) == 1);
    assert(event.aa_checksum == 0x0306 && event.position_raw == 65535);
    // Confirm STATE validation remains strict even with the corrected checksum.
    packet[7] = packet[8] = 0xFF;
    assert(feed(&p, packet, 13, &event) == 0 && p.invalid_state == 1);
    puts("OK: AA 00EB/0100/0102/0306, high/low corruption, regression/recovery/STATE");
    return 0;
}
