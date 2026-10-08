/* Unit tests for src/anim.c. */
#include <math.h>
#include <stdio.h>

#include "anim.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

int main(void)
{
    enum anim_easing e;
    CHECK(anim_easing_from_name("linear", &e) && e == EASE_LINEAR);
    CHECK(anim_easing_from_name("ease-in", &e) && e == EASE_IN);
    CHECK(anim_easing_from_name("ease-out", &e) && e == EASE_OUT);
    CHECK(anim_easing_from_name("ease-in-out", &e) && e == EASE_IN_OUT);
    CHECK(!anim_easing_from_name("bounce", &e));

    /* every curve is anchored at both ends and clamps its input */
    for (int c = EASE_LINEAR; c <= EASE_IN_OUT; c++) {
        CHECK(NEAR(anim_ease(c, 0), 0) && NEAR(anim_ease(c, 1), 1));
        CHECK(NEAR(anim_ease(c, -3), 0) && NEAR(anim_ease(c, 7), 1));
        double prev = -1; /* monotonic */
        for (int i = 0; i <= 100; i++) {
            double v = anim_ease(c, i / 100.0);
            CHECK(v >= prev);
            prev = v;
        }
    }
    CHECK(NEAR(anim_ease(EASE_LINEAR, 0.3), 0.3));
    CHECK(NEAR(anim_ease(EASE_IN, 0.5), 0.125));
    CHECK(NEAR(anim_ease(EASE_OUT, 0.5), 0.875));
    CHECK(NEAR(anim_ease(EASE_IN_OUT, 0.5), 0.5));
    CHECK(NEAR(anim_ease(EASE_IN_OUT, 0.25), 0.0625));
    CHECK(NEAR(anim_ease(EASE_IN_OUT, 0.75), 0.9375));
    CHECK(anim_ease(EASE_IN, 0.3) < 0.3 && anim_ease(EASE_OUT, 0.3) > 0.3); /* slow start / fast start */

    CHECK(NEAR(anim_progress(1000, 1000, 200), 0));
    CHECK(NEAR(anim_progress(1000, 1050, 200), 0.25));
    CHECK(NEAR(anim_progress(1000, 1200, 200), 1));
    CHECK(NEAR(anim_progress(1000, 5000, 200), 1));
    CHECK(NEAR(anim_progress(1000, 1000, 0), 1)); /* no duration: already finished */
    /* the 32-bit millisecond clock wraps around after ~49 days */
    CHECK(NEAR(anim_progress(0xfffffff0u, 0x00000010u, 64), 0.5));

    CHECK(NEAR(anim_lerp(10, 20, 0.5), 15));
    CHECK(NEAR(anim_lerp(20, 10, 0.25), 17.5));

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_anim: all checks passed\n");
    return 0;
}
