// Host tests for the scene-sequence static table (scenes.h).
#include <assert.h>
#include <stdio.h>

#include "scenes.h"

#define CATALOG_N 24  // must match APP_CATALOG_COUNT (app_catalog.h)

int main(void) {
    // Table integrity: fixed count, non-empty names.
    assert(APP_SCENE_COUNT == 6);
    for (int s = 0; s < APP_SCENE_COUNT; s++) {
        assert(APP_SCENES[s].name && APP_SCENES[s].name[0]);
        // Sequence: at least one entry, all indices in catalog range,
        // no consecutive repeats (a scene advances, never stutters).
        assert(APP_SCENES[s].n > 0);
        for (int i = 0; i < APP_SCENES[s].n; i++) {
            assert(APP_SCENES[s].seq[i] >= 0 && APP_SCENES[s].seq[i] < CATALOG_N);
            if (i > 0) assert(APP_SCENES[s].seq[i] != APP_SCENES[s].seq[i - 1]);
        }
    }
    // Total scheduled plays across all scenes (sanity against table edits).
    int total = 0;
    for (int s = 0; s < APP_SCENE_COUNT; s++) total += APP_SCENES[s].n;
    assert(total == 24);

    puts("scene table tests: PASS");
    return 0;
}
