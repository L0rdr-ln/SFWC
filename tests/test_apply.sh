#!/bin/sh
# Tests of the sfwc-theme-apply helper: rendering, theme lookup, template override, --skip,
# unchanged outputs are not rewritten, bad templates are reported but do not stop the rest.
# Usage: test_apply.sh <sfwc-theme-apply>
set -u
APPLY=$1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$T/cfg/themes" "$T/tpl" "$T/user" "$T/out"
printf '[colors]\nbackground = #102030\n[font]\nfamily = Iosevka\n' >"$T/cfg/themes/mine.theme"
printf 'bg=@colors.background@ hex=@colors.background:hex@ font=@font.family@ n=@theme.name@\n' >"$T/tpl/a.conf.in"
printf 'width=@geometry.border_width@\n' >"$T/tpl/b.css.in"
printf 'mail me@@example.org\n' >"$T/tpl/c.txt.in"

run() { "$APPLY" --no-signal --config-dir "$T/cfg" --templates "$T/tpl" --out "$T/out" "$@"; }

# 1. renders with the named theme
OUT=$(run --theme mine 2>&1) || fail "first run failed: $OUT"
[ "$(cat "$T/out/a.conf")" = "bg=#102030 hex=102030 font=Iosevka n=default" ] || fail "a.conf: $(cat "$T/out/a.conf")"
[ "$(cat "$T/out/b.css")" = "width=2" ] || fail "b.css: $(cat "$T/out/b.css")"
[ "$(cat "$T/out/c.txt")" = "mail me@example.org" ] || fail "c.txt: $(cat "$T/out/c.txt")"
echo "$OUT" | grep -q "updated a.conf" || fail "no 'updated a.conf' message"

# 2. nothing changed: nothing rewritten
OUT=$(run --theme mine 2>&1) || fail "second run failed"
[ -z "$OUT" ] || fail "unchanged outputs were reported: $OUT"

# 3. a theme change updates only what depends on it
printf '[geometry]\nborder_width = 5\n' >>"$T/cfg/themes/mine.theme"
OUT=$(run --theme mine 2>&1) || fail "third run failed"
echo "$OUT" | grep -q "updated b.css" || fail "b.css was not updated"
echo "$OUT" | grep -q "a.conf" && fail "a.conf was rewritten without a change"
[ "$(cat "$T/out/b.css")" = "width=5" ] || fail "b.css after the theme change: $(cat "$T/out/b.css")"

# 4. --skip
rm -f "$T/out/b.css"
run --theme mine --skip b >/dev/null 2>&1 || fail "run with --skip failed"
[ ! -e "$T/out/b.css" ] || fail "--skip did not skip b"

# 5. the first template directory wins
printf 'user=@font.family@\n' >"$T/user/a.conf.in"
"$APPLY" --no-signal --config-dir "$T/cfg" --templates "$T/user" --templates "$T/tpl" --out "$T/out" --theme mine >/dev/null 2>&1 || fail "override run failed"
[ "$(cat "$T/out/a.conf")" = "user=Iosevka" ] || fail "user template did not win: $(cat "$T/out/a.conf")"

# 6. unknown theme
run --theme nonexistent >/dev/null 2>&1 && fail "an unknown theme was accepted"

# 7. a bad template is reported with its file and line, the others are still written
printf 'ok\nbroken=@colors.nope@\n' >"$T/tpl/d.ini.in"
rm -f "$T/out/c.txt"
ERR=$(run --theme mine 2>&1 >/dev/null); RC=$?
[ "$RC" = 1 ] || fail "a bad template should give exit status 1, got $RC"
echo "$ERR" | grep -q "d.ini.in: line 2: unknown theme key 'colors.nope'" || fail "error message: $ERR"
[ -e "$T/out/c.txt" ] || fail "templates after a bad one were not rendered"
[ ! -e "$T/out/d.ini" ] || fail "a bad template produced an output"

# 8. usage error
"$APPLY" --bogus >/dev/null 2>&1; [ $? = 2 ] || fail "bad option should exit with 2"

echo "sfwc-theme-apply tests: OK"
