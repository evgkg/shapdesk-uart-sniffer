#pragma once

#include "desk_protocol.h"

#define DESK_MOTION_DELTA_RAW 8
#define DESK_MOTION_IDLE_US 500000
#define DESK_TRACK_GAP_US 1000000

typedef struct {
    bool known;
    uint8_t display_height;
    bool current_known;
    uint8_t current_height; // last plausible physical height; see current_held
    bool current_held;
    bool candidate_known;
    uint8_t preset_target_candidate; // observed display value, not a motor target
    bool moving;
    int motion_direction; // sign of raw change, not a decoded command
    uint16_t position_raw;
    uint8_t ctrl_command;
    uint16_t motion_anchor;
    uint16_t current_anchor;
    int64_t last_rx_us;
    int64_t last_motion_us;
    uint32_t display_changes;
    uint32_t display_transients;
} desk_tracking_t;

// Only call for validated RX_STATE events. Preset semantics currently cover 0x04.
void desk_tracking_feed(desk_tracking_t *state, const desk_event_t *event, int64_t now);
void desk_tracking_tick(desk_tracking_t *state, int64_t now);
// Transport loss invalidates physical certainty; next valid sample reanchors raw.
void desk_tracking_discontinuity(desk_tracking_t *state);
