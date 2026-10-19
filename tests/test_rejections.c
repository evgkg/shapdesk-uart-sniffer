#include "desk_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t valid[] = {
    0xAA, 9, 0x14, 0xBD, 0, 0, 0xDA, 0x62, 0x62, 0, 0, 90, 90
};

static unsigned feed(desk_parser_t *p, const uint8_t *data, size_t len) {
    unsigned events = 0;
    desk_event_t event = {0};
    for (size_t i = 0; i < len; ++i) events += desk_feed_rx(p, data[i], &event);
    return events;
}

int main(void) {
    desk_parser_t p = {0};
    uint8_t bad[13];
    memcpy(bad, valid, sizeof(bad));
    bad[6] ^= 1;
    assert(feed(&p, bad, 7) == 0);
    assert(p.decode_errors == 1 && p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 1);
    const desk_reject_sample_t *sample = &p.reject_samples[DESK_REJECT_AA_CHECKSUM];
    assert(sample->len == 7 && sample->decision_len == 7 && !sample->closed && sample->offset == 1);
    assert(feed(&p, bad + 7, 6) == 0);
    assert(sample->len == 13 && sample->closed && memcmp(sample->bytes, bad, 13) == 0);
    assert(p.skipped == 12 && p.invalid_state == 0);
    desk_reject_sample_t saved = *sample;
    // Bounded examples: later errors count but never overwrite the first sample.
    bad[12] = 91;
    assert(feed(&p, bad, 13) == 0);
    assert(p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 2);
    assert(memcmp(sample, &saved, sizeof(saved)) == 0);
    assert(feed(&p, valid, sizeof(valid)) == 1);

    desk_parser_reset(&p);
    memcpy(bad, valid, 13);
    bad[1] = 8;
    bad[6] = 0xD9; // correct sum for this unsupported header
    assert(feed(&p, bad, 13) == 0);
    assert(p.reject_counts[DESK_REJECT_AA_HEADER] == 1 && p.decode_errors == 1);
    assert(p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 0 && p.skipped == 12);
    // Header takes precedence when both header and checksum disagree.
    bad[6] = 0;
    assert(feed(&p, bad, 13) == 0);
    assert(p.reject_counts[DESK_REJECT_AA_HEADER] == 2 && p.decode_errors == 2);

    desk_parser_reset(&p);
    memcpy(bad, valid, 13);
    bad[7] = bad[8] = 0x68; // unknown scan: policy refusal, not proven corruption
    assert(feed(&p, bad, 13) == 0);
    assert(p.invalid_state == 1 && p.decode_errors == 0);
    assert(p.reject_samples[DESK_REJECT_STATE].state_flags == DESK_STATE_UNKNOWN_SCAN_POLICY);
    assert(memcmp(p.reject_samples[DESK_REJECT_STATE].bytes, bad, 13) == 0);
    assert(feed(&p, valid, 13) == 1);
    desk_parser_reset(&p);
    memcpy(bad, valid, 13);
    bad[7] = 0xFF; bad[8] = 0xFE; bad[11] = bad[12] = 3;
    assert(feed(&p, bad, 13) == 0);
    assert(p.reject_samples[DESK_REJECT_STATE].state_flags ==
           (DESK_STATE_BAD_PAIRS | DESK_STATE_UNKNOWN_SCAN_POLICY | DESK_STATE_HEIGHT_POLICY));

    // Exact arithmetic of the reported interval, with no dropped UART bytes.
    desk_parser_reset(&p);
    for (unsigned i = 0; i < 490; ++i) assert(feed(&p, valid, 13) == 1);
    memcpy(bad, valid, 13); bad[6] ^= 1;
    for (unsigned i = 0; i < 36; ++i) assert(feed(&p, bad, 13) == 0);
    const uint8_t partial[] = {0xAA, 9};
    assert(feed(&p, partial, 2) == 0);
    assert(p.bytes_fed == 6840 && p.good == 490 && p.decode_errors == 36);
    assert(p.skipped == 432 && p.len == 2 && p.invalid_state == 0);
    assert(p.reject_counts[DESK_REJECT_AA_CHECKSUM] == 36);
    assert(p.reject_counts[DESK_REJECT_DESYNC] == 36);
    desk_parser_discard_partial(&p);
    assert(p.reject_counts[DESK_REJECT_TRANSPORT_PARTIAL] == 1 && p.len == 0);
    sample = &p.reject_samples[DESK_REJECT_TRANSPORT_PARTIAL];
    assert(sample->closed && sample->len == 2 && sample->offset == 6839);
    assert(feed(&p, valid, 13) == 1 && sample->len == 2);

    // An interrupted context must not append bytes from after the transport gap.
    desk_parser_reset(&p);
    assert(feed(&p, bad, 7) == 0);
    desk_parser_discard_partial(&p);
    sample = &p.reject_samples[DESK_REJECT_AA_CHECKSUM];
    assert(sample->closed && sample->len == 7);
    assert(feed(&p, valid, 13) == 1 && sample->len == 7);

    // Defensive capacity branch: unreachable through a normal RX feed, tested explicitly.
    desk_parser_reset(&p);
    memset(p.buf, 0, sizeof(p.buf)); p.len = 13; p.bytes_fed = 13;
    const uint8_t zero = 0;
    assert(feed(&p, &zero, 1) == 0);
    assert(p.reject_counts[DESK_REJECT_CAPACITY] == 1 && p.decode_errors == 1);
    assert(p.reject_samples[DESK_REJECT_CAPACITY].offset == 1);

    desk_parser_reset(&p);
    const uint8_t bad_tx[] = {0xD8, 0xD8, 0x66, 1, 2};
    desk_event_t ev = {0};
    for (unsigned i = 0; i < sizeof(bad_tx); ++i) assert(!desk_feed_tx(&p, bad_tx[i], &ev));
    assert(p.reject_counts[DESK_REJECT_TX_VALIDATION] == 1 && p.decode_errors == 1);
    puts("OK: rejection causes, bounded raw context, unchanged recovery, 6840/490/36/432 reproduction");
    return 0;
}
