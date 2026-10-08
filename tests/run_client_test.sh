#!/usr/bin/env bash
# Starts sfwc on the headless backend, runs the test client against it and
# checks the compositor's log. Usage: run_client_test.sh <sfwc> <client_test>
set -u
SFWC="$1"; CLIENT="$2"

TMP="$(mktemp -d)"
export XDG_RUNTIME_DIR="$TMP"; chmod 700 "$TMP"
export WLR_BACKENDS=headless WLR_RENDERER=pixman SFWC_ENABLE_VIRTUAL_INPUT=1
export ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0
LOG="$TMP/sfwc.log"

# Config under test: one deliberately invalid line (4) which must be reported with its
# line number and fall back to the default, plus autostart with expansions.
export SFWC_CONFIG="$TMP/sfwc.conf"
cat >"$SFWC_CONFIG" <<'CONF'
[general]
terminal = true
[windows]
snap_distance = banana
[autostart]
exec = touch $runtime/autostart-ran
exec = echo "$terminal $theme" > $runtime/autostart-expanded
CONF

"$SFWC" >"$LOG" 2>&1 &
PID=$!
cleanup() { kill -9 "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

fail() { echo "FAIL: $1"; echo "--- sfwc log ---"; cat "$LOG"; exit 1; }

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
timeout 20 "$CLIENT" || fail "client test failed"
kill -0 "$PID" 2>/dev/null || fail "sfwc died while serving the client"

kill -INT "$PID"
wait "$PID"; STATUS=$?
[ "$STATUS" -eq 0 ] || fail "sfwc exited with status $STATUS after SIGINT"

grep -q "sfwc.conf:4: snap_distance" "$LOG" || fail "invalid config line was not reported with its line number"
[ "$(grep -c 'config loaded' "$LOG")" -ge 2 ] || fail "config was not loaded at startup and again on live reload"
for _ in $(seq 1 20); do [ -e "$TMP/autostart-expanded" ] && break; sleep 0.1; done
[ -e "$TMP/autostart-ran" ] || fail "autostart command did not run"
[ "$(cat "$TMP/autostart-expanded" 2>/dev/null)" = "true default" ] || fail "autostart \$terminal/\$theme/\$runtime were not expanded"
grep -q "output .* added" "$LOG"    || fail "no output was created"
grep -q "window mapped.*sfwc-test-window" "$LOG" || fail "window was never mapped"
grep -q "window minimized" "$LOG"   || fail "minimize request was not handled"
grep -q "window restored" "$LOG"    || fail "restore (Alt+Shift+m) was not handled"
grep -q "window unmapped" "$LOG"    || fail "window was never unmapped"
echo "client test passed"
