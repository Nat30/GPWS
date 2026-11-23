// Host tests for the alarm setup page state machine (alarm_ctl.h).
#include <assert.h>
#include <stdio.h>

#include "alarm_ctl.h"
#include "bsp_button.h"

static alarm_ctl_t a;

static alarm_action_t key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    return alarm_ctl_key(&a, btn, ev);
}

int main(void) {
    // Closed page: every event swallowed.
    a = (alarm_ctl_t){0};
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == ALARM_NONE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == ALARM_NONE);
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == ALARM_NONE);

    // Open resets to browse on the duration row; values survive a close
    // (caller seeds the default 5 once, later opens keep the last pick).
    a.min = 7;
    a.tune = 3;
    alarm_ctl_open(&a);
    assert(a.open && !a.editing && a.row == ALARM_ROW_MIN);
    assert(a.min == 7 && a.tune == 3);  // values not touched by open

    // Browse: UP/DOWN wraps rows both ways.
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == ALARM_ROW_MOVE);
    assert(a.row == ALARM_ROW_TUNE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == ALARM_ROW_MOVE);
    assert(a.row == ALARM_ROW_MIN);  // wraps
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == ALARM_ROW_MOVE);
    assert(a.row == ALARM_ROW_TUNE);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == ALARM_ROW_MOVE);
    assert(a.row == ALARM_ROW_MIN);  // wraps

    // Duration edit: +-1 clamped at 1..120; long-press repeat actions;
    // RELEASE stops the repeat; tune long-press is inert.
    a.min = 1;
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == ALARM_EDIT_ENTER);
    assert(a.editing);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == ALARM_MIN_DOWN);
    assert(a.min == 1);  // clamped at the lower bound
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == ALARM_MIN_UP);
    assert(a.min == 2);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == ALARM_MIN_REPEAT_UP);
    assert(a.min == 2);  // repeat action only: the caller steps + timer
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == ALARM_REPEAT_STOP);
    a.min = 120;
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == ALARM_MIN_UP);
    assert(a.min == 120);  // clamped at the upper bound
    a.min = 60;
    assert(key(BSP_BTN_DOWN, BSP_BTN_LONG) == ALARM_MIN_REPEAT_DOWN);
    assert(key(BSP_BTN_DOWN, BSP_BTN_RELEASE) == ALARM_REPEAT_STOP);
    // OK long-press inside edit is not a confirm; OK click exits edit.
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == ALARM_NONE);
    assert(a.open && a.editing);
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == ALARM_EDIT_EXIT);
    assert(!a.editing);

    // Tune edit: wraps 0..23 both ways; long-press repeats (continuous wrap),
    // RELEASE stops; tune long-press is inert.
    a.row = ALARM_ROW_TUNE;
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == ALARM_EDIT_ENTER);
    a.tune = 0;
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == ALARM_TUNE_UP);
    assert(a.tune == 23);  // wraps up
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == ALARM_TUNE_DOWN);
    assert(a.tune == 0);   // wraps down
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == ALARM_TUNE_REPEAT_UP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_LONG) == ALARM_TUNE_REPEAT_DOWN);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == ALARM_REPEAT_STOP);
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == ALARM_EDIT_EXIT);

    // Tune wrap helper (the UI repeat timer steps it).
    assert(alarm_ctl_tune_step(0, -1) == 23);
    assert(alarm_ctl_tune_step(23, 1) == 0);
    assert(alarm_ctl_tune_step(5, 1) == 6);
    assert(alarm_ctl_tune_step(5, -1) == 4);

    // Confirm: OK long-press from browse only; closes the page.
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == ALARM_CONFIRM);
    assert(!a.open && !a.editing);
    // Closed again: everything swallowed, values kept for the next open.
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == ALARM_NONE);
    assert(a.tune == 0);

    // Browse: UP/DOWN long-press starts continuous row move; RELEASE stops it
    // (idempotent); OK RELEASE stays swallowed; PRESS/DOUBLE swallowed.
    alarm_ctl_open(&a);
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == ALARM_ROW_REPEAT_UP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_LONG) == ALARM_ROW_REPEAT_DOWN);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == ALARM_REPEAT_STOP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_PRESS) == ALARM_NONE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_DOUBLE) == ALARM_NONE);
    assert(key(BSP_BTN_OK, BSP_BTN_RELEASE) == ALARM_NONE);

    // Browse wrap-move helper (the UI repeat timer steps it).
    assert(a.row == ALARM_ROW_MIN);
    alarm_ctl_row_move(&a, 1);
    assert(a.row == ALARM_ROW_TUNE);
    alarm_ctl_row_move(&a, 1);
    assert(a.row == ALARM_ROW_MIN);  // wraps
    alarm_ctl_row_move(&a, -1);
    assert(a.row == ALARM_ROW_TUNE);

    // Clamp helper bounds.
    assert(alarm_ctl_min_clamp(0) == 1);
    assert(alarm_ctl_min_clamp(121) == 120);
    assert(alarm_ctl_min_clamp(42) == 42);

    puts("alarm setup state-machine tests: PASS");
    return 0;
}
