/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

#include "main.h"
#include <math.h>
#include <limits.h>

#define MACOS_SWITCH_MOVE_X 10
#define MACOS_SWITCH_MOVE_COUNT 5
#define ACCEL_POINTS 7
#define MOUSE_CRITICAL_QUEUE_TIMEOUT_US 100000

uint16_t get_jump_threshold(output_t *output, enum screen_pos_e direction) {
    const uint16_t NO_JUMP_THRESHOLD = 0;

    /* If on non-main local screen, every possible switch is local */
    if (output->screen_index > 1)
        return NO_JUMP_THRESHOLD;

    /* If on main screen but going away from the border, switch is local */
    if (output->pos == direction && output->screen_index == 1)
        return NO_JUMP_THRESHOLD;

    /* ... in all other cases, switch is non-local (jump to another pc) */
    return global_state.config.jump_threshold;
}

/* Check if our upcoming mouse movement would result in having to switch outputs */
enum screen_pos_e is_screen_switch_needed(output_t *output, int position, int offset) {
    enum screen_pos_e direction = (offset < 0) ? LEFT : RIGHT;

    /* No position offset implies no switch needed. */
    if (offset == 0)
        return NONE;

    /* Local switches (virtual desktop changes) have no gap, only cross-output jumps use threshold */
    uint16_t threshold = get_jump_threshold(output, direction);

    if ((int64_t)position + offset < MIN_SCREEN_COORD - threshold)
        return LEFT;

    if ((int64_t)position + offset > MAX_SCREEN_COORD + threshold)
        return RIGHT;

    return NONE;
}

/* Move mouse coordinate 'position' by 'offset', but don't fall off the screen */
int32_t move_and_keep_on_screen(int position, int offset) {
    /* Lowest we can go is 0 */
    if ((int64_t)position + offset < MIN_SCREEN_COORD)
        return MIN_SCREEN_COORD;

    /* Highest we can go is MAX_SCREEN_COORD */
    else if ((int64_t)position + offset > MAX_SCREEN_COORD)
        return MAX_SCREEN_COORD;

    /* We're still on screen, all good */
    return position + offset;
}

/* Implement basic mouse acceleration based on actual 2D movement magnitude.
   Returns the acceleration factor to apply to both x and y components. */
float calculate_mouse_acceleration_factor(int32_t offset_x, int32_t offset_y) {
    const struct curve {
        int value;
        float factor;
    } acceleration[ACCEL_POINTS] = {
                   // 4 |                                        *
        {2, 1},    //   |                                  *
        {5, 1.1},  // 3 |
        {15, 1.4}, //   |                       *
        {30, 1.9}, // 2 |                *
        {45, 2.6}, //   |        *
        {60, 3.4}, // 1 |  *
        {70, 4.0}, //    -------------------------------------------
    };             //        10    20    30    40    50    60    70

    if (offset_x == 0 && offset_y == 0)
        return 1.0;

    if (!global_state.config.enable_acceleration)
        return 1.0;

    // Calculate the 2D movement magnitude
    const float movement_magnitude = sqrtf((float)offset_x * offset_x + (float)offset_y * offset_y);

    if (movement_magnitude <= acceleration[0].value)
        return acceleration[0].factor;

    if (movement_magnitude >= acceleration[ACCEL_POINTS-1].value)
        return acceleration[ACCEL_POINTS-1].factor;

    const struct curve *lower = NULL;
    const struct curve *upper = NULL;

    for (int i = 0; i < ACCEL_POINTS-1; i++) {
        if (movement_magnitude < acceleration[i + 1].value) {
            lower = &acceleration[i];
            upper = &acceleration[i + 1];
            break;
        }
    }

    // Should never happen, but just in case
    if (lower == NULL || upper == NULL)
        return 1.0;

    const float interpolation_pos = (movement_magnitude - lower->value) /
                                  (upper->value - lower->value);

    return lower->factor + interpolation_pos * (upper->factor - lower->factor);
}

/* Keep descriptor/config extremes from overflowing float-to-int conversion.
   Ordinary mouse deltas retain their existing rounding and acceleration. */
static int scaled_mouse_offset(int32_t delta, float factor, int32_t speed) {
    double scaled = round(delta * factor * speed);
    if (scaled >= INT_MAX)
        return INT_MAX;
    if (scaled <= INT_MIN)
        return INT_MIN;
    return (int)scaled;
}

static int32_t canonical_mouse_axis(int32_t value) {
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return value;
}

/* Returns LEFT if need to jump left, RIGHT if right, NONE otherwise */
enum screen_pos_e update_mouse_position(device_t *state, mouse_values_t *values) {
    output_t *current    = &state->config.output[state->active_output];
    uint8_t reduce_speed = 0;

    /* Check if we are configured to move slowly */
    if (state->mouse_zoom)
        reduce_speed = MOUSE_ZOOM_SCALING_FACTOR;

    /* Calculate movement */
    float acceleration_factor = calculate_mouse_acceleration_factor(values->move_x, values->move_y);
    int offset_x = scaled_mouse_offset(values->move_x, acceleration_factor, current->speed_x >> reduce_speed);
    int offset_y = scaled_mouse_offset(values->move_y, acceleration_factor, current->speed_y >> reduce_speed);

    /* Determine if our upcoming movement would stay within the screen */
    enum screen_pos_e switch_direction = is_screen_switch_needed(current, state->pointer_x, offset_x);

    /* Update movement */
    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);

    /* Update buttons state */
    state->mouse_buttons = combined_mouse_buttons(state);

    return switch_direction;
}

/* If we are active output, queue packet to mouse queue, else send them through UART */
void output_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_mouse_report(report, state);
    } else {
        enum packet_type_e type = state->peer_mouse_state_known
            ? MOUSE_SYNTHETIC_REPORT_MSG : MOUSE_REPORT_MSG;
        queue_packet((uint8_t *)report, type, MOUSE_REPORT_LENGTH);
    }
}

uint8_t combined_mouse_buttons(const device_t *state) {
    return state->local_mouse_buttons | state->peer_mouse_buttons;
}

static void refresh_local_mouse_buttons(hid_interface_t *changed, device_t *state) {
    uint8_t buttons = changed ? changed->mouse_buttons : 0;
    for (unsigned dev = 0; dev < MAX_DEVICES; dev++)
        for (unsigned itf = 0; itf < MAX_INTERFACES; itf++)
            buttons |= state->iface[dev][itf].mouse_buttons;
    state->local_mouse_buttons = buttons;
    state->mouse_buttons = combined_mouse_buttons(state);
}

static void publish_mouse_buttons(device_t *state, bool detached) {
    uint8_t data[] = {state->local_mouse_buttons, detached};
    if (detached)
        queue_packet_blocking(data, MOUSE_BUTTONS_SYNC_MSG, sizeof(data));
    else
        queue_packet(data, MOUSE_BUTTONS_SYNC_MSG, sizeof(data));
}

/* Also advertises source-report support to a peer. Old firmware ignores this
 * new message and continues receiving the original physical report format. */
void sync_mouse_buttons(device_t *state) {
    publish_mouse_buttons(state, false);
}

void mouse_interface_removed(hid_interface_t *iface, device_t *state) {
    uint8_t before = state->local_mouse_buttons;
    iface->mouse_buttons = 0;
    refresh_local_mouse_buttons(NULL, state);
    if (before == state->local_mouse_buttons)
        return;
    publish_mouse_buttons(state, true);
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        bool relative = mouse_uses_relative_mode(state);
        mouse_report_t report = {.buttons = combined_mouse_buttons(state),
            .x = relative ? 0 : state->pointer_x, .y = relative ? 0 : state->pointer_y,
            .mode = relative ? RELATIVE : ABSOLUTE};
        queue_mouse_report_critical(&report, state);
    }
}

bool queue_mouse_report_critical(mouse_report_t *report, device_t *state) {
    uint64_t started = time_us_64();
    while (state->tud_connected) {
        if (queue_try_add(&state->mouse_queue, report))
            return true;
        if (time_us_64() - started >= MOUSE_CRITICAL_QUEUE_TIMEOUT_US) {
            state->reboot_requested = true;
            return false;
        }
        tight_loop_contents();
    }
    return false;
}

/* A focus change ends a drag on both HID interfaces of the old host. Physical
 * source masks remain authoritative for subsequent input on the new host. */
void release_mouse_host_buttons(device_t *state) {
    if (!combined_mouse_buttons(state))
        return;
    mouse_report_t absolute = {.x = state->pointer_x, .y = state->pointer_y, .mode = ABSOLUTE};
    mouse_report_t relative = {.mode = RELATIVE};
    if (queue_mouse_report_critical(&absolute, state))
        queue_mouse_report_critical(&relative, state);
}

/* Physical reports carry only this Pico's source state across UART. Synthetic
 * parking/desktop reports retain the original exact-output message semantics. */
static void output_source_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        report->buttons = combined_mouse_buttons(state);
        queue_mouse_report(report, state);
    } else {
        enum packet_type_e type = state->peer_mouse_state_known
            ? MOUSE_SOURCE_REPORT_MSG : MOUSE_REPORT_MSG;
        queue_packet((uint8_t *)report, type, MOUSE_REPORT_LENGTH);
    }
}

/* Calculate and return Y coordinate when moving from screen out_from to screen out_to */
int16_t scale_y_coordinate(int screen_from, int screen_to, device_t *state) {
    output_t *from = &state->config.output[screen_from];
    output_t *to   = &state->config.output[screen_to];

    int size_to   = to->border.bottom - to->border.top;
    int size_from = from->border.bottom - from->border.top;

    /* If sizes match, there is nothing to do */
    if (size_from == size_to)
        return state->pointer_y;

    /* Moving from smaller ==> bigger screen
       y_a = top + (((bottom - top) * y_b) / HEIGHT) */

    if (size_from > size_to) {
        return to->border.top + ((size_to * state->pointer_y) / MAX_SCREEN_COORD);
    }

    /* Moving from bigger ==> smaller screen
       y_b = ((y_a - top) * HEIGHT) / (bottom - top) */

    if (state->pointer_y < from->border.top)
        return MIN_SCREEN_COORD;

    if (state->pointer_y > from->border.bottom)
        return MAX_SCREEN_COORD;

    return ((state->pointer_y - from->border.top) * MAX_SCREEN_COORD) / size_from;
}

void switch_to_another_pc(
    device_t *state, output_t *output, int output_to, int direction) {
    uint8_t *mouse_park_pos = &state->config.output[state->active_output].mouse_park_pos;

    int16_t mouse_y = (*mouse_park_pos == 0) ? MIN_SCREEN_COORD : /* Top */
                      (*mouse_park_pos == 1) ? MAX_SCREEN_COORD : /* Bottom */
                                               state->pointer_y;  /* Previous */

    mouse_report_t hidden_pointer = {.y = mouse_y, .x = MAX_SCREEN_COORD};

    output_mouse_report(&hidden_pointer, state);
    set_active_output(state, output_to);
    state->pointer_x = (direction == LEFT) ? MAX_SCREEN_COORD : MIN_SCREEN_COORD;
    state->pointer_y = scale_y_coordinate(output->number, 1 - output->number, state);

    /* Tell the other board where the cursor actually ended up. There is only one
       cursor but each board tracks it separately, and a pointing device may well be
       attached to the other board (e.g. a keyboard with an integrated trackball).
       This also overwrites the parking coordinates the hidden_pointer report above
       just left there, which would otherwise be picked up as a real cursor position. */
    sync_pointer_position(state);
}

/* Send our current cursor position to the other board so both agree where it is */
void sync_pointer_position(device_t *state) {
    uart_packet_t packet = {
        .type = POINTER_SYNC_MSG,
        .data16 = {
            [0] = (uint16_t)state->pointer_x,
            [1] = (uint16_t)state->pointer_y,
        },
    };

    queue_try_add(&state->uart_tx_queue, &packet);
}

void switch_virtual_desktop_macos(device_t *state, int direction) {
    /*
     * Fix for MACOS: Before sending new absolute report setting X to 0:
     * 1. Move the cursor to the edge of the screen directly in the middle to handle screens
     *    of different heights
     * 2. Send relative mouse movement one or two pixels in the direction of movement to get
     *    the cursor onto the next screen
     */
    mouse_report_t edge_position = {
        .x = (direction == LEFT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD,
        .y = MAX_SCREEN_COORD / 2,
        .mode = ABSOLUTE,
        .buttons = state->mouse_buttons,
    };

    uint16_t move = (direction == LEFT) ? -MACOS_SWITCH_MOVE_X : MACOS_SWITCH_MOVE_X;
    mouse_report_t move_relative_one = {
        .x = move,
        .mode = RELATIVE,
        /* Force buttons to 0 for relative movement to avoid duplicating the button 
           press state, which would leave the relative HID mouse permanently stuck 
           down if the user is dragging an item while switching desktops. */
        .buttons = 0,
    };

    output_mouse_report(&edge_position, state);

    /* Once doesn't seem reliable enough, do it a few times */
    for (int i = 0; i < MACOS_SWITCH_MOVE_COUNT; i++)
        output_mouse_report(&move_relative_one, state);
}

void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    switch (output->os) {
        case MACOS:
            switch_virtual_desktop_macos(state, direction);
            break;

        case WINDOWS:
            /* TODO: Switch to relative-only if index > 1, but keep tabs to switch back */
            state->relative_mouse = (new_index > 1);
            break;

        case LINUX:
        case ANDROID:
        case OTHER:
            /* Linux should treat all desktops as a single virtual screen, so you should leave
            screen_count at 1 and it should just work */
            break;
    }

    state->pointer_x       = (direction == RIGHT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
    config_lock();
    output->screen_index = new_index;
    config_unlock();
}

/*                               BORDER
                                   |
       .---------.    .---------.  |  .---------.    .---------.    .---------.
      ||    B2   ||  ||    B1   || | ||    A1   ||  ||    A2   ||  ||    A3   ||   (output, index)
      ||  extra  ||  ||   main  || | ||   main  ||  ||  extra  ||  ||  extra  ||   (main or extra)
       '---------'    '---------'  |  '---------'    '---------'    '---------'
          )___(          )___(     |     )___(          )___(          )___(
*/
void do_screen_switch(device_t *state, int direction) {
    output_t *output = &state->config.output[state->active_output];

    /* Zoom assist uses relative reports so macOS can keep panning a zoomed
       viewport at the physical screen edge. Pointer-based switching must stay
       disabled until the user deliberately zooms all the way back out. */
    if (state->switch_lock || state->gaming_mode || zoom_assist_is_active(state))
        return;

    /* We want to jump in the direction of the other computer */
    if (output->pos != direction) {
        if (output->screen_index == 1) { /* We are at the border -> switch outputs */
            /* No switching allowed if mouse button is held. Should only apply to the border! */
            if (state->mouse_buttons)
                return;

            switch_to_another_pc(state, output, 1 - state->active_output, direction);
        }
        /* If here, this output has multiple desktops and we are not on the main one */
        else
            switch_virtual_desktop(state, output, output->screen_index - 1, direction);
    }

    /* We want to jump away from the other computer, only possible if there is another screen to jump to */
    else if (output->screen_index < output->screen_count)
        switch_virtual_desktop(state, output, output->screen_index + 1, direction);
}

static inline bool extract_value(bool uses_id, int32_t *dst, report_val_t *src, uint8_t *raw_report, int len) {
    /* If HID Report ID is used, the report is prefixed by the report ID so we have to move by 1 byte */
    if (uses_id) {
        if (len <= 0 || *raw_report++ != src->report_id)
            return false;
        len--;
    }

    *dst = get_report_value(raw_report, len, src);
    return true;
}

void extract_report_values(uint8_t *raw_report, int len, device_t *state, mouse_values_t *values, hid_interface_t *iface) {
    /* Interpret values depending on the current protocol used. */
    if (iface->protocol == HID_PROTOCOL_BOOT) {
        /* USB boot mouse guarantees only buttons, X and Y. Wheels are common
           extensions, but reading them from a three-byte transfer is invalid. */
        if (!raw_report || len < 3)
            return;
        values->buttons = raw_report[0];
        values->move_x  = (int8_t)raw_report[1];
        values->move_y  = (int8_t)raw_report[2];
        values->wheel   = len > 3 ? (int8_t)raw_report[3] : 0;
        values->pan     = len > 4 ? (int8_t)raw_report[4] : 0;
        return;
    }
    mouse_t *mouse = &iface->mouse;
    bool uses_id = iface->uses_report_id;

    extract_value(uses_id, &values->move_x, &mouse->move_x, raw_report, len);
    extract_value(uses_id, &values->move_y, &mouse->move_y, raw_report, len);
    extract_value(uses_id, &values->wheel, &mouse->wheel, raw_report, len);
    extract_value(uses_id, &values->pan, &mouse->pan, raw_report, len);

    if (!mouse->buttons.size || !extract_value(uses_id, &values->buttons, &mouse->buttons, raw_report, len)) {
        values->buttons = iface->mouse_buttons;
    }
}

mouse_report_t create_mouse_report(device_t *state, mouse_values_t *values) {
    mouse_report_t mouse_report = {
        .buttons = values->buttons,
        .x       = state->pointer_x,
        .y       = state->pointer_y,
        .wheel   = values->wheel,
        .pan     = values->pan,
        .mode    = ABSOLUTE,
    };

    /* Workaround for Windows multiple desktops, gaming mode, and inferred
       macOS zoom assist. */
    if (mouse_uses_relative_mode(state)) {
        mouse_report.x = values->move_x;
        mouse_report.y = values->move_y;
        mouse_report.mode = RELATIVE;
    }

    return mouse_report;
}

/* A partial transfer is neither a zero-motion report nor a button release.
   Validate all fields belonging to this ID before changing state/activity. */
static bool mouse_report_complete(const uint8_t *raw_report, int len, const hid_interface_t *iface) {
    if (!raw_report || len <= 0)
        return false;
    if (iface->protocol == HID_PROTOCOL_BOOT)
        return len >= 3;
    int payload_len = len - iface->uses_report_id;
    if (payload_len <= 0)
        return false;
    const mouse_t *mouse = &iface->mouse;
    const report_val_t *fields[] = {&mouse->buttons, &mouse->move_x, &mouse->move_y,
                                   &mouse->wheel, &mouse->pan};
    bool found = false;
    for (unsigned i = 0; i < ARRAY_SIZE(fields); i++) {
        const report_val_t *field = fields[i];
        if (!field->size || (iface->uses_report_id && field->report_id != raw_report[0]))
            continue;
        if (field->size > 32 || (uint32_t)field->offset + field->size > (uint64_t)payload_len * 8)
            return false;
        found = true;
    }
    return found;
}

void process_mouse_report(uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface) {
    mouse_values_t values = {0};
    device_t *state = &global_state;

    if (!mouse_report_complete(raw_report, len, iface))
        return;

    /* Interpret the mouse HID report, extract and save values we need. */
    extract_report_values(raw_report, len, state, &values, iface);

    /* The output protocol carries signed 16-bit motion; saturate wider HID
       fields once so local and forwarded relative reports cannot wrap. */
    values.move_x = canonical_mouse_axis(values.move_x);
    values.move_y = canonical_mouse_axis(values.move_y);

    /* DeskHop's device and UART mouse reports carry an int8 wheel. Clamp a
       wider source field once so local and forwarded inference see identical
       magnitudes and can never disagree about direction through truncation. */
    values.wheel = zoom_canonicalize_wheel(values.wheel);

    /* If nothing changed, don't send a report. This prevents composite keyboards
       (e.g. QMK) that expose a mouse HID interface from generating spurious
       absolute position reports when they send zero-movement mouse reports during
       keyboard events. */
    if (values.move_x == 0 && values.move_y == 0 &&
        values.wheel == 0 && values.pan == 0 &&
        values.buttons == iface->mouse_buttons) {
        return;
    }

    uint8_t previous_local = state->local_mouse_buttons;
    iface->mouse_buttons = (uint8_t)values.buttons;
    refresh_local_mouse_buttons(iface, state);
    values.buttons = state->local_mouse_buttons;
    /* An inactive source sends its state with the routed physical report.
     * The active source must mirror its own holds for future output switches. */
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT && previous_local != state->local_mouse_buttons)
        sync_mouse_buttons(state);

    record_local_activity(state, state->active_output);

    /* Command-scroll is macOS's built-in accessibility zoom gesture. Observe
       it before constructing this mouse report so the activating wheel event
       itself is emitted through the relative helper interface. */
    bool zoom_scroll = is_macos_zoom_scroll(state, values.wheel);
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        observe_zoom_scroll(state, values.wheel);
    else if (zoom_scroll)
        state->zoom_activation_pending[state->active_output] = true;

    bool position_changed = values.move_x != 0 || values.move_y != 0;

    /* Calculate and update mouse pointer movement. */
    enum screen_pos_e switch_direction = update_mouse_position(state, &values);

    if (!CURRENT_BOARD_IS_ACTIVE_OUTPUT && !position_changed) {
        /* Buttons and wheels do not carry a position. Let the active board attach
           its authoritative coordinates instead of forwarding our cached copy. */
        mouse_nonmotion_report_t report = {
            .buttons = values.buttons,
            .wheel   = values.wheel,
            .pan     = values.pan,
            .mode    = mouse_uses_relative_mode(state) ? RELATIVE : ABSOLUTE,
        };
        queue_packet((uint8_t *)&report, MOUSE_NONMOTION_MSG, sizeof(report));
    } else {
        /* Create the report for the output PC based on the updated values */
        mouse_report_t report = create_mouse_report(state, &values);

        /* A remote mouse may initiate zoom assist before the owner's state
           mirror makes the round trip. Preserve the raw deltas in that first
           combined Command-scroll report instead of forwarding absolute x/y. */
        if (zoom_scroll) {
            report.x = values.move_x;
            report.y = values.move_y;
            report.mode = RELATIVE;
        }

        /* Move the mouse, depending where the output is supposed to go */
        output_source_mouse_report(&report, state);
    }

    /* There is one cursor, but each board tracks its position separately. Absolute
       reports forwarded to the active board already carry our coordinates. Relative
       reports carry only deltas, so a separately attached keyboard/mouse button on
       that board would otherwise reuse stale absolute coordinates and make the cursor
       jump after zoom assist exits. Mirror the logical position in both cases. */
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT
        || (position_changed && (mouse_uses_relative_mode(state) || zoom_scroll)))
        sync_pointer_position(state);

    /* We use the mouse to switch outputs, if switch_direction is LEFT or RIGHT */
    /* On the first forwarded Command-scroll event, the active-output Pico has
       not mirrored the newly active state back yet. Suppress a coincident edge
       crossing locally so that first zoom gesture cannot hop computers. */
    if (switch_direction != NONE && !zoom_scroll)
        do_screen_switch(state, switch_direction);
}

/* ==================================================== *
 * Mouse Queue Section
 * ==================================================== */

void process_mouse_queue_task(device_t *state) {
    mouse_report_t report = {0};

    /* We need to be connected to the host to send messages */
    if (!state->tud_connected)
        return;

    /* Peek first, if there is anything there... */
    if (!queue_try_peek(&state->mouse_queue, &report))
        return;

    /* If we are suspended, let's wake the host up */
    if (tud_suspended())
        tud_remote_wakeup();

    /* Absolute and relative reports use separate HID interfaces. Check the
       interface this queued report will actually use. */
    uint8_t instance = report.mode == RELATIVE ? ITF_NUM_HID_REL_M : ITF_NUM_HID;
    if (!tud_hid_n_ready(instance))
        return;

    /* Try sending it to the host, if it's successful */
    bool succeeded
        = tud_mouse_report(report.mode, report.buttons, report.x, report.y, report.wheel, report.pan);

    /* ... then we can remove it from the queue */
    if (succeeded)
        queue_try_remove(&state->mouse_queue, &report);
}

void queue_mouse_report(mouse_report_t *report, device_t *state) {
    /* It wouldn't be fun to queue up a bunch of messages and then dump them all on host */
    if (!state->tud_connected)
        return;

    queue_try_add(&state->mouse_queue, report);
}
