#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "desk_protocol.h"

// These two GPIOs are INPUTS ONLY in this application. No desk TX drive.
// Change them here if GPIO4/GPIO5 aren't exposed on your ESP32-S3 board.
#define DESK_BOARD_RX_GPIO 4  // Shapdesk board pad RX (yellow) -> ESP32 GPIO4
#define DESK_BOARD_TX_GPIO 5  // Shapdesk board pad TX (light blue) -> ESP32 GPIO5
#define DESK_BAUD          9600

static const char *TAG = "shapdesk";
static desk_parser_t incoming_from_controller;
static desk_parser_t outgoing_from_panel;

typedef struct {
    bool height_known;
    uint8_t height_cm;
    uint16_t controller_position;
    uint16_t panel_position;
    uint8_t panel_button;
    bool button_known;
    uint8_t controller_button;
    uint8_t scan_code;
    uint8_t mode;
    int64_t last_rx_us;
    int64_t last_tx_us;
} desk_state_t;
static desk_state_t desk;

static const char *button_name(uint8_t v) {
    switch (v) {
        case 0x00: return "RELEASED";
        case 0x01: return "DOWN";
        case 0x02: return "UP";
        case 0x04: return "PRESET_1?";
        case 0x08: return "PRESET_2?";
        case 0x10: return "PRESET_3?";
        case 0x20: return "PRESET_4?";
        case 0x40: return "MEMORY?";
        case 0x80: return "SLEEP?";
        default: return "UNKNOWN";
    }
}

static void handle_rx(const desk_event_t *ev) {
    desk.last_rx_us = esp_timer_get_time();
    bool change = !desk.height_known || desk.height_cm != ev->height_cm;
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

static void handle_tx(const desk_event_t *ev) {
    desk.last_tx_us = esp_timer_get_time();
    if (ev->type == DESK_EVENT_TX_POS) {
        desk.panel_position = ev->position_raw;
        return;
    }
    if (!desk.button_known || desk.panel_button != ev->button) {
        desk.panel_button = ev->button;
        desk.button_known = true;
        ESP_LOGI(TAG, "PANEL BUTTON %s (0x%02X)",
                 button_name(ev->button), (unsigned)ev->button);
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
    desk_parser_reset(&outgoing_from_panel);
    init_uart_rx_only(UART_NUM_1, DESK_BOARD_RX_GPIO);
    init_uart_rx_only(UART_NUM_2, DESK_BOARD_TX_GPIO);
    ESP_LOGI(TAG, "PASSIVE SNIFFER: 9600 8N1, desk RX -> GPIO%d, desk TX -> GPIO%d",
             DESK_BOARD_RX_GPIO, DESK_BOARD_TX_GPIO);
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
        n = uart_read_bytes(UART_NUM_2, rx_buf, sizeof(rx_buf), 0);
        for (int i = 0; i < n; ++i) {
            if (desk_feed_tx(&outgoing_from_panel, rx_buf[i], &ev)) {
                handle_tx(&ev);
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
            ESP_LOGI(TAG, "STATUS height=%s | RX AA+state=%lu invalid=%lu | "
                          "TX BB+D8=%lu invalid=%lu | panel_button=%s | RX live=%s",
                     height_text,
                     (unsigned long)incoming_from_controller.good,
                     (unsigned long)incoming_from_controller.invalid,
                     (unsigned long)outgoing_from_panel.good,
                     (unsigned long)outgoing_from_panel.invalid,
                     desk.button_known ? button_name(desk.panel_button) : "?",
                     fresh ? "yes" : "no (stale)");
            last_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
