#include "desk_protocol.h"
#include <assert.h>
#include <stdio.h>

static desk_event_t feed_rx_frame(desk_parser_t *p, const uint8_t *a, size_t n) {
    desk_event_t event = {0};
    bool got = false;
    for (size_t i = 0; i < n; ++i) {
        desk_event_t result = {0};
        if (desk_feed_rx(p, a[i], &result)) {
            assert(!got);
            event = result;
            got = true;
        }
    }
    assert(got);
    return event;
}

int main(void) {
    desk_parser_t p;
    desk_parser_reset(&p);
    const uint8_t f90[] = {0xAA,0x09,0x14,0xBD,0x00,0x00,0xDA,
                           0x62,0x62,0x00,0x00,0x5A,0x5A};
    desk_event_t event = feed_rx_frame(&p, f90, sizeof(f90));
    assert(event.type == DESK_EVENT_RX_STATE);
    assert(event.position_raw == 5309 && event.height_cm == 90);
    assert(event.scan_code == 0x62 && event.mode == 0x00);
    const uint8_t f77[] = {0xAA,0x09,0x12,0xB6,0x00,0x00,0xD1,
                           0x60,0x60,0x03,0x03,0x4D,0x4D};
    event = feed_rx_frame(&p, f77, sizeof(f77));
    assert(event.height_cm == 77 && event.position_raw == 4790);
    // Faulty checksum, followed by a valid frame; parser must recover.
    const uint8_t corrupt[] = {0x00,0xBB,0xAA,0x09,0x14,0xBD,0x00,0x00,0x01,
                               0x62,0x62,0x00,0x00,0x5A,0x5A};
    for (size_t i = 0; i < sizeof(corrupt); ++i)
        assert(!desk_feed_rx(&p, corrupt[i], &event));
    event = feed_rx_frame(&p, f90, sizeof(f90));
    assert(event.height_cm == 90 && p.invalid > 0);

    desk_parser_reset(&p);
    const uint8_t combined[] = {
        0xBB,0x01,0x12,0xC4,0x00,0xD7,
        0xD8,0xD8,0x66,0x02,0x02,
        0xD8,0xD8,0x66,0x00,0x00,
        0xD8,0xD8,0x66,0x80,0x80,
    };
    int count = 0;
    for (size_t i = 0; i < sizeof(combined); ++i) {
        if (!desk_feed_tx(&p, combined[i], &event)) continue;
        switch (count++) {
            case 0: assert(event.type == DESK_EVENT_TX_POS && event.position_raw == 4804); break;
            case 1: assert(event.type == DESK_EVENT_TX_BUTTON && event.button == 2); break;
            case 2: assert(event.type == DESK_EVENT_TX_BUTTON && event.button == 0); break;
            case 3: assert(event.type == DESK_EVENT_TX_BUTTON && event.button == 0x80); break;
            default: assert(0);
        }
    }
    assert(count == 4 && p.invalid == 0);
    puts("OK: RX 77/90, checksum/recovery, BB, D8/UP/RELEASED/SLEEP?");
    return 0;
}
