// main/menu_ctl.h —— 主菜单输入状态机(纯逻辑,host 可测)。
//
// 主菜单 = OK 长按弹出的功能入口列表(行数随批次生长, 见 MENU_ITEM_*)。
// 只做导航: UP/DOWN CLICK 环绕移动、OK CLICK 进入选中项(动作由调用方派发)、
// OK LONG 关闭;其余事件(PRESS/DOUBLE/RELEASE/方向键 LONG)全部吞掉——菜单行
// 没有连调语义, 连发在菜单打开时已停(modal_open 同款, 37b7ecd)。
// 不复用 modal_ctl: 其 OK=切编辑态/隐藏行位掩码语义与菜单不匹配。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

typedef enum {
    MENU_ITEM_QUIZ = 0,    // 听声辨机(quiz)
    MENU_ITEM_SCENES,      // 场景连播(scene 播放, scenes.h 表)
    MENU_ITEM_ALARM,       // GPWS 闹钟(设置页 + timer 唤醒深睡)
    MENU_ITEM_STATS,       // 播放统计(stats 弹窗, 从设置弹窗迁出)
    MENU_ITEM_SETTINGS,    // 设置(原设置弹窗)
    MENU_ITEM_DIAG,        // 硬件诊断(只读快照页)
    // spec 终态行序(六行)已齐。
    MENU_ITEM_COUNT,
} menu_item_t;

typedef enum {
    MENU_NONE = 0,  // event swallowed, nothing to apply
    MENU_UP,        // selection moved up (state updated here)
    MENU_DOWN,
    MENU_ENTER,     // OK click: enter the selected row (caller dispatches)
    MENU_CLOSE,     // OK long-press: close the menu (screen stays on)
    MENU_REPEAT_UP,   // UP/DOWN long-press: caller starts continuous move
    MENU_REPEAT_DOWN, // (rows advance on the shared repeat timer)
    MENU_REPEAT_STOP, // RELEASE: caller stops the continuous move
} menu_action_t;

typedef struct {
    bool open;
    int count;  // 行数(每次 open 时固定;主菜单传 MENU_ITEM_COUNT,模式页传 2)
    int item;   // menu_item_t: which row is selected
} menu_ctl_t;

static inline void menu_ctl_open(menu_ctl_t *m, int count) {
    m->open = true;
    m->count = count;
    m->item = 0;
}

static inline void menu_ctl_close(menu_ctl_t *m) { m->open = false; }

// Wrap move by one row in direction dir (-1/+1); shared by clicks and the
// continuous-repeat timer in the UI. State (item) is updated here.
static inline void menu_ctl_move(menu_ctl_t *m, int dir) {
    m->item = (m->item + (dir < 0 ? m->count - 1 : 1)) % m->count;
}

// Full key matrix for the open menu. Navigation state (item) is mutated here;
// the returned action tells the caller what to apply. Rows are fixed at
// compile time (no hidden rows), so movement is a plain wrap.
// UP/DOWN long-press starts continuous move (MENU_REPEAT_*): the caller runs
// the shared repeat timer; RELEASE stops it (idempotent when no repeat runs).
static inline menu_action_t menu_ctl_key(menu_ctl_t *m, bsp_btn_t btn,
                                         bsp_btn_ev_t ev) {
    if (!m->open) return MENU_NONE;
    switch (ev) {
    case BSP_BTN_CLICK:
        if (btn == BSP_BTN_OK) return MENU_ENTER;
        menu_ctl_move(m, btn == BSP_BTN_UP ? -1 : 1);
        return btn == BSP_BTN_UP ? MENU_UP : MENU_DOWN;
    case BSP_BTN_LONG:
        if (btn == BSP_BTN_OK) {
            m->open = false;
            return MENU_CLOSE;
        }
        return btn == BSP_BTN_UP ? MENU_REPEAT_UP : MENU_REPEAT_DOWN;
    case BSP_BTN_RELEASE:
        if (btn != BSP_BTN_OK) return MENU_REPEAT_STOP;
        return MENU_NONE;
    default:  // PRESS / DOUBLE: swallowed
        return MENU_NONE;
    }
}
