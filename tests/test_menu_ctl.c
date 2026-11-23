// Host tests for the main-menu input state machine (menu_ctl.h).
// Compiles like test_modal_ctl.c (BSP headers via stubs).
#include <assert.h>
#include <stdio.h>

#include "bsp_button.h"
#include "menu_ctl.h"

static menu_ctl_t m;

static menu_action_t key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    return menu_ctl_key(&m, btn, ev);
}

int main(void) {
    // Closed menu: every event swallowed, state untouched.
    m = (menu_ctl_t){0};
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MENU_NONE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_NONE);
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == MENU_NONE);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MENU_NONE);
    assert(!m.open && m.item == MENU_ITEM_QUIZ);

    // Open resets to the first row (row count fixed at open).
    m.item = MENU_ITEM_SETTINGS;
    menu_ctl_open(&m, MENU_ITEM_COUNT);
    assert(m.open && m.item == MENU_ITEM_QUIZ);

    // UP/DOWN click cycles rows, wrapping both ways (6 rows, spec final order).
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_DOWN);
    assert(m.item == MENU_ITEM_SCENES);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_DOWN);
    assert(m.item == MENU_ITEM_ALARM);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_DOWN);
    assert(m.item == MENU_ITEM_STATS);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_DOWN);
    assert(m.item == MENU_ITEM_SETTINGS);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_DOWN);
    assert(m.item == MENU_ITEM_DIAG);
    assert(key(BSP_BTN_DOWN, BSP_BTN_CLICK) == MENU_DOWN);
    assert(m.item == MENU_ITEM_QUIZ);  // wraps
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MENU_UP);
    assert(m.item == MENU_ITEM_DIAG);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MENU_UP);
    assert(m.item == MENU_ITEM_SETTINGS);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MENU_UP);
    assert(m.item == MENU_ITEM_STATS);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MENU_UP);
    assert(m.item == MENU_ITEM_ALARM);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MENU_UP);
    assert(m.item == MENU_ITEM_SCENES);
    assert(key(BSP_BTN_UP, BSP_BTN_CLICK) == MENU_UP);
    assert(m.item == MENU_ITEM_QUIZ);  // wraps

    // OK click enters the selected row (state untouched; caller dispatches).
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MENU_ENTER);
    assert(m.open && m.item == MENU_ITEM_QUIZ);

    // OK long-press closes; after close every event is swallowed again.
    assert(key(BSP_BTN_OK, BSP_BTN_LONG) == MENU_CLOSE);
    assert(!m.open);
    assert(key(BSP_BTN_OK, BSP_BTN_CLICK) == MENU_NONE);
    menu_ctl_open(&m, MENU_ITEM_COUNT);

    // UP/DOWN long-press starts continuous move; RELEASE stops it (idempotent);
    // OK RELEASE stays swallowed (menus have no OK repeat).
    assert(key(BSP_BTN_UP, BSP_BTN_LONG) == MENU_REPEAT_UP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_LONG) == MENU_REPEAT_DOWN);
    assert(key(BSP_BTN_UP, BSP_BTN_RELEASE) == MENU_REPEAT_STOP);
    assert(key(BSP_BTN_DOWN, BSP_BTN_RELEASE) == MENU_REPEAT_STOP);
    assert(key(BSP_BTN_OK, BSP_BTN_RELEASE) == MENU_NONE);
    // PRESS/DOUBLE swallowed, item untouched.
    assert(key(BSP_BTN_DOWN, BSP_BTN_PRESS) == MENU_NONE);
    assert(key(BSP_BTN_DOWN, BSP_BTN_DOUBLE) == MENU_NONE);
    assert(m.open && m.item == MENU_ITEM_QUIZ);

    // Shared wrap-move helper (the UI repeat timer steps it).
    menu_ctl_move(&m, 1);
    assert(m.item == MENU_ITEM_SCENES);
    menu_ctl_move(&m, -1);
    assert(m.item == MENU_ITEM_QUIZ);  // wraps

    puts("main-menu input state-machine tests: PASS");
    return 0;
}
