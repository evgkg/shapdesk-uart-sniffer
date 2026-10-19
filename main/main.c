#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "desk_protocol.h"
#include "desk_diagnostics.h"

// Receive-only desk tap. Native USB Serial/JTAG uses GPIO18/19 on ESP32-C3.
#define DESK_BOARD_RX_GPIO 4  // Shapdesk pad RX (yellow), through safe level translation
#define DESK_BAUD          9600

static const char *TAG = "shapdesk";
static desk_parser_t incoming_from_controller;
static desk_diagnostics_t diagnostics;

typedef struct {
    bool height_known;
    uint8_t height_cm;
    uint16_t controller_position;
    uint8_t controller_button;
    uint8_t scan_code;
    uint8_t mode;
    int64_t last_rx_us;
} desk_state_t;
static desk_state_t desk;

static void log_diagnostic(desk_diag_kind_t kind, const desk_diag_sample_t *sample,
                           void *ctx) {
    (void)ctx;
    const desk_event_t *ev = &sample->event;
    char aa[3 * sizeof(ev->aa)];
    char state[3 * sizeof(ev->state)];
    for (size_t i = 0; i < sizeof(ev->aa); ++i) {
        snprintf(aa + 3 * i, sizeof(aa) - 3 * i, "%02X%s",
                 (unsigned)ev->aa[i], i + 1 == sizeof(ev->aa) ? "" : " ");
    }
    for (size_t i = 0; i < sizeof(ev->state); ++i) {
        snprintf(state + 3 * i, sizeof(state) - 3 * i, "%02X%s",
                 (unsigned)ev->state[i], i + 1 == sizeof(ev->state) ? "" : " ");
    }
    const char *label = kind == DESK_DIAG_BEFORE ? "BEFORE" :
                        kind == DESK_DIAG_ANOMALY ? "ANOMALY" : "AFTER";
    ESP_LOGI(TAG, "DIAG %s seq=%" PRIu32 " rx_us=%" PRId64
             " position_raw=%u height_cm=%u button=0x%02X scan_code=0x%02X mode=0x%02X"
             " | AA=[%s] STATE=[%s]", label, sample->sequence, sample->time_us,
             (unsigned)ev->position_raw, (unsigned)ev->height_cm,
             (unsigned)ev->button, (unsigned)ev->scan_code, (unsigned)ev->mode, aa, state);
}

static void handle_rx(const desk_event_t *ev) {
    desk.last_rx_us = esp_timer_get_time();
    bool change = !desk.height_known || desk.height_cm != ev->height_cm;
    if (!desk.height_known || desk.controller_button != ev->button) {
        ESP_LOGI(TAG, "CTRL_COMMAND 0x%02X | raw=%u | height_cm=%u",
                 (unsigned)ev->button, (unsigned)ev->position_raw, (unsigned)ev->height_cm);
    }
    desk_diagnostics_feed(&diagnostics, ev, desk.last_rx_us, log_diagnostic, NULL);
    desk.height_cm = ev->height_cm;
    desk.height_known = true;
    desk.controller_position = ev->position_raw;
    desk.controller_button = ev->button;
    desk.scan_code = ev->scan_code;
    desk.mode = ev->mode;
    if (change) {
        ESP_LOGI(TAG, "HEIGHT %u cm | raw=%u | ctrl_command=0x%02X | mode=0x%02X",
                 (unsigned)ev->height_cm, (unsigned)ev->position_raw,
                 (unsigned)ev->button, (unsigned)ev->mode);
    }
}

static void init_uart_rx_only(uart_port_t port, int gpio) {
    const uart_config_t settings = {
        .baud_rate = DESK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(port, &settings));
    // UART TX is NOT routed to any GPIO. No uart_write_bytes anywhere.
    ESP_ERROR_CHECK(uart_set_pin(port, UART_PIN_NO_CHANGE, gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port, 2048, 0, 0, NULL, 0));
}

void app_main(void) {
    desk_parser_reset(&incoming_from_controller);
    init_uart_rx_only(UART_NUM_1, DESK_BOARD_RX_GPIO);
    ESP_LOGI(TAG, "PASSIVE SNIFFER: 9600 8N1, desk RX -> UART1 GPIO%d",
             DESK_BOARD_RX_GPIO);
    ESP_LOGW(TAG, "Listen-only. NEVER attach desk UART directly to ESP32 GPIO without safe level shifting.");

    uint8_t rx_buf[256];
    desk_event_t ev;
    int64_t last_report = esp_timer_get_time();
    while (true) {
        int n = uart_read_bytes(UART_NUM_1, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(10));
        for (int i = 0; i < n; ++i) {
            if (desk_feed_rx(&incoming_from_controller, rx_buf[i], &ev)) {
                handle_rx(&ev);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 5000000) {
            const bool fresh = desk.height_known && (now - desk.last_rx_us < 1000000);
            char height_text[16];
            if (desk.height_known) {
                snprintf(height_text, sizeof(height_text), "%u cm", (unsigned)desk.height_cm);
            } else {
                snprintf(height_text, sizeof(height_text), "unknown");
            }
            char command_text[8];
            if (desk.height_known) {
                snprintf(command_text, sizeof(command_text), "0x%02X",
                         (unsigned)desk.controller_button);
            } else {
                snprintf(command_text, sizeof(command_text), "unknown");
            }
            ESP_LOGI(TAG, "STATUS height=%s ctrl_command=%s | RX AA+state=%lu invalid=%lu skipped=%lu | "
                          "anomalies=%lu RX live=%s",
                     height_text,
                     command_text,
                     (unsigned long)incoming_from_controller.good,
                     (unsigned long)incoming_from_controller.invalid,
                     (unsigned long)incoming_from_controller.skipped,
                     (unsigned long)diagnostics.anomalies,
                     fresh ? "yes" : "no (stale)");
            last_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
