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
#include "freertos/queue.h"

#include "desk_protocol.h"
#include "desk_diagnostics.h"
#include "desk_tracking.h"
#include "desk_button.h"

// Receive-only desk tap. Native USB Serial/JTAG uses GPIO18/19 on ESP32-C3.
#define DESK_BOARD_RX_GPIO 4  // Shapdesk pad RX (yellow), through safe level translation
#define DESK_BAUD          9600

static const char *TAG = "shapdesk";
static desk_parser_t incoming_from_controller;
static desk_diagnostics_t diagnostics;
#define RX_RING_SIZE 8192
#define UART_EVENT_QUEUE_SIZE 64
#define LOG_QUEUE_SIZE 64

static QueueHandle_t uart_events;
static QueueHandle_t log_queue;
static desk_tracking_t desk;
static TaskHandle_t uart_owner;

static void request_status(void) { xTaskNotifyGive(uart_owner); }

typedef struct {
    uint32_t bytes, fifo_overflow, ring_full, frame_errors, parity_errors, breaks;
    uint32_t read_errors, log_dropped, event_queue_full_polls;
    uint32_t recovery_flushes, recovery_discarded_bytes;
    size_t rx_peak;
    UBaseType_t event_peak, log_peak;
    int64_t read_gap_max_us;
} transport_stats_t;
static transport_stats_t transport;

typedef enum { LOG_DIAG, LOG_CHANGE, LOG_STATUS } log_kind_t;
typedef struct {
    log_kind_t kind;
    desk_diag_kind_t diag_kind;
    desk_diag_sample_t sample;
    desk_tracking_t state;
    desk_parser_t parser;
    transport_stats_t transport;
    uint32_t anomalies;
    bool command_changed, display_changed, motion_changed;
} log_record_t;

static void enqueue_log(const log_record_t *record) {
    if (xQueueSend(log_queue, record, 0) != pdTRUE) ++transport.log_dropped;
    UBaseType_t used = uxQueueMessagesWaiting(log_queue);
    if (used > transport.log_peak) transport.log_peak = used;
}

static void queue_diagnostic(desk_diag_kind_t kind, const desk_diag_sample_t *sample,
                             void *ctx) {
    (void)ctx;
    log_record_t record = {.kind = LOG_DIAG, .diag_kind = kind, .sample = *sample};
    enqueue_log(&record);
}

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
             " position_raw=%u height_cm=%u button=0x%02X aa_checksum=0x%04X scan_code=0x%02X mode=0x%02X"
             " | AA=[%s] STATE=[%s]", label, sample->sequence, sample->time_us,
             (unsigned)ev->position_raw, (unsigned)ev->height_cm,
             (unsigned)ev->button, (unsigned)ev->aa_checksum, (unsigned)ev->scan_code, (unsigned)ev->mode, aa, state);
}

static void log_task(void *ctx) {
    (void)ctx;
    log_record_t r;
    int64_t log_max_us = 0;
    bool printed_prefix[DESK_REJECT_REASON_COUNT] = {0};
    bool printed_complete[DESK_REJECT_REASON_COUNT] = {0};
    while (true) {
        desk_button_log_events();
        if (xQueueReceive(log_queue, &r, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        int64_t start = esp_timer_get_time();
        if (r.kind == LOG_DIAG) {
            log_diagnostic(r.diag_kind, &r.sample, NULL);
        } else {
            const desk_tracking_t *s = &r.state;
            char display[12] = "unknown", current[12] = "unknown", candidate[12] = "unknown";
            char command[12] = "unknown";
            if (s->known) {
                snprintf(display, sizeof(display), "%u", (unsigned)s->display_height);
                snprintf(command, sizeof(command), "0x%02X", (unsigned)s->ctrl_command);
            }
            if (s->current_known) snprintf(current, sizeof(current), "%u", (unsigned)s->current_height);
            if (s->candidate_known) snprintf(candidate, sizeof(candidate), "%u", (unsigned)s->preset_target_candidate);
            if (r.kind == LOG_CHANGE) {
                if (r.command_changed) ESP_LOGI(TAG, "CTRL_COMMAND %s", command);
                if (r.motion_changed) ESP_LOGI(TAG, "MOTION raw_active=%s direction=%d raw=%u",
                    s->moving ? "yes" : "no", s->motion_direction, (unsigned)s->position_raw);
                if (r.display_changed) ESP_LOGI(TAG,
                    "HEIGHT display=%s current=%s held=%s preset_target_candidate=%s raw=%u",
                    display, current, s->current_held ? "yes" : "no", candidate, (unsigned)s->position_raw);
            } else {
                desk_button_status_t button = desk_button_status();
                ESP_LOGI(TAG, "BUTTON GPIO5=%d active=%s remaining_ms=%u event_log_dropped=%lu",
                    button.gpio_level, button.active ? "yes" : "no", button.remaining_ms, (unsigned long)button.log_dropped);
                ESP_LOGI(TAG, "STATUS display=%s current=%s held=%s preset_target_candidate=%s ctrl_command=%s"
                    " raw=%u moving=%s direction=%d RX_live=%s", display, current,
                    s->current_held ? "yes" : "no", candidate, command, (unsigned)s->position_raw,
                    s->moving ? "yes" : "no", s->motion_direction, s->known ? "yes" : "no");
                ESP_LOGI(TAG, "PARSER good=%lu invalid=%lu decode_errors=%lu invalid_state=%lu skipped=%lu"
                    " display_changes=%lu display_transients=%lu anomalies=%lu",
                    (unsigned long)r.parser.good, (unsigned long)r.parser.invalid,
                    (unsigned long)r.parser.decode_errors, (unsigned long)r.parser.invalid_state,
                    (unsigned long)r.parser.skipped, (unsigned long)s->display_changes,
                    (unsigned long)s->display_transients, (unsigned long)r.anomalies);
                ESP_LOGI(TAG, "REJECT_COUNTS aa_header=%lu aa_checksum=%lu state_validation=%lu"
                    " desync_runs=%lu capacity=%lu tx_validation=%lu transport_partial=%lu",
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_AA_HEADER],
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_AA_CHECKSUM],
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_STATE],
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_DESYNC],
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_CAPACITY],
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_TX_VALIDATION],
                    (unsigned long)r.parser.reject_counts[DESK_REJECT_TRANSPORT_PARTIAL]);
                for (unsigned i = 0; i < DESK_REJECT_REASON_COUNT; ++i) {
                    const desk_reject_sample_t *sample = &r.parser.reject_samples[i];
                    if (!sample->len || (printed_prefix[i] && (!sample->closed || printed_complete[i]))) continue;
                    char bytes[3 * sizeof(sample->bytes)];
                    for (unsigned j = 0; j < sample->len; ++j)
                        snprintf(bytes + 3 * j, sizeof(bytes) - 3 * j, "%02X%s",
                            (unsigned)sample->bytes[j], j + 1 == sample->len ? "" : " ");
                    ESP_LOGI(TAG, "REJECT_SAMPLE reason=%s offset=%" PRIu64
                        " decision_len=%u captured=%u closed=%s state_flags=0x%02X bytes=[%s]",
                        desk_reject_reason_name((desk_reject_reason_t)i), sample->offset,
                        (unsigned)sample->decision_len, (unsigned)sample->len,
                        sample->closed ? "yes" : "no", (unsigned)sample->state_flags, bytes);
                    if (sample->bytes[0] == 0xAA && sample->len >= 7) {
                        const uint8_t *a = sample->bytes;
                        unsigned sum = desk_aa_checksum(a);
                        unsigned received = ((unsigned)a[5] << 8) | a[6];
                        ESP_LOGI(TAG, "REJECT_AA header_byte=0x%02X expected_header=0x09 checksum_seen=0x%04X calculated=0x%04X",
                            (unsigned)a[1], received, sum);
                    }
                    printed_prefix[i] = true;
                    if (sample->closed) printed_complete[i] = true;
                }
                if (r.parser.invalid_state) {
                    const uint8_t *b = r.parser.last_rejected_state;
                    ESP_LOGI(TAG, "STATE_REJECT pairs=%lu scan_policy_unknown=%lu height_policy=%lu last=[%02X %02X %02X %02X %02X %02X]",
                        (unsigned long)r.parser.state_pair_errors, (unsigned long)r.parser.state_scan_errors,
                        (unsigned long)r.parser.state_height_errors, (unsigned)b[0], (unsigned)b[1],
                        (unsigned)b[2], (unsigned)b[3], (unsigned)b[4], (unsigned)b[5]);
                }
                ESP_LOGI(TAG, "UART bytes=%lu fifo_overflow=%lu ring_full=%lu frame=%lu parity=%lu break=%lu"
                    " read_errors=%lu rx_peak=%u/%u event_peak=%u/%u event_queue_full_polls=%lu"
                    " recovery_flushes=%lu discarded=%lu read_gap_max_us=%" PRId64
                    " log_dropped=%lu log_peak=%u/%u log_max_us=%" PRId64,
                    (unsigned long)r.transport.bytes, (unsigned long)r.transport.fifo_overflow,
                    (unsigned long)r.transport.ring_full, (unsigned long)r.transport.frame_errors,
                    (unsigned long)r.transport.parity_errors, (unsigned long)r.transport.breaks,
                    (unsigned long)r.transport.read_errors, (unsigned)r.transport.rx_peak, (unsigned)RX_RING_SIZE,
                    (unsigned)r.transport.event_peak, (unsigned)UART_EVENT_QUEUE_SIZE,
                    (unsigned long)r.transport.event_queue_full_polls,
                    (unsigned long)r.transport.recovery_flushes, (unsigned long)r.transport.recovery_discarded_bytes,
                    r.transport.read_gap_max_us,
                    (unsigned long)r.transport.log_dropped, (unsigned)r.transport.log_peak, (unsigned)LOG_QUEUE_SIZE, log_max_us);
            }
        }
        int64_t elapsed = esp_timer_get_time() - start;
        if (elapsed > log_max_us) log_max_us = elapsed;
    }
}

static void handle_rx(const desk_event_t *ev) {
    desk_button_uart_rx();
    int64_t now = esp_timer_get_time();
    desk_tracking_t prev = desk;
    desk_tracking_feed(&desk, ev, now);
    desk_diagnostics_feed(&diagnostics, ev, now, queue_diagnostic, NULL);
    log_record_t r = {.kind = LOG_CHANGE, .state = desk,
        .command_changed = !prev.known || prev.ctrl_command != desk.ctrl_command,
        .display_changed = !prev.known || prev.display_height != desk.display_height ||
                           prev.current_held != desk.current_held,
        .motion_changed = prev.moving != desk.moving || prev.motion_direction != desk.motion_direction};
    if (r.command_changed || r.display_changed || r.motion_changed) enqueue_log(&r);
}

static void transport_discontinuity(void) {
    desk_button_stop(DESK_RELEASE_UART_ERROR);
    desk_parser_discard_partial(&incoming_from_controller);
    desk_tracking_discontinuity(&desk);
    diagnostics.count = diagnostics.next = diagnostics.following = 0;
}

static bool poll_uart_events(void) {
    UBaseType_t used = uxQueueMessagesWaiting(uart_events);
    if (used > transport.event_peak) transport.event_peak = used;
    if (used == UART_EVENT_QUEUE_SIZE) ++transport.event_queue_full_polls;
    uart_event_t event;
    bool lost = false;
    while (xQueueReceive(uart_events, &event, 0) == pdTRUE) {
        switch (event.type) {
            case UART_FIFO_OVF: ++transport.fifo_overflow; lost = true; break;
            case UART_BUFFER_FULL: ++transport.ring_full; lost = true; break;
            case UART_FRAME_ERR: ++transport.frame_errors; lost = true; break;
            case UART_PARITY_ERR: ++transport.parity_errors; lost = true; break;
            case UART_BREAK: ++transport.breaks; lost = true; break;
            default: break; // DATA bytes are read directly from the driver ring.
        }
    }
    if (lost) {
        // Discard bytes across an unknown gap rather than combine old/new fragments.
        size_t discarded = 0;
        ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_1, &discarded));
        ++transport.recovery_flushes;
        transport.recovery_discarded_bytes += (uint32_t)discarded;
        ESP_ERROR_CHECK(uart_flush_input(UART_NUM_1));
        transport_discontinuity();
    }
    return lost;
}

static void init_uart_rx_only(void) {
    const uart_config_t settings = {
        .baud_rate = DESK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &settings));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, DESK_BOARD_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, RX_RING_SIZE, 0,
                                        UART_EVENT_QUEUE_SIZE, &uart_events, 0));
}

void app_main(void) {
    ESP_ERROR_CHECK(desk_button_init());
    uart_owner = xTaskGetCurrentTaskHandle();
    desk_parser_reset(&incoming_from_controller);
    log_queue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(log_record_t));
    ESP_ERROR_CHECK(log_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(log_task, "desk_log", 6144, NULL, 1, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "PASSIVE SNIFFER: 9600 8N1, desk RX -> UART1 GPIO%d", DESK_BOARD_RX_GPIO);
    ESP_LOGW(TAG, "UART listen-only. GPIO5 UP emulation ready, released. Commands: up 50..5000, stop, status");
    init_uart_rx_only();
    ESP_ERROR_CHECK(desk_button_console_start(request_status));
    uint8_t rx_buf[256];
    desk_event_t ev;
    int64_t last_report = esp_timer_get_time(), last_read = last_report;
    while (true) {
        int64_t now = esp_timer_get_time();
        if (now - last_read > transport.read_gap_max_us) transport.read_gap_max_us = now - last_read;
        last_read = now;
        poll_uart_events();
        size_t buffered = 0;
        ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_1, &buffered));
        if (buffered > transport.rx_peak) transport.rx_peak = buffered;
        int n = uart_read_bytes(UART_NUM_1, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(10));
        if (n < 0) { ++transport.read_errors; transport_discontinuity(); }
        else transport.bytes += (uint32_t)n;
        if (poll_uart_events() && n > 0) {
            transport.recovery_discarded_bytes += (uint32_t)n;
            n = 0;
        }
        for (int i = 0; i < n; ++i) {
            if (desk_feed_rx(&incoming_from_controller, rx_buf[i], &ev)) handle_rx(&ev);
        }
        now = esp_timer_get_time();
        bool was_moving = desk.moving;
        desk_tracking_tick(&desk, now);
        if (was_moving != desk.moving) {
            log_record_t r = {.kind = LOG_CHANGE, .state = desk, .motion_changed = true};
            enqueue_log(&r);
        }
        if (ulTaskNotifyTake(pdTRUE, 0) || now - last_report >= 5000000) {
            log_record_t r = {.kind = LOG_STATUS, .state = desk, .parser = incoming_from_controller,
                              .transport = transport, .anomalies = diagnostics.anomalies};
            enqueue_log(&r);
            last_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
