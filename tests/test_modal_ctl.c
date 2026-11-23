// Host tests for the settings-popup input state machine (modal_ctl.h).
// Compiles like test_key_guard.c but needs the BSP headers via stubs
// (bsp_button.h -> esp_err.h).
#include <assert.h>
#include <stdio.h>

#include "bsp_button.h"
#include "modal_ctl.h"

static modal_ctl_t m;

static modal_action_t key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    return modal_ctl_key(&m, btn, ev);
}

int main(void) {
    // Volume step clamps at both ends.
    assert(modal_ctl_volume_step(80, 10) == 90);
    assert(modal_ctl_volume_step(90, 10) == 100);
    assert(modal_ctl_volume_step(100, 10) == 100);
    assert(modal_ctl_volume_step(30, -10) == 20);
    assert(modal_ctl_volume_step(0, -10) == 0);

    // Closed popup: every event swallowed, state untouched.
    m = (modal_ctl_t){0};
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MODAL_NONE);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MODAL_NONE);
    assert(!m.open && !m.editing && m.item == MODAL_ITEM_VOLUME);

    // Open resets to browse on the volume item.
    m.editing = true;
    m.item = MODAL_ITEM_AUTOPLAY;
    modal_ctl_open(&m);
    assert(m.open && !m.editing && m.item == MODAL_ITEM_VOLUME);

    // Browse: UP/DOWN click cycles volume/autoplay (the gated rows are
    // hidden right after open; the stats row moved to the main menu),
    // wrapping both ways.
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_ITEM_PREV);
    assert(m.item == MODAL_ITEM_AUTOPLAY);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_ITEM_PREV);
    assert(m.item == MODAL_ITEM_VOLUME);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_AUTOPLAY);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_VOLUME);

    // Browse: LONG starts continuous item move, RELEASE stops it (idempotent
    // stop - no repeat running); PRESS/DOUBLE swallowed, edit state untouched.
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MODAL_ITEM_REPEAT_UP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_LONG) == MODAL_ITEM_REPEAT_DOWN);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MODAL_REPEAT_STOP);
    assert(key(BSP_BTN_OK, BSP_BTN_RELEASE) == MODAL_NONE);
    assert(key(BSP_BTN_OK, BSP_BTN_DOUBLE) == MODAL_NONE);
    assert(key(BSP_BTN_OK, BSP_BTN_PRESS) == MODAL_NONE);
    assert(!m.editing);

    // OK long-press closes the popup from browse mode (screen stays on).
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == MODAL_CLOSE);
    assert(!m.open && !m.editing);
    // After close, every event is swallowed again.
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MODAL_NONE);
    modal_ctl_open(&m);

    // OK click enters edit, another click exits.
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MODAL_EDIT_ENTER);
    assert(m.editing);
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MODAL_EDIT_EXIT);
    assert(!m.editing);

    // Edit volume: click steps, long-press starts repeat, release stops.
    m.item = MODAL_ITEM_VOLUME;
    m.editing = true;
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_VOL_UP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_VOL_DOWN);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MODAL_VOL_REPEAT_UP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_LONG) == MODAL_VOL_REPEAT_DOWN);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MODAL_REPEAT_STOP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_RELEASE) == MODAL_REPEAT_STOP);
    // OK release stays swallowed while editing.
    assert(key(BSP_BTN_OK, BSP_BTN_RELEASE) == MODAL_NONE);

    // OK long-press also closes the popup from edit mode (drops editing).
    assert(m.editing);
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == MODAL_CLOSE);
    assert(!m.open && !m.editing);

    // Edit autoplay: either key toggles (value cycles on repeated presses);
    // no repeat adjust for boolean rows (stop on release is idempotent).
    modal_ctl_open(&m);
    m.item = MODAL_ITEM_AUTOPLAY;
    m.editing = true;
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_AUTOPLAY_TOGGLE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_AUTOPLAY_TOGGLE);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MODAL_NONE);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MODAL_REPEAT_STOP);

    // Open defaults the autoplay-gated rows to hidden (caller unhides them
    // with autoplay on).
    assert(m.hidden & (1u << MODAL_ITEM_RANDOM));
    assert(m.hidden & (1u << MODAL_ITEM_LISTEN));
    modal_ctl_open(&m);
    assert(m.item == MODAL_ITEM_VOLUME);
    assert(m.hidden & (1u << MODAL_ITEM_RANDOM));
    assert(m.hidden & (1u << MODAL_ITEM_LISTEN));

    // Both gated rows hidden: browse cycles volume/autoplay, wrapping skips
    // the hidden rows both ways.
    assert(m.item == MODAL_ITEM_VOLUME);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_AUTOPLAY);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_VOLUME);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_ITEM_PREV);
    assert(m.item == MODAL_ITEM_AUTOPLAY);

    // Both gated rows visible: browse cycles all four items both ways.
    modal_ctl_hidden_set(&m, MODAL_ITEM_RANDOM, false);
    modal_ctl_hidden_set(&m, MODAL_ITEM_LISTEN, false);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_RANDOM);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_LISTEN);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_VOLUME);  // wraps
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_ITEM_PREV);
    assert(m.item == MODAL_ITEM_LISTEN);

    // Only listen visible besides volume/autoplay (random hidden):
    // navigation skips exactly the random row.
    modal_ctl_open(&m);  // hides both gated rows; unhide listen only
    modal_ctl_hidden_set(&m, MODAL_ITEM_LISTEN, false);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_AUTOPLAY);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_LISTEN);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_VOLUME);  // wraps (no stats row anymore)

    // Edit random: either key toggles; LONG inert for boolean rows.
    m.item = MODAL_ITEM_RANDOM;
    m.editing = true;
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_RANDOM_TOGGLE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_RANDOM_TOGGLE);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MODAL_NONE);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MODAL_REPEAT_STOP);

    // Edit listen: either key toggles (like autoplay/random).
    m.item = MODAL_ITEM_LISTEN;
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MODAL_LISTEN_TOGGLE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_LISTEN_TOGGLE);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MODAL_NONE);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MODAL_REPEAT_STOP);

    // Stale focus recovery: item stuck on a hidden row -> one step lands on
    // the nearest visible row (skips any number of hidden rows).
    m.editing = false;
    modal_ctl_hidden_set(&m, MODAL_ITEM_RANDOM, true);
    modal_ctl_hidden_set(&m, MODAL_ITEM_LISTEN, true);
    m.item = MODAL_ITEM_RANDOM;  // hidden: DOWN skips the hidden listen row
    key(BSP_BTN_DOWN, BSP_BTN_CLICK);
    assert(m.item == MODAL_ITEM_VOLUME);  // ... and wraps to volume
    m.item = MODAL_ITEM_LISTEN;  // hidden: UP skips the hidden random row
    key(BSP_BTN_UP, BSP_BTN_CLICK);
    assert(m.item == MODAL_ITEM_AUTOPLAY);

    // Only one visible row: navigation reports the move but stays put
    // (idempotent refresh on the caller side).
    m.hidden = (uint8_t)~(1u << MODAL_ITEM_VOLUME);
    m.item = MODAL_ITEM_VOLUME;
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MODAL_ITEM_NEXT);
    assert(m.item == MODAL_ITEM_VOLUME);

    // Close drops edit mode; reopen resets to volume browse with the gated
    // rows hidden again.
    m.editing = true;
    m.item = MODAL_ITEM_AUTOPLAY;
    modal_ctl_close(&m);
    assert(!m.open && !m.editing);
    modal_ctl_open(&m);
    assert(m.open && !m.editing && m.item == MODAL_ITEM_VOLUME);
    assert(m.hidden == ((1u << MODAL_ITEM_RANDOM) | (1u << MODAL_ITEM_LISTEN)));

    // Timeout constants keep their relative contract (popup closes long
    // before the idle screen-off can fire).
    assert(MODAL_IDLE_CLOSE_US < SCREEN_IDLE_OFF_US);
    assert(WAKE_GUARD_US < MODAL_IDLE_CLOSE_US);

    puts("settings modal input state-machine tests: PASS");
    return 0;
}
