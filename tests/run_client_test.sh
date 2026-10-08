#!/usr/bin/env bash
# Starts sfwc on the headless backend, runs the test client against it and checks the
# compositor's log and side effects.
# Usage: run_client_test.sh <sfwc> <client_test> [single|multi]
#   single: one output, input, snapping, live config reload, autostart, terminal
#   multi:  two outputs (different size/scale/position), follow-mouse, output actions,
#           reload-config key (config file watching is switched off)
set -u
SFWC="$1"; CLIENT="$2"; MODE="${3:-single}"

TMP="$(mktemp -d)"
export XDG_RUNTIME_DIR="$TMP"; chmod 700 "$TMP"
export WLR_BACKENDS=headless WLR_RENDERER=pixman SFWC_ENABLE_VIRTUAL_INPUT=1 SFWC_LOG_LEVEL=debug
export ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0
LOG="$TMP/sfwc.log"
export SFWC_CONFIG="$TMP/sfwc.conf"

case "$MODE" in
single)
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

"$SFWC" >"$LOG" 2>&1 &
PID=$!
cleanup() { kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

fail() { echo "FAIL ($MODE): $1"; echo "--- sfwc log ---"; cat "$LOG"; exit 1; }

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
timeout 40 "$CLIENT" "$MODE" || fail "client test failed"
kill -0 "$PID" 2>/dev/null || fail "sfwc died while serving the client"

kill -INT "$PID"
wait "$PID"; STATUS=$?
[ "$STATUS" -eq 0 ] || fail "sfwc exited with status $STATUS after SIGINT"

grep -q "output .* added" "$LOG" || fail "no output was created"
grep -q "window unmapped" "$LOG" || fail "window was never unmapped"

case "$MODE" in
single)
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
