#include "desk_button_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static desk_command_t line(desk_command_stream_t *s, const char *text) {
    desk_command_t result = {.kind = DESK_CMD_NONE};
    for (size_t i = 0; i < strlen(text); ++i) {
        desk_command_t c = desk_command_feed(s, (unsigned char)text[i]);
        if (c.kind != DESK_CMD_NONE) result = c;
    }
    return result;
}

int main(void) {
    const char *invalid[] = {"up", "up 49", "up 5001", "up -300", "up +300", "up 0",
        "up 3e2", "up 300.0", "up 300 extra", "up 300 stop", "up 999999999999999999999",
        "stop 1", "status 1", "UP 300", "up300", "unknown"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(desk_command_parse(invalid[i]).kind == DESK_CMD_INVALID);
    assert(desk_command_parse("up 50").duration_ms == 50);
    assert(desk_command_parse("up 1000").duration_ms == 1000);
    assert(desk_command_parse("  up\t300  ").duration_ms == 300);
    assert(desk_command_parse("stop").kind == DESK_CMD_STOP);
    assert(desk_command_parse("status").kind == DESK_CMD_STATUS);
    desk_command_stream_t stream = {0};
    assert(line(&stream, "up 3").kind == DESK_CMD_NONE);
    assert(line(&stream, "00\r\n").kind == DESK_CMD_UP); // split read + CRLF
    assert(line(&stream, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx up 300\n").kind == DESK_CMD_INVALID);
    assert(line(&stream, "up 300\n").kind == DESK_CMD_UP); // overflow recovery
    assert(desk_command_feed(&stream, 0).kind == DESK_CMD_NONE);
    assert(line(&stream, "up 300\n").kind == DESK_CMD_INVALID); // embedded NUL
    assert(line(&stream, "\033up 300\n").kind == DESK_CMD_INVALID);

    desk_button_policy_t state = {0};
    assert(!state.active && desk_button_policy_tick(&state, 0) == DESK_RELEASE_NONE);
    assert(!desk_button_policy_start(&state, 49, 0));
    assert(!desk_button_policy_start(&state, 5001, 0));
    assert(desk_button_policy_start(&state, 300, 1000000));
    assert(state.deadline_us == 1300000);
    assert(!desk_button_policy_start(&state, 1000, 1100000)); // busy never extends deadline
    assert(state.deadline_us == 1300000);
    assert(desk_button_policy_tick(&state, 1299999) == DESK_RELEASE_NONE);
    assert(desk_button_policy_tick(&state, 1300000) == DESK_RELEASE_TIMEOUT);
    state.active = false; // adapter's immediate stop/release
    assert(desk_button_policy_tick(&state, 2000000) == DESK_RELEASE_NONE);
    assert(desk_button_policy_start(&state, 1000, 2000000));
    assert(desk_button_policy_tick(&state, 2499999) == DESK_RELEASE_NONE);
    assert(desk_button_policy_tick(&state, 2500000) ==
        (DESK_UP_REQUIRE_UART_FEEDBACK ? DESK_RELEASE_RX_LOSS : DESK_RELEASE_NONE));
    state.last_rx_us = 2490000;
    assert(desk_button_policy_tick(&state, 2500000) == DESK_RELEASE_NONE);
    assert(desk_button_policy_tick(&state, 2990000) ==
        (DESK_UP_REQUIRE_UART_FEEDBACK ? DESK_RELEASE_RX_LOSS : DESK_RELEASE_NONE));
    state.last_rx_us = 2999000;
    assert(desk_button_policy_tick(&state, 3000000) == DESK_RELEASE_TIMEOUT);
    assert(desk_command_parse("up 5000").kind == DESK_CMD_UP);
    assert(desk_command_parse("up 5000").duration_ms == 5000);
    assert(desk_command_parse("up 1001").kind == DESK_CMD_UP);
    state = (desk_button_policy_t){0};
    assert(desk_button_policy_start(&state, 5000, 10000000));
    assert(state.deadline_us == 15000000);
    assert(!desk_button_policy_start(&state, 5000, 11000000));
    assert(state.deadline_us == 15000000);
    // Continuous valid RX keeps the guard satisfied for the whole five seconds.
    for (int64_t now = 10000000; now < 15000000; now += 100000) {
        state.last_rx_us = now;
        assert(desk_button_policy_tick(&state, now) == DESK_RELEASE_NONE);
    }
    assert(desk_button_policy_tick(&state, 15000000) == DESK_RELEASE_TIMEOUT);
    state = (desk_button_policy_t){0};
    assert(desk_button_policy_start(&state, 5000, 20000000));
    assert(desk_button_policy_tick(&state, 20500000) ==
        (DESK_UP_REQUIRE_UART_FEEDBACK ? DESK_RELEASE_RX_LOSS : DESK_RELEASE_NONE));
    assert(desk_button_policy_tick(&state, 24999999) ==
        (DESK_UP_REQUIRE_UART_FEEDBACK ? DESK_RELEASE_RX_LOSS : DESK_RELEASE_NONE));
    assert(desk_button_policy_tick(&state, 25000000) == DESK_RELEASE_TIMEOUT);
    puts("OK: strict commands/framing, bounds, busy, deadline, stop, RX watchdog");
    return 0;
}
