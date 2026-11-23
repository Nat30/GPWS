// main/modal_ctl.h —— Settings-popup input state machine (pure logic, host-testable).
//
// The settings popup holds two to four items (volume, autoplay, and the
// autoplay-gated random / listen rows) and is driven by the three shared
// buttons. The play-stats entry and the main menu live in menu_ctl.h.
// This header keeps only the navigation state machine so the full key x
// state matrix can be host-tested without LVGL/ESP-IDF:
//   browse: UP/DOWN click cycles visible items (hidden rows are skipped),
//           OK click enters edit
//   edit:   UP/DOWN click adjusts (volume +-10 via returned action, autoplay
//           /random/listen on/off), UP/DOWN long-press starts continuous
//           volume adjust, RELEASE stops it, OK click returns to browse,
//           OK long-press closes the popup from either mode
// Item values live in app_audio (single source, applied to the codec there);
// this module mutates only navigation state and returns actions to apply.
// While the popup is open every event is consumed, including MODAL_NONE.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"

// Popup auto-close: no key event for this long while open (us).
#define MODAL_IDLE_CLOSE_US (15LL * 1000 * 1000)
// Auto screen-off: no key event for this long, and nothing playing or
// playback paused (screen-off stops paused playback, matching the manual
// double-tap gesture; listen mode keeps playing with the backlight off).
// Pausing refreshes nothing: the span still counts from the last key input,
// so a fresh pause alone never triggers it.
#define SCREEN_IDLE_OFF_US  (60LL * 1000 * 1000)
// Auto deep sleep: no key input for this long, counted from the last input
// (us). Timeline from last input: 60s -> screen off, 5min -> deep sleep.
// Deep sleep additionally requires quiet or paused playback, so a listening
// session (playing with the screen off) never sleeps mid-chain; the chain
// end rolls straight into the deep-sleep timeline.
#define AUTO_SLEEP_US       (5LL * 60 * 1000 * 1000)
// Wake guard fallback after screen-off/wake: swallow events this long (us).
// The real disarm is the RELEASE of the key that woke the screen.
#define WAKE_GUARD_US       (600LL * 1000)

typedef enum {
    MODAL_ITEM_VOLUME = 0,
    MODAL_ITEM_AUTOPLAY,
    MODAL_ITEM_RANDOM,   // only selectable while its row is visible (autoplay on)
    MODAL_ITEM_LISTEN,   // ditto; "keep playing with the screen off"
    MODAL_ITEM_COUNT,
} modal_item_t;

typedef enum {
    MODAL_NONE = 0,        // event swallowed, nothing to apply
    MODAL_ITEM_PREV,       // browse: selection moved (state updated here)
    MODAL_ITEM_NEXT,
    MODAL_EDIT_ENTER,      // entered edit of the selected item
    MODAL_EDIT_EXIT,       // returned to browse
    MODAL_VOL_UP,          // edit volume: single step, caller applies +-10
    MODAL_VOL_DOWN,
    MODAL_VOL_REPEAT_UP,   // edit volume: start continuous adjust (repeat timer)
    MODAL_VOL_REPEAT_DOWN,
    MODAL_REPEAT_STOP,     // RELEASE: stop the continuous adjust/move
    MODAL_AUTOPLAY_TOGGLE, // edit autoplay: caller flips the setting
    MODAL_RANDOM_TOGGLE,   // edit random: ditto
    MODAL_LISTEN_TOGGLE,   // edit listen: ditto
    MODAL_ITEM_REPEAT_UP,   // browse long-press: continuous item move (caller
    MODAL_ITEM_REPEAT_DOWN, // steps modal_ctl_item_next on the repeat timer)
    MODAL_CLOSE,           // OK long-press: close popup (screen stays on)
} modal_action_t;

typedef struct {
    bool open;
    bool editing;
    int item;           // modal_item_t: which row is selected
    uint8_t hidden;     // bit per modal_item_t: navigation skips hidden rows
} modal_ctl_t;

// Set/clear one row's hidden bit (caller syncs bits from app_audio state on
// every refresh; hidden rows are skipped by navigation and never selected).
static inline void modal_ctl_hidden_set(modal_ctl_t *m, int item, bool hide) {
    uint8_t bit = (uint8_t)(1u << item);
    if (hide) m->hidden |= bit;
    else m->hidden &= (uint8_t)~bit;
}

// Open resets to browse mode on the volume item. The autoplay-gated rows
// start hidden; the caller unhides them when autoplay is on.
static inline void modal_ctl_open(modal_ctl_t *m) {
    m->open = true;
    m->editing = false;
    m->item = MODAL_ITEM_VOLUME;
    m->hidden = (uint8_t)((1u << MODAL_ITEM_RANDOM) | (1u << MODAL_ITEM_LISTEN));
}

// Close also drops edit mode so a later reopen starts from browse.
static inline void modal_ctl_close(modal_ctl_t *m) {
    m->open = false;
    m->editing = false;
}

// Clamp a volume step to 0..100. The UI only feeds multiples of 10 (default
// 80, steps +-10), so this only guards the bounds.
static inline int modal_ctl_volume_step(int vol, int delta) {
    vol += delta;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return vol;
}

// Next visible item from `from` in direction dir (-1/+1), wrapping. Returns
// `from` itself when it is the only visible row (action still reported; the
// caller's refresh is idempotent). All-hidden never happens in the UI.
static inline int modal_ctl_item_next(const modal_ctl_t *m, int from, int dir) {
    for (int i = 1; i <= MODAL_ITEM_COUNT; i++) {
        int t = (from + (dir < 0 ? MODAL_ITEM_COUNT - i : i)) % MODAL_ITEM_COUNT;
        if (!(m->hidden & (uint8_t)(1u << t))) return t;
    }
    return from;
}

// Full key x state matrix for the open popup. Navigation state (item/editing)
// is mutated here; the returned action tells the caller what to apply.
// Boolean rows (autoplay/random/listen) toggle on either UP or DOWN click in
// edit mode, so the value cycles on repeated presses instead of sticking at
// ON for one key and OFF for the other. Browse-mode long-press starts a
// continuous item move (MODAL_ITEM_REPEAT_*) on the caller's repeat timer;
// RELEASE stops it (idempotent when no repeat runs).
static inline modal_action_t modal_ctl_key(modal_ctl_t *m, bsp_btn_t btn,
                                           bsp_btn_ev_t ev) {
    if (!m->open) return MODAL_NONE;
    switch (ev) {
    case BSP_BTN_CLICK:
        if (btn == BSP_BTN_OK) {
            m->editing = !m->editing;
            return m->editing ? MODAL_EDIT_ENTER : MODAL_EDIT_EXIT;
        }
        if (!m->editing) {
            m->item = modal_ctl_item_next(m, m->item,
                                          btn == BSP_BTN_UP ? -1 : 1);
            return btn == BSP_BTN_UP ? MODAL_ITEM_PREV : MODAL_ITEM_NEXT;
        }
        switch (m->item) {
        case MODAL_ITEM_VOLUME:
            return btn == BSP_BTN_UP ? MODAL_VOL_UP : MODAL_VOL_DOWN;
        case MODAL_ITEM_AUTOPLAY:
            return MODAL_AUTOPLAY_TOGGLE;
        case MODAL_ITEM_RANDOM:
            return MODAL_RANDOM_TOGGLE;
        case MODAL_ITEM_LISTEN:
            return MODAL_LISTEN_TOGGLE;
        default:
            return MODAL_NONE;
        }
    case BSP_BTN_LONG:
        // OK long-press closes the popup (browse or edit), screen stays on.
        // UP/DOWN long-press: continuous adjust for volume (edit mode),
        // continuous item move in browse mode; other items are inert.
        if (btn == BSP_BTN_OK) {
            m->open = false;
            m->editing = false;
            return MODAL_CLOSE;
        }
        if (m->editing && m->item == MODAL_ITEM_VOLUME)
            return btn == BSP_BTN_UP ? MODAL_VOL_REPEAT_UP : MODAL_VOL_REPEAT_DOWN;
        if (!m->editing)
            return btn == BSP_BTN_UP ? MODAL_ITEM_REPEAT_UP : MODAL_ITEM_REPEAT_DOWN;
        return MODAL_NONE;
    case BSP_BTN_RELEASE:
        if (btn != BSP_BTN_OK) return MODAL_REPEAT_STOP;
        return MODAL_NONE;
    default:  // PRESS / DOUBLE: swallowed, no action
        return MODAL_NONE;
    }
}
