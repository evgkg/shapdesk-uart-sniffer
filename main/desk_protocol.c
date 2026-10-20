#include "desk_protocol.h"

#include <string.h>

uint16_t desk_aa_checksum(const uint8_t aa[7]) {
    return (uint16_t)aa[1] + aa[2] + aa[3] + aa[4];
}

const char *desk_reject_reason_name(desk_reject_reason_t reason) {
    static const char *const names[] = {"AA_HEADER", "AA_CHECKSUM", "STATE_VALIDATION",
        "DESYNCHRONIZATION", "PARSER_CAPACITY", "TX_VALIDATION", "TRANSPORT_PARTIAL"};
    return (unsigned)reason < DESK_REJECT_REASON_COUNT ? names[reason] : "UNKNOWN";
}

static void record_reject(desk_parser_t *p, desk_reject_reason_t reason, uint8_t flags) {
    ++p->reject_counts[reason];
    desk_reject_sample_t *s = &p->reject_samples[reason];
    if (s->len || !p->len) return;
    s->len = p->len;
    s->decision_len = p->len;
    s->state_flags = flags;
    s->offset = p->bytes_fed - p->len + 1;
    memcpy(s->bytes, p->buf, s->len);
    s->closed = s->len == sizeof(s->bytes);
}

static void capture_context(desk_parser_t *p, uint8_t byte) {
    ++p->bytes_fed;
    for (unsigned i = 0; i < DESK_REJECT_REASON_COUNT; ++i) {
        desk_reject_sample_t *s = &p->reject_samples[i];
        if (s->len && !s->closed) {
            s->bytes[s->len++] = byte;
            s->closed = s->len == sizeof(s->bytes);
        }
    }
}

void desk_parser_reset(desk_parser_t *parser) {
    memset(parser, 0, sizeof(*parser));
}

void desk_parser_discard_partial(desk_parser_t *parser) {
    if (parser->len) record_reject(parser, DESK_REJECT_TRANSPORT_PARTIAL, 0);
    for (unsigned i = 0; i < DESK_REJECT_REASON_COUNT; ++i)
        if (parser->reject_samples[i].len) parser->reject_samples[i].closed = true;
    parser->skipped += parser->len;
    parser->len = 0;
    parser->desync_active = false;
}

static void discard_prefix(desk_parser_t *parser, size_t n) {
    if (n >= parser->len) {
        parser->len = 0;
    } else {
        memmove(parser->buf, parser->buf + n, parser->len - n);
        parser->len -= (uint8_t)n;
    }
}

static void push(desk_parser_t *parser, uint8_t byte) {
    // Capture before mutation. This never delays a parsing decision for more bytes.
    if (parser->len == sizeof(parser->buf)) {
        record_reject(parser, DESK_REJECT_CAPACITY, 0);
    }
    capture_context(parser, byte);
    if (parser->len == sizeof(parser->buf)) {
        discard_prefix(parser, 1);
        ++parser->invalid;
        ++parser->decode_errors;
    }
    parser->buf[parser->len++] = byte;
}

static void record_skip(desk_parser_t *parser) {
    if (!parser->desync_active) record_reject(parser, DESK_REJECT_DESYNC, 0);
    parser->desync_active = true;
}

bool desk_feed_rx(desk_parser_t *parser, uint8_t byte, desk_event_t *event) {
    push(parser, byte);
    while (parser->len) {
        if (parser->buf[0] != 0xAA) {
            record_skip(parser);
            discard_prefix(parser, 1);
            ++parser->skipped;
            continue;
        }
        parser->desync_active = false;
        if (parser->len < 7) return false;
        const uint8_t *aa = parser->buf;
        const uint16_t checksum = desk_aa_checksum(aa);
        const uint16_t received = ((uint16_t)aa[5] << 8) | aa[6];
        if (aa[1] != 0x09 || checksum != received) {
            record_reject(parser, aa[1] != 0x09 ? DESK_REJECT_AA_HEADER : DESK_REJECT_AA_CHECKSUM, 0);
            discard_prefix(parser, 1);
            ++parser->invalid;
            ++parser->decode_errors;
            continue;
        }
        if (parser->len < 13) return false;
        const uint8_t *s = parser->buf + 7;
        const bool scan_ok = (s[0] >= 0x60 && s[0] <= 0x67) || s[0] == 0x98;
        const bool pairs_ok = s[0] == s[1] && s[2] == s[3] && s[4] == s[5];
        const bool height_ok = s[4] >= DESK_DISPLAY_MIN_CM && s[4] <= DESK_DISPLAY_MAX_CM;
        if (!pairs_ok || !scan_ok || !height_ok) {
            record_reject(parser, DESK_REJECT_STATE,
                (!pairs_ok ? DESK_STATE_BAD_PAIRS : 0) |
                (!scan_ok ? DESK_STATE_UNKNOWN_SCAN_POLICY : 0) |
                (!height_ok ? DESK_STATE_HEIGHT_POLICY : 0));
            memcpy(parser->last_rejected_state, s, sizeof(parser->last_rejected_state));
            if (!pairs_ok) ++parser->state_pair_errors;
            if (!scan_ok) ++parser->state_scan_errors;
            if (!height_ok) ++parser->state_height_errors;
            // Another AA could have started after the first; preserve it.
            discard_prefix(parser, 1);
            ++parser->invalid;
            ++parser->invalid_state;
            continue;
        }
        *event = (desk_event_t){
            .type = DESK_EVENT_RX_STATE,
            .position_raw = (uint16_t)((parser->buf[2] << 8) | parser->buf[3]),
            .height_cm = s[4],
            .button = parser->buf[4],
            .aa_checksum = received,
            .scan_code = s[0],
            .mode = s[2],
        };
        memcpy(event->aa, parser->buf, sizeof(event->aa));
        memcpy(event->state, s, sizeof(event->state));
        ++parser->good;
        discard_prefix(parser, 13);
        return true;
    }
    return false;
}

static bool valid_bb(const uint8_t *p) {
    if (p[0] != 0xBB || p[1] != 0x01) return false;
    uint16_t checksum = (uint16_t)p[1] + p[2] + p[3];
    return p[4] == (uint8_t)(checksum >> 8) &&
           p[5] == (uint8_t)checksum;
}

static bool valid_d8(const uint8_t *p) {
    return p[0] == 0xD8 && p[1] == 0xD8 && p[2] == 0x66 && p[3] == p[4];
}

bool desk_feed_tx(desk_parser_t *parser, uint8_t byte, desk_event_t *event) {
    push(parser, byte);
    while (parser->len) {
        if (parser->buf[0] != 0xBB && parser->buf[0] != 0xD8) {
            record_skip(parser);
            discard_prefix(parser, 1);
            ++parser->skipped;
            continue;
        }
        parser->desync_active = false;
        size_t length = parser->buf[0] == 0xBB ? 6 : 5;
        if (parser->len < length) return false;
        if (parser->buf[0] == 0xBB && valid_bb(parser->buf)) {
            *event = (desk_event_t){
                .type = DESK_EVENT_TX_POS,
                .position_raw = (uint16_t)((parser->buf[2] << 8) | parser->buf[3]),
            };
        } else if (parser->buf[0] == 0xD8 && valid_d8(parser->buf)) {
            *event = (desk_event_t){
                .type = DESK_EVENT_TX_BUTTON,
                .button = parser->buf[3],
            };
        } else {
            record_reject(parser, DESK_REJECT_TX_VALIDATION, 0);
            discard_prefix(parser, 1);
            ++parser->invalid;
            ++parser->decode_errors;
            continue;
        }
        ++parser->good;
        discard_prefix(parser, length);
        return true;
    }
    return false;
}
