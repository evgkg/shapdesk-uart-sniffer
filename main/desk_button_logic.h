#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DESK_UP_MIN_MS 50
#define DESK_UP_MAX_MS 5000
#define DESK_UP_RX_GUARD_US 500000
// Temporary button-only bench mode, explicitly requested by the owner.
// Set to 1 to restore RX-silence and UART-error release interlocks.
#ifndef DESK_UP_REQUIRE_UART_FEEDBACK
#define DESK_UP_REQUIRE_UART_FEEDBACK 0
#endif

typedef enum { DESK_CMD_NONE, DESK_CMD_INVALID, DESK_CMD_UP, DESK_CMD_STOP, DESK_CMD_STATUS } desk_command_kind_t;
typedef struct { desk_command_kind_t kind; unsigned duration_ms; } desk_command_t;
typedef struct { char line[32]; size_t len; bool invalid; } desk_command_stream_t;
desk_command_t desk_command_parse(const char *line);
desk_command_t desk_command_feed(desk_command_stream_t *stream, unsigned char byte);

typedef enum { DESK_RELEASE_NONE, DESK_RELEASE_TIMEOUT, DESK_RELEASE_STOP,
    DESK_RELEASE_RX_LOSS, DESK_RELEASE_UART_ERROR, DESK_RELEASE_USB_LOSS,
    DESK_RELEASE_SHUTDOWN } desk_release_reason_t;
typedef struct { bool active; int64_t started_us, deadline_us, last_rx_us; } desk_button_policy_t;
bool desk_button_policy_start(desk_button_policy_t *state, unsigned ms, int64_t now);
desk_release_reason_t desk_button_policy_tick(const desk_button_policy_t *state, int64_t now);
