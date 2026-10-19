#include "desk_protocol.h"

#include <string.h>

void desk_parser_reset(desk_parser_t *parser) {
    memset(parser, 0, sizeof(*parser));
}

void desk_parser_discard_partial(desk_parser_t *parser) {
    parser->skipped += parser->len;
    parser->len = 0;
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
    if (parser->len == sizeof(parser->buf)) {
        discard_prefix(parser, 1);
        ++parser->invalid;
        ++parser->decode_errors;
    }
    parser->buf[parser->len++] = byte;
}

static bool valid_aa(const uint8_t *p) {
    return p[0] == 0xAA && p[1] == 0x09 &&
           (uint8_t)(p[1] + p[2] + p[3] + p[4] + p[5]) == p[6];
}

bool desk_feed_rx(desk_parser_t *parser, uint8_t byte, desk_event_t *event) {
    push(parser, byte);
    while (parser->len) {
        if (parser->buf[0] != 0xAA) {
            discard_prefix(parser, 1);
            ++parser->skipped;
            continue;
        }
        if (parser->len < 7) return false;
        if (!valid_aa(parser->buf)) {
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
            discard_prefix(parser, 1);
            ++parser->skipped;
            continue;
        }
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
