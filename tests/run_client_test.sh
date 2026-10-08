#!/usr/bin/env bash
# Starts sfwc on the headless backend, runs the test client against it and checks the
# compositor's log and side effects.
# Usage: run_client_test.sh <sfwc> <client_test> [single|multi|nested|deco|anim|layers|workspaces]
#   single: one output, input, snapping, live config reload, autostart, terminal
#   multi:  two outputs (different size/scale/position), follow-mouse, output actions,
#           reload-config key (config file watching is switched off)
#   deco:   server-side decorations drawn from a theme: screen captures are checked pixel by
#           pixel, the frame is clicked and dragged, the theme is reloaded live
#   layers: wlr-layer-shell: wallpaper, panel with an exclusive zone, overlay launcher with
#           exclusive keyboard focus
#   anim:   open/close/move animations (2 s, linear) checked with screen captures, then
#           switched off by a live config reload
#   nested: the single scenario, but sfwc runs with the wayland backend as a window of a
#           second (headless) sfwc, like `./sfwc` started inside another Wayland session
set -u
SFWC="$1"; CLIENT="$2"; MODE="${3:-single}"

TMP="$(mktemp -d)"
export XDG_RUNTIME_DIR="$TMP"; chmod 700 "$TMP"
export WLR_BACKENDS=headless WLR_RENDERER=pixman SFWC_ENABLE_VIRTUAL_INPUT=1 SFWC_LOG_LEVEL=debug
export ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0
# animations would make pixel checks timing dependent; only the anim mode wants them
export SFWC_NO_ANIMATIONS=1
LOG="$TMP/sfwc.log"
export SFWC_CONFIG="$TMP/sfwc.conf"

CLIENT_MODE="$MODE"
case "$MODE" in
single|nested)
    # One deliberately invalid line (4) which must be reported with its line number and fall
    # back to the default, a keyboard repeat setting, plus autostart with expansions.
    cat >"$SFWC_CONFIG" <<CONF
[general]
terminal = touch $TMP/terminal-ran
[windows]
snap_distance = banana
[keyboard]
repeat_rate = 33
repeat_delay = 250
[autostart]
exec = touch \$runtime/autostart-ran
exec = echo "\$terminal \$theme" > \$runtime/autostart-expanded
CONF
    ;;
layers|workspaces)
    : >"$SFWC_CONFIG"
    ;;
anim)
    unset SFWC_NO_ANIMATIONS
    cat >"$SFWC_CONFIG" <<'CONF'
[animations]
enabled = true
open = fade
close = fade
move = true
duration_ms = 2000
easing = linear
CONF
    ;;
deco)
    # Theme with distinct colors; shadow is white so that it is visible on the black background.
    mkdir -p "$TMP/themes"
    cat >"$TMP/themes/test.theme" <<'THEME'
format = 1
name = Test
[colors]
background = #101010
border_focused = #ff0000
border_unfocused = #0000ff
titlebar_focused = #00ff00
titlebar_unfocused = #ffff00
title_text = #ffffff
close_button = #ff00ff
maximize_button = #00ffff
minimize_button = #ff8000
[geometry]
border_width = 4
titlebar_height = 24
corner_radius = 10
button_size = 12
button_spacing = 6
[shadow]
enabled = true
radius = 20
offset_y = 6
color = #ffffff80
[font]
family = sans
size = 10
THEME
    sed -e 's/#ff0000/#123456/' -e 's/#00ff00/#abcdef/' "$TMP/themes/test.theme" >"$TMP/themes/test2.theme"
    printf '[general]\ntheme = test\n' >"$SFWC_CONFIG"
    if fc-list 2>/dev/null | grep -q .; then export SFWC_TEST_FONTS=1; fi
    ;;
multi)
    # HEADLESS-2 is added by SFWC_TEST_OUTPUTS: 1024x600, scale 2 (logical 512x300), placed
    # below the first output. Both outputs are positioned explicitly because the order in
    # which they appear is not defined. The layout name does not exist: the keymap must fall back.
    export SFWC_TEST_OUTPUTS=1024x600 SFWC_NO_CONFIG_WATCH=1
    cat >"$SFWC_CONFIG" <<'CONF'
[general]
focus = follow-mouse
[keyboard]
layout = nosuchlayout
repeat_rate = 40
repeat_delay = 300
[output:HEADLESS-1]
position = 0,0
[output:HEADLESS-2]
scale = 2
position = 0,720
[keybinds]
$mod+f = toggle-maximize
$mod+o = move-to-next-output
$mod+Shift+o = focus-next-output
$mod+Shift+r = reload-config
CONF
    ;;
*)
    echo "unknown mode $MODE"; exit 2 ;;
esac
[ "$MODE" = nested ] && CLIENT_MODE=single

PID=""
OUTER_PID=""
cleanup() { kill -9 $PID $OUTER_PID 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() {
    echo "FAIL ($MODE): $1"
    echo "--- sfwc log ---"; cat "$LOG"
    [ -n "$OUTER_PID" ] && { echo "--- outer sfwc log ---"; cat "$TMP/outer.log"; }
    exit 1
}

if [ "$MODE" = nested ]; then
    # Outer compositor: headless, default config, no virtual input.
    SFWC_CONFIG="$TMP/outer.conf" SFWC_ENABLE_VIRTUAL_INPUT=0 "$SFWC" >"$TMP/outer.log" 2>&1 &
    OUTER_PID=$!
    OUTER_SOCKET=""
    for _ in $(seq 1 50); do
        OUTER_SOCKET="$(sed -n 's/.*WAYLAND_DISPLAY=\(.*\)$/\1/p' "$TMP/outer.log" | head -n1)"
        [ -n "$OUTER_SOCKET" ] && break
        kill -0 "$OUTER_PID" 2>/dev/null || { PID=$OUTER_PID; LOG="$TMP/outer.log"; fail "outer sfwc exited during startup"; }
        sleep 0.1
    done
    [ -n "$OUTER_SOCKET" ] || { PID=$OUTER_PID; LOG="$TMP/outer.log"; fail "outer sfwc did not start"; }
    # The compositor under test uses the wayland backend and shows up as a window of the outer one.
    export WLR_BACKENDS=wayland WAYLAND_DISPLAY="$OUTER_SOCKET"
fi

"$SFWC" >"$LOG" 2>&1 &
PID=$!

# wait for the compositor to announce its socket
SOCKET=""
for _ in $(seq 1 50); do
    SOCKET="$(sed -n 's/.*WAYLAND_DISPLAY=\(.*\)$/\1/p' "$LOG" | head -n1)"
    [ -n "$SOCKET" ] && break
    kill -0 "$PID" 2>/dev/null || fail "sfwc exited during startup"
    sleep 0.1
done
[ -n "$SOCKET" ] || fail "sfwc did not start within 5s"

export WAYLAND_DISPLAY="$SOCKET"
timeout 60 "$CLIENT" "$CLIENT_MODE" || fail "client test failed"
kill -0 "$PID" 2>/dev/null || fail "sfwc died while serving the client"

kill -INT "$PID"
wait "$PID"; STATUS=$?
[ "$STATUS" -eq 0 ] || fail "sfwc exited with status $STATUS after SIGINT"
if [ -n "$OUTER_PID" ]; then
    kill -INT "$OUTER_PID"
    wait "$OUTER_PID"; OUTER_STATUS=$?
    [ "$OUTER_STATUS" -eq 0 ] || fail "outer sfwc exited with status $OUTER_STATUS after SIGINT"
    grep -q "window mapped" "$TMP/outer.log" || fail "the nested compositor never appeared as a window of the outer one"
fi

grep -q "output .* added" "$LOG" || fail "no output was created"
grep -q "window unmapped" "$LOG" || fail "window was never unmapped"

case "$MODE" in
workspaces)
    grep -q "workspace 2" "$LOG" || fail "the compositor never switched workspace"
    ;;
layers)
    grep -q "layer surface mapped: namespace=bar" "$LOG" || fail "the panel was never mapped"
    grep -q "layer surface unmapped: namespace=launcher" "$LOG" || fail "the launcher was never unmapped"
    ;;
anim)
    grep -q "window mapped.*animated" "$LOG" || fail "window was never mapped"
    ;;
deco)
    grep -q "window mapped.*decorated window" "$LOG" || fail "window was never mapped"
    [ "$(grep -c 'theme loaded' "$LOG")" -ge 2 ] || fail "theme was not loaded at startup and again on reload"
    grep -q "theme .* not found" "$LOG" && fail "a theme was not found"
    ;;
single|nested)
    grep -q "window mapped.*sfwc-test-window" "$LOG" || fail "window was never mapped"
    grep -q "sfwc.conf:4: snap_distance" "$LOG" || fail "invalid config line was not reported with its line number"
    [ "$(grep -c 'config loaded' "$LOG")" -ge 2 ] || fail "config was not loaded at startup and again on live reload"
    for _ in $(seq 1 20); do [ -e "$TMP/autostart-expanded" ] && [ -e "$TMP/terminal-ran" ] && break; sleep 0.1; done
    [ -e "$TMP/autostart-ran" ] || fail "autostart command did not run"
    [ "$(cat "$TMP/autostart-expanded" 2>/dev/null)" = "touch $TMP/terminal-ran default" ] || fail "autostart \$terminal/\$theme/\$runtime were not expanded"
    [ -e "$TMP/terminal-ran" ] || fail "Alt+Return did not run the configured terminal"
    grep -q "window minimized" "$LOG" || fail "minimize request was not handled"
    grep -q "window restored" "$LOG" || fail "restore (Alt+Shift+m) was not handled"
    ;;
multi)
    grep -q "window mapped.*sfwc-multi-A" "$LOG" || fail "window A was never mapped"
    grep -q "window mapped.*sfwc-multi-B" "$LOG" || fail "window B was never mapped"
    grep -q "output HEADLESS-2 added" "$LOG" || fail "second output was not created"
    grep -q "cannot compile keymap" "$LOG" || fail "bad keyboard layout was not reported (no fallback?)"
    [ "$(grep -c 'config loaded' "$LOG")" -ge 2 ] || fail "reload-config key did not reload the config"
    ;;
esac
echo "client test ($MODE) passed"
