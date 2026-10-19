#include "desk_protocol.h"

#include <string.h>

void desk_parser_reset(desk_parser_t *parser) {
    memset(parser, 0, sizeof(*parser));
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
            continue;
        }
        if (parser->len < 13) return false;
        const uint8_t *s = parser->buf + 7;
        if (s[0] != s[1] || s[2] != s[3] || s[4] != s[5]) {
            // Another AA could have started after the first; preserve it.
            discard_prefix(parser, 1);
            ++parser->invalid;
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
            continue;
        }
        ++parser->good;
        discard_prefix(parser, length);
        return true;
    }
    return false;
}
