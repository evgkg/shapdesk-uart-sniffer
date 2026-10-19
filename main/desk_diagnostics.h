#pragma once

#include "desk_protocol.h"

// Diagnostic heuristics, not a position-to-height conversion or protocol rules.
#define DESK_DIAG_HISTORY 4
#define DESK_DIAG_FOLLOWING 4
#define DESK_DIAG_HEIGHT_JUMP 5
#define DESK_DIAG_RAW_LIMIT 32
#define DESK_DIAG_MAX_GAP_US 1000000

typedef struct {
    desk_event_t event;
    int64_t time_us;  // software receive time; not a wire timestamp
    uint32_t sequence;
} desk_diag_sample_t;

typedef enum {
    DESK_DIAG_BEFORE,
    DESK_DIAG_ANOMALY,
    DESK_DIAG_AFTER,
} desk_diag_kind_t;

typedef void (*desk_diag_output_t)(desk_diag_kind_t kind,
                                   const desk_diag_sample_t *sample, void *ctx);

typedef struct {
    desk_diag_sample_t history[DESK_DIAG_HISTORY];
    unsigned next;
    unsigned count;
    unsigned following;
    uint32_t sequence;
    uint32_t anomalies;
} desk_diagnostics_t;

// Zero initialization is sufficient. Only validated RX_STATE events are accepted.
// Returns true on a suspected mismatch; never changes or suppresses decoded data.
bool desk_diagnostics_feed(desk_diagnostics_t *diag, const desk_event_t *event,
                           int64_t time_us, desk_diag_output_t output, void *ctx);
