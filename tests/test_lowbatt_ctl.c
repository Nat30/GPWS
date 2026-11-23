// 低电保护决策(lowbatt_ctl.h)的 host 测试:伪像跳过、去抖计数、强睡门限、
// 提醒闩滞回、弹窗互斥。历史回归锚点:ff58464(伪 0% fall-through 强睡)、
// c3613b4(伪像拍 return 掉出分支)。
#include <assert.h>
#include <stdio.h>

#include "lowbatt_ctl.h"

int main(void) {
    lowbatt_ctl_t m;

    // ---- 伪像拍:重算窗口的假 0%(soc 低 + 电压健康)----
    // ff58464 回归:伪像若掉出去抖分支,会 fall-through 直达强睡——
    // 无论连续多少拍伪像,永不 SLEEP、streak 恒 0。
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < 50; i++) {
        assert(lowbatt_ctl_poll(&m, 0, 4000, false) == LOWBATT_NONE);
    }
    assert(m.low_streak == 0);

    // 伪像拍不清零也不累加 streak:3 拍真低 + 1 拍伪像后 streak 仍 3,
    // 第 4 拍真低(streak→4)即 SLEEP——伪像拍没有消耗去抖预算。
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < 3; i++) assert(lowbatt_ctl_poll(&m, 4, 3500, false) == LOWBATT_NONE);
    assert(lowbatt_ctl_poll(&m, 0, 3900, false) == LOWBATT_NONE);  // 伪像
    assert(m.low_streak == 3);
    assert(lowbatt_ctl_poll(&m, 4, 3500, false) == LOWBATT_SLEEP); // 第 4 拍真低

    // ---- 强睡:≤5 连续 4 拍真低,不受弹窗/播放门限 ----
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < LOW_BATT_CONFIRM_POLLS - 1; i++) {
        assert(lowbatt_ctl_poll(&m, 5, 3600, true) == LOWBATT_NONE);  // modal_busy 也不挡
    }
    assert(lowbatt_ctl_poll(&m, 5, 3600, true) == LOWBATT_SLEEP);

    // mv<0(电压读失败)保守按真低计。
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < LOW_BATT_CONFIRM_POLLS; i++) {
        assert(lowbatt_ctl_poll(&m, 3, -1, false) ==
               (i == LOW_BATT_CONFIRM_POLLS - 1 ? LOWBATT_SLEEP : LOWBATT_NONE));
    }

    // ---- 提醒:≤10(且 >5)连续 4 拍真低且弹窗没开 ----
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < LOW_BATT_CONFIRM_POLLS - 1; i++) {
        assert(lowbatt_ctl_poll(&m, 8, 3600, false) == LOWBATT_NONE);
    }
    assert(lowbatt_ctl_poll(&m, 8, 3600, false) == LOWBATT_WARN);
    assert(m.warned);

    // 弹窗开着:不提醒、闩不置位(下一拍复查),弹窗关了才提。
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < LOW_BATT_CONFIRM_POLLS + 3; i++) {
        assert(lowbatt_ctl_poll(&m, 8, 3600, true) == LOWBATT_NONE);
        assert(!m.warned);
    }
    assert(lowbatt_ctl_poll(&m, 8, 3600, false) == LOWBATT_WARN);

    // ---- 提醒闩滞回:提过一次后不再提,回弹 ≥15 解闩,再跌回再提 ----
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < LOW_BATT_CONFIRM_POLLS; i++) {
        lowbatt_ctl_poll(&m, 8, 3600, false);  // 第 4 拍 WARN
    }
    assert(m.warned);
    // 仍在低位:NONE(哪怕 streak 早已 ≥4)。
    for (int i = 0; i < 10; i++) {
        assert(lowbatt_ctl_poll(&m, 8, 3600, false) == LOWBATT_NONE);
    }
    // 11..14(WARN 之上、REARM 之下)不解闩。
    for (int soc = 11; soc < LOW_BATT_REARM_SOC; soc++) {
        assert(lowbatt_ctl_poll(&m, soc, 3800, false) == LOWBATT_NONE);
        assert(m.warned);
    }
    // 伪像拍不动闩。
    assert(lowbatt_ctl_poll(&m, 0, 4000, false) == LOWBATT_NONE);
    assert(m.warned);
    // ≥15 解闩 + streak 清零;再跌回 8 需重新去抖 4 拍。
    assert(lowbatt_ctl_poll(&m, 15, 3800, false) == LOWBATT_NONE);
    assert(!m.warned && m.low_streak == 0);
    for (int i = 0; i < LOW_BATT_CONFIRM_POLLS - 1; i++) {
        assert(lowbatt_ctl_poll(&m, 8, 3600, false) == LOWBATT_NONE);
    }
    assert(lowbatt_ctl_poll(&m, 8, 3600, false) == LOWBATT_WARN);

    // ---- 常态拍:高于 WARN 清零 streak ----
    m = (lowbatt_ctl_t){0};
    for (int i = 0; i < 3; i++) lowbatt_ctl_poll(&m, 9, 3600, false);
    assert(m.low_streak == 3);
    for (int i = 0; i < 5; i++) {
        assert(lowbatt_ctl_poll(&m, 50, 3900, false) == LOWBATT_NONE);
        assert(m.low_streak == 0);
    }
    // 闩已置位时高于 REARM 才解;高位恒 NONE。
    m.warned = true;
    assert(lowbatt_ctl_poll(&m, 50, 3900, false) == LOWBATT_NONE);
    assert(!m.warned);

    puts("lowbatt_ctl decision tests: PASS");
    return 0;
}
