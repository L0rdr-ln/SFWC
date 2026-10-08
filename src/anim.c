#include "anim.h"

#include <string.h>

bool anim_easing_from_name(const char *name, enum anim_easing *out)
{
    static const struct {
        const char *name;
        enum anim_easing easing;
    } names[] = {
        {"linear", EASE_LINEAR},
        {"ease-in", EASE_IN},
        {"ease-out", EASE_OUT},
        {"ease-in-out", EASE_IN_OUT},
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        if (!strcmp(name, names[i].name)) {
            *out = names[i].easing;
            return true;
        }
    }
    return false;
}

double anim_ease(enum anim_easing e, double t)
{
    if (t <= 0) {
        return 0;
    }
    if (t >= 1) {
        return 1;
    }
    switch (e) {
    case EASE_IN:
        return t * t * t;
    case EASE_OUT: {
        double u = 1 - t;
        return 1 - u * u * u;
    }
    case EASE_IN_OUT:
        if (t < 0.5) {
            return 4 * t * t * t;
        } else {
            double u = -2 * t + 2;
            return 1 - u * u * u / 2;
        }
    case EASE_LINEAR:
    default:
        return t;
    }
}

double anim_progress(uint32_t start_ms, uint32_t now_ms, uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return 1;
    }
    uint32_t elapsed = now_ms - start_ms; /* wraps correctly */
    if (elapsed >= duration_ms) {
        return 1;
    }
    return (double)elapsed / duration_ms;
}

double anim_lerp(double from, double to, double t)
{
    return from + (to - from) * t;
}
