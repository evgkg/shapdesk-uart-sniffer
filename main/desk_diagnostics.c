#include "desk_diagnostics.h"

static int magnitude(int value) {
    return value < 0 ? -value : value;
}

bool desk_diagnostics_feed(desk_diagnostics_t *diag, const desk_event_t *event,
                           int64_t time_us, desk_diag_output_t output, void *ctx) {
    if (event->type != DESK_EVENT_RX_STATE) return false;
    bool anomaly = false;
    if (diag->count) {
        const desk_diag_sample_t *prev =
            &diag->history[(diag->next + DESK_DIAG_HISTORY - 1) % DESK_DIAG_HISTORY];
        const int64_t gap = time_us - prev->time_us;
        if (gap < 0 || gap > DESK_DIAG_MAX_GAP_US) {
            // Do not compare disconnected observation windows.
            diag->count = 0;
            diag->next = 0;
            diag->following = 0;
        } else {
            anomaly = magnitude((int)event->height_cm - prev->event.height_cm) >=
                          DESK_DIAG_HEIGHT_JUMP &&
                      magnitude((int)event->position_raw - prev->event.position_raw) <=
                          DESK_DIAG_RAW_LIMIT;
        }
    }
    const desk_diag_sample_t sample = {
        .event = *event, .time_us = time_us, .sequence = ++diag->sequence,
    };
    if (anomaly) {
        ++diag->anomalies;
        // Overlapping anomalies share one window; avoid repeating the history.
        if (!diag->following && output) {
            for (unsigned i = 0; i < diag->count; ++i) {
                unsigned index = (diag->next + DESK_DIAG_HISTORY - diag->count + i)
                                 % DESK_DIAG_HISTORY;
                output(DESK_DIAG_BEFORE, &diag->history[index], ctx);
            }
        }
        if (output) output(DESK_DIAG_ANOMALY, &sample, ctx);
        diag->following = DESK_DIAG_FOLLOWING;
    } else if (diag->following) {
        if (output) output(DESK_DIAG_AFTER, &sample, ctx);
        --diag->following;
    }
    diag->history[diag->next] = sample;
    diag->next = (diag->next + 1) % DESK_DIAG_HISTORY;
    if (diag->count < DESK_DIAG_HISTORY) ++diag->count;
    return anomaly;
}
