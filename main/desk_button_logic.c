#include "desk_button_logic.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

static bool space(char c) { return c == ' ' || c == '\t'; }

desk_command_t desk_command_parse(const char *line) {
    desk_command_t command = {.kind = DESK_CMD_INVALID};
    while (space(*line)) ++line;
    size_t len = strlen(line);
    while (len && space(line[len - 1])) --len;
    if (!len) return (desk_command_t){.kind = DESK_CMD_NONE};
    if (len == 4 && memcmp(line, "stop", 4) == 0) return (desk_command_t){.kind = DESK_CMD_STOP};
    if (len == 6 && memcmp(line, "status", 6) == 0) return (desk_command_t){.kind = DESK_CMD_STATUS};
    if (len < 4 || line[0] != 'u' || line[1] != 'p' || !space(line[2])) return command;
    size_t i = 2;
    while (i < len && space(line[i])) ++i;
    if (i == len) return command;
    unsigned ms = 0;
    for (; i < len; ++i) {
        if (line[i] < '0' || line[i] > '9') return command;
        ms = ms * 10 + (unsigned)(line[i] - '0');
        if (ms > DESK_UP_MAX_MS) return command; // bound before integer overflow
    }
    if (ms < DESK_UP_MIN_MS) return command;
    return (desk_command_t){.kind = DESK_CMD_UP, .duration_ms = ms};
}

desk_command_t desk_command_feed(desk_command_stream_t *s, unsigned char byte) {
    if (byte == '\r' || byte == '\n') {
        s->line[s->len] = 0;
        desk_command_t command = s->invalid ? (desk_command_t){.kind = DESK_CMD_INVALID} : desk_command_parse(s->line);
        s->len = 0;
        s->invalid = false;
        return command;
    }
    if (byte != '\t' && (byte < 32 || byte > 126)) s->invalid = true;
    if (s->len + 1 >= sizeof(s->line)) s->invalid = true;
    if (!s->invalid) s->line[s->len++] = (char)byte;
    return (desk_command_t){.kind = DESK_CMD_NONE};
}

bool IRAM_ATTR desk_button_policy_start(desk_button_policy_t *s, unsigned ms, int64_t now) {
    if (s->active || ms < DESK_UP_MIN_MS || ms > DESK_UP_MAX_MS) return false;
    s->active = true;
    s->started_us = now;
    s->deadline_us = now + (int64_t)ms * 1000;
    return true;
}

desk_release_reason_t IRAM_ATTR desk_button_policy_tick(const desk_button_policy_t *s, int64_t now) {
    if (!s->active) return DESK_RELEASE_NONE;
    if (now >= s->deadline_us) return DESK_RELEASE_TIMEOUT;
    if (DESK_UP_REQUIRE_UART_FEEDBACK && now - s->started_us >= DESK_UP_RX_GUARD_US && now - s->last_rx_us >= DESK_UP_RX_GUARD_US)
        return DESK_RELEASE_RX_LOSS;
    return DESK_RELEASE_NONE;
}
