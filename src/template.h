/*
 * Theme templating: replace @section.key@ / @section.key:modifier@ in a text with values of
 * the active theme, so that one theme can style the companion tools too (waybar, fuzzel, ...).
 * No wlroots dependency (unit-tested in tests/test_template.c).
 */
#ifndef SFWC_TEMPLATE_H
#define SFWC_TEMPLATE_H

#include <stdbool.h>
#include <stddef.h>

#include "theme.h"

/*
 * Value of the theme key "section.key" (colors.*, geometry.*, shadow.*, font.*, theme.name).
 * Colors are written "#rrggbb" ("#rrggbbaa" when not opaque); a modifier selects another
 * format: hex (rrggbb), hexa (rrggbbaa), rgb ("r, g, b"), rgba ("rgba(r, g, b, a)").
 * Returns false (and fills `out` with a message) for unknown keys and modifiers.
 */
bool theme_value(const struct theme *t, const char *key, const char *modifier, char *out, size_t n);

/*
 * Render `in`. Strict mode (template files): "@@" is a literal '@', an unknown placeholder
 * or an unterminated '@' is an error (NULL, message in `err` with the line number).
 * Lenient mode (commands in the config): only well-formed placeholders that name a theme key
 * are replaced, everything else (e-mail addresses, "ssh user@host") stays as it is.
 * The caller frees the result.
 */
char *template_render(const struct theme *t, const char *in, bool strict, char *err, size_t err_n);

#endif
