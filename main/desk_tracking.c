#include "desk_tracking.h"
#include "desk_diagnostics.h"

static int magnitude(int value) { return value < 0 ? -value : value; }

void desk_tracking_discontinuity(desk_tracking_t *s) {
    s->known = false;
    s->current_known = false;
    s->current_held = false;
    s->candidate_known = false;
    s->moving = false;
    s->motion_direction = 0;
}

void desk_tracking_tick(desk_tracking_t *s, int64_t now) {
    if (!s->known) return;
    if (now - s->last_rx_us > DESK_TRACK_GAP_US) {
        desk_tracking_discontinuity(s);
    } else if (now - s->last_motion_us >= DESK_MOTION_IDLE_US) {
        s->moving = false;
        s->motion_direction = 0;
    }
}

void desk_tracking_feed(desk_tracking_t *s, const desk_event_t *ev, int64_t now) {
    if (ev->type != DESK_EVENT_RX_STATE) return;
    if (s->known && (now < s->last_rx_us || now - s->last_rx_us > DESK_TRACK_GAP_US))
        desk_tracking_discontinuity(s);
    const bool first = !s->known;
    if (first) {
        s->motion_anchor = ev->position_raw;
        s->last_motion_us = now;
    }
    const int delta = (int)ev->position_raw - s->motion_anchor;
    if (!first && magnitude(delta) >= DESK_MOTION_DELTA_RAW) {
        s->moving = true;
        s->motion_direction = delta > 0 ? 1 : -1;
        s->last_motion_us = now;
        s->motion_anchor = ev->position_raw;
    }
    if (!first && s->display_height != ev->height_cm) ++s->display_changes;

    const bool mismatch = s->current_known &&
        magnitude((int)ev->height_cm - s->current_height) >= DESK_DIAG_HEIGHT_JUMP &&
        magnitude((int)ev->position_raw - s->current_anchor) <= DESK_DIAG_RAW_LIMIT;
    if (mismatch && !s->current_held) ++s->display_transients;
    if (mismatch) s->current_held = true;
    if (ev->button == 0x04 && (mismatch || !s->current_known)) {
        s->candidate_known = true;
        s->preset_target_candidate = ev->height_cm;
    }
    // No raw-to-cm conversion. Resume only near the previous plausible height;
    // otherwise keep the last physical value explicitly marked as held.
    const bool unanchored_candidate = !s->current_known && s->candidate_known &&
        (ev->button == 0x04 ||
         magnitude((int)ev->height_cm - s->preset_target_candidate) < DESK_DIAG_HEIGHT_JUMP);
    if (!s->current_held && !unanchored_candidate) {
        s->current_height = ev->height_cm;
        s->current_known = true;
        s->current_anchor = ev->position_raw;
    } else if (s->current_known &&
               magnitude((int)ev->height_cm - s->current_height) <= 2) {
        s->current_held = false;
        s->current_height = ev->height_cm;
        s->current_anchor = ev->position_raw;
    }
    s->display_height = ev->height_cm;
    s->position_raw = ev->position_raw;
    s->ctrl_command = ev->button;
    s->known = true;
    s->last_rx_us = now;
    desk_tracking_tick(s, now);
}
