// main/alarm_ctl.h —— GPWS 闹钟设置页输入状态机(纯逻辑,host 可测)。
//
// 两行(时长/选曲), modal 同款两态(浏览/编辑):
//   浏览: UP/DOWN 换行, OK 进编辑, OK 长按确认闹钟(确认流程由调用方走)
//   编辑: UP/DOWN 调值(时长 ±1 钳 1..120, 选曲 0..23 环绕), 时长长按连调
//         (调用方启 repeat timer), OK 退编辑
// 值(min/tune)在本状态机内钳制/环绕——闹钟值只在确认时一次性消费
// (NVS+深睡), 无需像音量那样外置应用层常驻。min==0 为「未初始化」,
// 由调用方在首次打开时置默认 5(此后跨开关页保留上次值)。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

#define ALARM_MIN_MIN 1    // 时长下限(分钟)
#define ALARM_MIN_MAX 120  // 时长上限(分钟)
#define ALARM_TUNE_N  24   // 目录条目数(与 app_catalog 锁定; quiz 同款约定)

typedef enum {
    ALARM_ROW_MIN = 0,  // 时长
    ALARM_ROW_TUNE,     // 选曲
    ALARM_ROW_COUNT,
} alarm_row_t;

typedef enum {
    ALARM_NONE = 0,          // event swallowed, nothing to apply
    ALARM_ROW_MOVE,          // browse: row moved (state updated here)
    ALARM_EDIT_ENTER,        // browse OK: enter edit of the selected row
    ALARM_EDIT_EXIT,         // edit OK: back to browse
    ALARM_MIN_UP,            // edit duration: +1 (clamped here)
    ALARM_MIN_DOWN,
    ALARM_MIN_REPEAT_UP,     // edit duration long-press: continuous adjust
    ALARM_MIN_REPEAT_DOWN,   // (caller starts the repeat timer)
    ALARM_TUNE_UP,           // edit tune: wrap +-1 (state updated here)
    ALARM_TUNE_DOWN,
    ALARM_TUNE_REPEAT_UP,    // edit tune long-press: continuous wrap (caller
    ALARM_TUNE_REPEAT_DOWN,  // steps alarm_ctl_tune_step on the repeat timer)
    ALARM_REPEAT_STOP,       // RELEASE: stop the continuous adjust/move
    ALARM_ROW_REPEAT_UP,     // browse long-press: continuous row move (caller
    ALARM_ROW_REPEAT_DOWN,   // steps menu-family rows on the repeat timer)
    ALARM_CONFIRM,           // browse OK long-press: confirm alarm (closes)
} alarm_action_t;

typedef struct {
    bool open;
    bool editing;
    int row;   // alarm_row_t
    int min;   // 1..120 (0 = uninitialized, caller seeds the default 5)
    int tune;  // 0..ALARM_TUNE_N-1
} alarm_ctl_t;

static inline void alarm_ctl_open(alarm_ctl_t *a) {
    a->open = true;
    a->editing = false;
    a->row = ALARM_ROW_MIN;
}

static inline void alarm_ctl_close(alarm_ctl_t *a) {
    a->open = false;
    a->editing = false;
}

// Wrap the row selection by one in direction dir (-1/+1); shared by clicks
// and the continuous-repeat timer in the UI.
static inline void alarm_ctl_row_move(alarm_ctl_t *a, int dir) {
    a->row = (a->row + (dir < 0 ? ALARM_ROW_COUNT - 1 : 1)) % ALARM_ROW_COUNT;
}

// Clamp helper shared by single steps and the continuous adjust in the UI.
static inline int alarm_ctl_min_clamp(int min) {
    if (min < ALARM_MIN_MIN) min = ALARM_MIN_MIN;
    if (min > ALARM_MIN_MAX) min = ALARM_MIN_MAX;
    return min;
}

// Wrap the tune selection by one in direction dir (-1/+1); shared by clicks
// and the continuous-repeat timer in the UI. Returns the new value.
static inline int alarm_ctl_tune_step(int tune, int dir) {
    return (tune + (dir < 0 ? ALARM_TUNE_N - 1 : 1)) % ALARM_TUNE_N;
}

static inline alarm_action_t alarm_ctl_key(alarm_ctl_t *a, bsp_btn_t btn,
                                           bsp_btn_ev_t ev) {
    if (!a->open) return ALARM_NONE;
    switch (ev) {
    case BSP_BTN_CLICK:
        if (btn == BSP_BTN_OK) {
            a->editing = !a->editing;
            return a->editing ? ALARM_EDIT_ENTER : ALARM_EDIT_EXIT;
        }
        if (!a->editing) {
            alarm_ctl_row_move(a, btn == BSP_BTN_UP ? -1 : 1);
            return ALARM_ROW_MOVE;
        }
        if (a->row == ALARM_ROW_MIN) {
            a->min = alarm_ctl_min_clamp(a->min + (btn == BSP_BTN_UP ? 1 : -1));
            return btn == BSP_BTN_UP ? ALARM_MIN_UP : ALARM_MIN_DOWN;
        }
        a->tune = alarm_ctl_tune_step(a->tune, btn == BSP_BTN_UP ? -1 : 1);
        return btn == BSP_BTN_UP ? ALARM_TUNE_UP : ALARM_TUNE_DOWN;
    case BSP_BTN_LONG:
        // OK long-press: confirm only from browse (an in-edit long-press is
        // an accidental hold, not a confirm gesture).
        if (btn == BSP_BTN_OK) {
            if (a->editing) return ALARM_NONE;
            a->open = false;
            return ALARM_CONFIRM;
        }
        if (a->editing) {
            if (a->row == ALARM_ROW_MIN)
                return btn == BSP_BTN_UP ? ALARM_MIN_REPEAT_UP : ALARM_MIN_REPEAT_DOWN;
            return btn == BSP_BTN_UP ? ALARM_TUNE_REPEAT_UP : ALARM_TUNE_REPEAT_DOWN;
        }
        return btn == BSP_BTN_UP ? ALARM_ROW_REPEAT_UP : ALARM_ROW_REPEAT_DOWN;
    case BSP_BTN_RELEASE:
        if (btn != BSP_BTN_OK) return ALARM_REPEAT_STOP;
        return ALARM_NONE;
    default:  // PRESS / DOUBLE: swallowed
        return ALARM_NONE;
    }
}
