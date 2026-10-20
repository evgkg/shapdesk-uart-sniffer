#include "desk_button.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !CONFIG_GPTIMER_ISR_CACHE_SAFE || !CONFIG_GPIO_CTRL_FUNC_IN_IRAM
#error "Button deadline guard requires cache-safe GPTimer and IRAM GPIO control"
#endif

typedef enum { BUTTON_BEGIN, BUTTON_END, BUTTON_INVALID, BUTTON_BUSY, BUTTON_STOP_IDLE } event_kind_t;
typedef struct { event_kind_t kind; desk_release_reason_t reason; unsigned ms; int64_t time_us; } button_event_t;
static DRAM_ATTR portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR desk_button_policy_t policy;
static DRAM_ATTR QueueHandle_t events;
static DRAM_ATTR uint32_t dropped;
static gptimer_handle_t guard_timer;
static void (*status_callback)(void);

static void event_task(event_kind_t kind, desk_release_reason_t reason, unsigned ms) {
    button_event_t event = {.kind = kind, .reason = reason, .ms = ms, .time_us = esp_timer_get_time()};
    if (xQueueSend(events, &event, 0) != pdTRUE) ++dropped; // caller holds lock
}

static void release_locked(desk_release_reason_t reason) {
    // Clear the electrical output BEFORE any queue/log operation.
    ESP_ERROR_CHECK(gpio_set_level(DESK_UP_GPIO, 0));
    if (policy.active) {
        policy.active = false;
        event_task(BUTTON_END, reason, 0);
    }
}

static bool IRAM_ATTR guard_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *data, void *ctx) {
    (void)timer; (void)data; (void)ctx;
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&lock);
    int64_t now = esp_timer_get_time();
    desk_release_reason_t reason = desk_button_policy_tick(&policy, now);
    if (reason != DESK_RELEASE_NONE) {
        gpio_set_level(DESK_UP_GPIO, 0);
        policy.active = false;
        button_event_t event = {.kind = BUTTON_END, .reason = reason, .time_us = now};
        if (xQueueSendFromISR(events, &event, &wake) != pdTRUE) ++dropped;
    }
    portEXIT_CRITICAL_ISR(&lock);
    return wake == pdTRUE;
}

void desk_button_stop(desk_release_reason_t reason) {
    if (reason == DESK_RELEASE_UART_ERROR && !DESK_UP_REQUIRE_UART_FEEDBACK) return;
    portENTER_CRITICAL(&lock);
    release_locked(reason);
    portEXIT_CRITICAL(&lock);
}

static void shutdown_release(void) { desk_button_stop(DESK_RELEASE_SHUTDOWN); }

esp_err_t desk_button_init(void) {
    // Load LOW into the output latch before enabling output. The external 100k
    // base pulldown holds the transistor off before app_main and during reset.
    ESP_RETURN_ON_ERROR(gpio_set_level(DESK_UP_GPIO, 0), "desk_button", "LOW latch");
    const gpio_config_t gpio = {.pin_bit_mask = 1ULL << DESK_UP_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&gpio), "desk_button", "GPIO5 init");
    ESP_RETURN_ON_ERROR(gpio_set_level(DESK_UP_GPIO, 0), "desk_button", "LOW output");
    events = xQueueCreate(16, sizeof(button_event_t));
    if (!events) return ESP_ERR_NO_MEM;
    const gptimer_config_t config = {.clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000, .intr_priority = 3};
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&config, &guard_timer), "desk_button", "guard timer");
    const gptimer_event_callbacks_t callbacks = {.on_alarm = guard_alarm};
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(guard_timer, &callbacks, NULL), "desk_button", "guard ISR");
    const gptimer_alarm_config_t alarm = {.alarm_count = 1000, .reload_count = 0,
        .flags.auto_reload_on_alarm = true};
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(guard_timer, &alarm), "desk_button", "guard alarm");
    ESP_RETURN_ON_ERROR(gptimer_enable(guard_timer), "desk_button", "guard enable");
    ESP_RETURN_ON_ERROR(gptimer_start(guard_timer), "desk_button", "guard start");
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(shutdown_release), "desk_button", "shutdown release");
    ESP_LOGW("desk_button", "UART feedback interlock: %s; maximum press=%u ms",
        DESK_UP_REQUIRE_UART_FEEDBACK ? "enabled" : "disabled (button-only test)", (unsigned)DESK_UP_MAX_MS);
    return ESP_OK;
}

void desk_button_uart_rx(void) {
    portENTER_CRITICAL(&lock);
    policy.last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&lock);
}

desk_button_status_t desk_button_status(void) {
    portENTER_CRITICAL(&lock);
    int64_t left = policy.active ? policy.deadline_us - esp_timer_get_time() : 0;
    desk_button_status_t result = {.active = policy.active, .gpio_level = gpio_get_level(DESK_UP_GPIO),
        .remaining_ms = left > 0 ? (unsigned)((left + 999) / 1000) : 0, .log_dropped = dropped};
    portEXIT_CRITICAL(&lock);
    return result;
}

void desk_button_log_events(void) {
    static const char *const reasons[] = {"none", "timeout", "stop", "RX loss", "UART error", "USB loss", "shutdown"};
    button_event_t event;
    // Bound each logger pass, even when command input is flooded.
    for (unsigned i = 0; i < 16 && xQueueReceive(events, &event, 0) == pdTRUE; ++i) {
        switch (event.kind) {
            case BUTTON_BEGIN: ESP_LOGI("desk_button", "UP BEGIN GPIO5=HIGH duration=%u ms at_us=%lld", event.ms, (long long)event.time_us); break;
            case BUTTON_END: ESP_LOGI("desk_button", "UP END GPIO5=LOW reason=%s at_us=%lld", reasons[event.reason], (long long)event.time_us); break;
            case BUTTON_BUSY: ESP_LOGW("desk_button", "up rejected: a press is already active; deadline unchanged"); break;
            case BUTTON_INVALID: ESP_LOGW("desk_button", "invalid command; use up 50..5000, stop, status"); break;
            case BUTTON_STOP_IDLE: ESP_LOGI("desk_button", "stop: GPIO5=LOW (already released)"); break;
        }
    }
}

static void console_task(void *ctx) {
    (void)ctx;
    desk_command_stream_t stream = {0};
    uint8_t bytes[64];
    while (true) {
        if (!usb_serial_jtag_is_connected()) {
            desk_button_stop(DESK_RELEASE_USB_LOSS);
            stream = (desk_command_stream_t){0};
        }
        int n = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(10));
        if (n < 0) { desk_button_stop(DESK_RELEASE_USB_LOSS); stream = (desk_command_stream_t){0}; }
        if (!usb_serial_jtag_is_connected()) { vTaskDelay(1); continue; }
        for (int i = 0; i < n; ++i) {
            desk_command_t cmd = desk_command_feed(&stream, bytes[i]);
            if (cmd.kind == DESK_CMD_NONE) continue;
            if (cmd.kind == DESK_CMD_STATUS) { status_callback(); continue; }
            portENTER_CRITICAL(&lock);
            if (cmd.kind == DESK_CMD_STOP) {
                bool was_active = policy.active;
                release_locked(DESK_RELEASE_STOP);
                if (!was_active) event_task(BUTTON_STOP_IDLE, DESK_RELEASE_NONE, 0);
            } else if (cmd.kind == DESK_CMD_UP) {
                if (desk_button_policy_start(&policy, cmd.duration_ms, esp_timer_get_time())) {
                    ESP_ERROR_CHECK(gpio_set_level(DESK_UP_GPIO, 1));
                    event_task(BUTTON_BEGIN, DESK_RELEASE_NONE, cmd.duration_ms);
                } else event_task(BUTTON_BUSY, DESK_RELEASE_NONE, 0);
            } else event_task(BUTTON_INVALID, DESK_RELEASE_NONE, 0);
            portEXIT_CRITICAL(&lock);
        }
        vTaskDelay(1); // a continuous USB command stream must not starve UART RX
    }
}

esp_err_t desk_button_console_start(void (*request_status)(void)) {
    status_callback = request_status;
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 256, .tx_buffer_size = 256};
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) return err;
    return xTaskCreate(console_task, "desk_commands", 4096, NULL, 1, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
