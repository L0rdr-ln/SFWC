/*
 * Animation maths: easing curves and time-based progress. No wlroots dependency
 * (unit-tested in tests/test_anim.c). The compositor drives the animations from its
 * output frame callbacks; progress is computed from timestamps, so skipped frames only
 * make the animation less smooth, never longer.
 */
#ifndef SFWC_ANIM_H
#define SFWC_ANIM_H

#include <stdbool.h>
#include <stdint.h>

enum anim_easing { EASE_LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT };

/* "linear", "ease-in", "ease-out", "ease-in-out" */
bool anim_easing_from_name(const char *name, enum anim_easing *out);

/* Maps t in [0,1] (clamped) through the curve; 0 -> 0 and 1 -> 1 for every curve. */
double anim_ease(enum anim_easing e, double t);

/* Fraction of the animation that has elapsed, in [0,1]; a zero duration is finished. */
double anim_progress(uint32_t start_ms, uint32_t now_ms, uint32_t duration_ms);

double anim_lerp(double from, double to, double t);

#endif
