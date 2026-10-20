#pragma once
#include "esp_err.h"
#include "desk_button_logic.h"

#define DESK_UP_GPIO 5
typedef struct { bool active; int gpio_level; unsigned remaining_ms; uint32_t log_dropped; } desk_button_status_t;
// Sets LOW first, then arms independent hardware-timer supervision. Never presses.
esp_err_t desk_button_init(void);
void desk_button_uart_rx(void);
void desk_button_stop(desk_release_reason_t reason);
desk_button_status_t desk_button_status(void);
// Nonblocking event consumption; call only from the existing logger task.
void desk_button_log_events(void);
// Launch USB-only command reader; notification requests a snapshot from UART owner.
esp_err_t desk_button_console_start(void (*request_status)(void));
