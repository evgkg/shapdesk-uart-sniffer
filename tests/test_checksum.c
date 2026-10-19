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
        assert(event.aa_byte5 == 0);
    }
    // Observed AA bytes, followed by synthetic STATE (not supplied in this capture).
    uint8_t packet[] = {0xAA,9,0x11,0xE6,2,1,2,0x60,0x60,3,3,112,112};
    assert(desk_aa_checksum(packet) == 2);
    // AA alone cannot emit a height event; retain it until STATE completes.
    assert(feed(&p, packet, 7, &event) == 0);
    assert(p.decode_errors == 0 && p.len == 7);
    assert(feed(&p, packet + 7, 6, &event) == 1);
    assert(event.aa_byte5 == 1 && event.position_raw == 0x11E6);
    assert(event.height_cm == 112 && event.button == 2 && event.scan_code == 0x60 && event.mode == 3);
    assert(memcmp(event.aa, packet, 7) == 0 && memcmp(event.state, packet + 7, 6) == 0);
    packet[6] = 3; // old sum including byte5 must now be rejected
    assert(feed(&p, packet, 13, &event) == 0);
    assert(p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 1 && p.skipped == 12);
    packet[6] = 2;
    assert(feed(&p, packet, 13, &event) == 1); // recovery
    packet[5] = 0;
    assert(feed(&p, packet, 13, &event) == 1 && event.aa_byte5 == 0);
    // No semantics assigned to byte5: any value is retained and excluded from sum.
    for (unsigned value = 0; value < 256; ++value) {
        packet[5] = (uint8_t)value;
        assert(desk_aa_checksum(packet) == 2);
        assert(feed(&p, packet, 13, &event) == 1 && event.aa_byte5 == value);
    }
    // Modulo 256, valid STATE, and unknown-field preservation.
    const uint8_t wrap[] = {0xAA,9,0xFF,0xFF,0xFF,1,6,0x98,0x98,0,0,90,90};
    assert(desk_aa_checksum(wrap) == 6 && feed(&p, wrap, 13, &event) == 1);
    assert(event.aa_byte5 == 1 && event.position_raw == 65535);
    // Confirm STATE validation remains strict even with the corrected checksum.
    packet[7] = packet[8] = 0xFF;
    assert(feed(&p, packet, 13, &event) == 0 && p.invalid_state == 1);
    puts("OK: byte5=00/01, observed AA, old-sum rejection, all byte5 values, modulo/recovery/STATE");
    return 0;
}
