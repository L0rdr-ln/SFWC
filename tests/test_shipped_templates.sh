#!/bin/sh
# Every template shipped in themes/templates must render with every shipped theme (no
# unknown placeholders), and a theme must actually change the output.
# Usage: test_shipped_templates.sh <sfwc-theme-apply> <source root>
set -u
APPLY=$1; SRC=$2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

n=0
for theme_file in "$SRC"/themes/*.theme; do
    theme=$(basename "$theme_file" .theme)
    SFWC_TEMPLATES="$SRC/themes/templates" "$APPLY" --no-signal --quiet --config-dir "$SRC" \
        --theme "$theme" --out "$T/$theme" || fail "rendering the templates with theme '$theme' failed"
    for tpl in "$SRC"/themes/templates/*.in; do
        out="$T/$theme/$(basename "$tpl" .in)"
        [ -s "$out" ] || fail "$out was not rendered or is empty"
        grep -q '@[a-z_]*\.[a-z_]*' "$out" && fail "$out still contains a placeholder"
        n=$((n + 1))
    done
done
[ "$n" -ge 6 ] || fail "too few templates were rendered ($n)"
cmp -s "$T/default/waybar.css" "$T/light/waybar.css" && fail "the light theme renders like the default theme"
grep -q 'background: #1e1e2e' "$T/default/waybar.css" || fail "default waybar.css has the wrong background: $(grep background "$T/default/waybar.css")"
echo "shipped templates: $n renderings OK"
