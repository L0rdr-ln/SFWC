#!/usr/bin/env bash
# Try sfwc the easy way and collect a report you can paste into an issue.
#
#   tools/try-sfwc.sh              # builds if needed, then starts sfwc (nested in your desktop,
#                                  # or on the console if you are on a TTY)
#   tools/try-sfwc.sh --my-config  # use your ~/.config/sfwc/sfwc.conf instead of the example one
#   tools/try-sfwc.sh --report-only LOGFILE   # just make the report from an existing log
#
# Environment: SFWC_BUILD_DIR (default ./build), SFWC_TERMINAL (terminal to start inside sfwc)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${SFWC_BUILD_DIR:-$ROOT/build}"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$HOME/sfwc-test-$STAMP.log"
REPORT="$HOME/sfwc-report-$STAMP.txt"
USE_MY_CONFIG=0
REPORT_ONLY=""

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
    --my-config) USE_MY_CONFIG=1 ;;
    --report-only) shift; REPORT_ONLY="${1:-}"; [ -r "$REPORT_ONLY" ] || die "--report-only needs a readable log file" ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option '$1' (try --help)" ;;
    esac
    shift
done

# ------------------------------------------------------------------ report
ANSWERS=""
ask() { # ask "question" -> y / n / s (skipped)
    local a
    while true; do
        { read -r -p "  $1 [y/n/s=skip] " a </dev/tty; } 2>/dev/null || a=s
        case "$a" in y|Y) ANSWERS+="  [yes ] $1"$'\n'; return ;; n|N) ANSWERS+="  [NO  ] $1"$'\n'; return ;;
                     s|S|"") ANSWERS+="  [skip] $1"$'\n'; return ;; esac
    done
}

make_report() { # make_report LOGFILE MODE EXITCODE
    local log="$1" mode="$2" code="$3"
    {
        say "sfwc test report ($STAMP)"
        say "  commit:   $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown) ($(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo ?))"
        say "  mode:     $mode, exit status $code"
        say "  os:       $(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}")"
        say "  kernel:   $(uname -r)"
        say "  session:  XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-} XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-}"
        say "  env:      $(env | grep -E '^(WLR_|SFWC_)' | tr '\n' ' ')"
        say "  /dev/dri: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
        if command -v lspci >/dev/null 2>&1; then
            say "  gpu:"
            lspci -nnk 2>/dev/null | grep -A3 -Ei 'vga|3d controller|display controller' | sed 's/^/    /'
        fi
        say ""
        say "What worked (your answers):"
        printf '%s' "${ANSWERS:-  (not asked)\n}"
        say ""
        say "Outputs and backend seen in the log:"
        grep -E 'output .* added|backend|renderer|sfwc running' "$log" 2>/dev/null | head -12 | sed 's/^/  /'
        say ""
        say "Errors and warnings:"
        grep -E '\[ERROR\]|\[WARN' "$log" 2>/dev/null | head -40 | sed 's/^/  /'
        say ""
        say "Last 60 lines of the log:"
        tail -n 60 "$log" 2>/dev/null | sed 's/^/  /'
    } >"$REPORT"
    say ""
    say "Report written to: $REPORT"
    say "Full log:          $log"
    say "Please read the report before you post it (paths may contain your user name), then"
    say "paste it into a new issue: https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor/issues/new/choose"
}

if [ -n "$REPORT_ONLY" ]; then
    make_report "$REPORT_ONLY" "unknown" "?"
    exit 0
fi

# ------------------------------------------------------------------- build
if [ ! -x "$BUILD/sfwc" ]; then
    command -v meson >/dev/null 2>&1 || die "meson is not installed. Install the build tools and libraries listed in the README (Build and run), then run this again."
    say "Building sfwc in $BUILD (the first time this also builds wlroots and takes a few minutes)..."
    (cd "$ROOT" && { [ -d "$BUILD" ] || meson setup "$BUILD"; } && meson compile -C "$BUILD") || die "the build failed; the messages above say which library is missing"
fi

# -------------------------------------------------------------------- mode
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then
    MODE=nested
else
    MODE=tty
fi

TERM_CMD="${SFWC_TERMINAL:-}"
if [ -z "$TERM_CMD" ]; then
    for t in foot alacritty kitty wezterm xterm konsole gnome-terminal xfce4-terminal; do
        if command -v "$t" >/dev/null 2>&1; then TERM_CMD="$t"; break; fi
    done
fi
[ -n "$TERM_CMD" ] || say "warning: no terminal program found (install foot or xterm): you will see an empty desktop."

# config: the shipped example by default, so that your own settings cannot get in the way
if [ "$USE_MY_CONFIG" = 0 ]; then
    TMPCFG="$(mktemp -d)"
    cp "$ROOT/config/sfwc.conf" "$TMPCFG/sfwc.conf"
    export SFWC_CONFIG="$TMPCFG/sfwc.conf"
fi
export SFWC_LOG_LEVEL=debug

say "Starting sfwc ($MODE mode). Log: $LOG"
say "  Alt+Return = terminal   Alt+Esc = quit   Alt+drag = move/resize   Alt+1..4 = workspaces"
if [ "$MODE" = tty ]; then
    say "  Console mode: Ctrl+Alt+F1..F12 switches VT. If the screen hangs, log in over SSH and run: pkill sfwc"
    say "  Starting in 5 seconds (Ctrl+C to cancel)..."
    sleep 5
fi

"$BUILD/sfwc" ${TERM_CMD:+-s "$TERM_CMD"} 2>"$LOG"
CODE=$?

say ""
say "sfwc exited with status $CODE. A few quick questions (y = worked, n = broken, s = did not try):"
ask "Did the screen come up and the cursor move?"
ask "Could you open a terminal and type in it (keyboard layout correct)?"
ask "Could you move and resize windows with Alt+drag and the title bar?"
ask "Did maximize, minimize and the title bar buttons work?"
ask "Did Alt+1 / Alt+2 switch workspaces?"
ask "Did a second monitor work (if you have one)?"
ask "Did touchpad or mouse scrolling and clicking work?"
ask "Did animations look smooth (no flicker or tearing)?"
ask "Did quitting with Alt+Esc leave the screen / console in order?"
make_report "$LOG" "$MODE" "$CODE"
exit 0
