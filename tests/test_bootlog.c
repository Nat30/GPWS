// Host tests for the reboot-history ring log (bootlog.h).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bootlog.h"

int main(void) {
    bootlog_t b;
    bootlog_rec_t r;

    // Empty log: get fails for every n.
    b = (bootlog_t){0};
    assert(!bootlog_get(&b, 0, &r));
    assert(!bootlog_get(&b, 1, &r));

    // Append grows count, head points at the newest.
    bootlog_append(&b, 1, 0, 0, 5);   // run 1: POWERON wc=0 5min
    assert(b.count == 1);
    assert(bootlog_get(&b, 0, &r) && r.rr == 1 && r.run_min == 5);
    assert(bootlog_get(&b, 1, &r) == false);
    bootlog_append(&b, 12, 0, 0, 3);  // run 2: SW(esp_restart)
    assert(b.count == 2);
    assert(bootlog_get(&b, 0, &r) && r.rr == 12 && r.run_min == 3);
    assert(bootlog_get(&b, 1, &r) && r.rr == 1);

    // Touch updates the current (head) slot only.
    bootlog_touch(&b, 3, 7);
    assert(bootlog_get(&b, 0, &r) && r.site == 3 && r.run_min == 7);
    assert(bootlog_get(&b, 1, &r) && r.site == 0 && r.run_min == 5);

    // Ring wraps at CAP: the oldest entry is overwritten, count stays capped.
    bootlog_t w = (bootlog_t){0};
    for (int i = 0; i < BOOTLOG_CAP + 4; i++) {
        bootlog_append(&w, (uint8_t)(i + 1), 0, 0, (uint8_t)i);
    }
    assert(w.count == BOOTLOG_CAP);
    // Newest = last appended (i = CAP+3 -> rr = CAP+4); oldest surviving =
    // i = 4 (the first four were overwritten), i.e. rr = 5 at n = CAP-1.
    assert(bootlog_get(&w, 0, &r) && r.rr == BOOTLOG_CAP + 4);
    assert(bootlog_get(&w, BOOTLOG_CAP - 1, &r) && r.rr == 5);
    assert(!bootlog_get(&w, BOOTLOG_CAP, &r));
    // Full-ring order is strictly newest -> oldest.
    for (int n = 0; n < BOOTLOG_CAP; n++) {
        assert(bootlog_get(&w, n, &r));
        assert(r.rr == (uint8_t)(BOOTLOG_CAP + 4 - n));
    }

    // Power-cut record stays distinguishable (no touch -> run_min 0).
    bootlog_append(&w, 1, 0, 0, 0);
    assert(bootlog_get(&w, 0, &r) && r.run_min == 0 && r.site == 0);

    puts("boot ring-log tests: PASS");
    return 0;
}
