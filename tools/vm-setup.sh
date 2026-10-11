#!/usr/bin/env bash
# One command to get sfwc built and ready in a fresh Debian/Ubuntu machine (a test VM, say).
#
#   git clone https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor sfwc
#   sfwc/tools/vm-setup.sh                 # packages, build, tests, a launcher: ~/start-sfwc
#   sfwc/tools/vm-setup.sh --with-plugins  # also builds the sfwc-plugins repository (private:
#                                          # git asks for your GitHub user name and a token)
#
# Afterwards:  ~/start-sfwc            starts sfwc (nested in a desktop, or on a console)
#              ~/start-sfwc --report   the same through tools/try-sfwc.sh (asks questions, writes a report)
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WITH_PLUGINS=0
[ "${1:-}" = "--with-plugins" ] && WITH_PLUGINS=1

say() { printf '\n==> %s\n' "$*"; }
command -v apt-get >/dev/null || { echo "this script knows apt (Debian/Ubuntu) only; see README 'Build and run'"; exit 1; }

say "installing packages (sudo asks for your password)"
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    git meson ninja-build pkg-config gcc cmake bison flex python3-pip \
    libwayland-dev wayland-protocols libxkbcommon-dev libcairo2-dev libpango1.0-dev \
    libfontconfig-dev fonts-dejavu-core libpixman-1-dev libdrm-dev libinput-dev \
    libudev-dev libseat-dev libegl-dev libgles-dev libgbm-dev libvulkan-dev \
    glslang-tools hwdata libdisplay-info-dev libliftoff-dev \
    libxcb1-dev libxcb-composite0-dev libxcb-icccm4-dev libxcb-render0-dev \
    libxcb-xfixes0-dev libxcb-res0-dev libxcb-ewmh-dev libxcb-dri3-dev \
    libxcb-present-dev libxcb-shm0-dev libxcb-xinput-dev libxcb-render-util0-dev \
    foot seatd wlr-randr wev swaybg

# wlroots 0.20 needs meson >= 1.4; Ubuntu 24.04 has 1.3
need_meson=1
if command -v meson >/dev/null; then
    v="$(meson --version)"
    [ "$(printf '1.4.0\n%s\n' "$v" | sort -V | head -n1)" = "1.4.0" ] && need_meson=0
fi
if [ "$need_meson" = 1 ]; then
    say "installing a newer meson"
    python3 -m pip install --user --break-system-packages "meson>=1.4,<2" 2>/dev/null \
        || python3 -m pip install --user "meson>=1.4,<2"
fi
export PATH="$HOME/.local/bin:$PATH"
grep -q '.local/bin' "$HOME/.bashrc" 2>/dev/null || echo 'export PATH="$HOME/.local/bin:$PATH"' >>"$HOME/.bashrc"
meson --version

say "building sfwc (the first build also builds wlroots: several minutes)"
cd "$ROOT"
[ -d build ] || meson setup build -Dwlroots:examples=false -Dwayland:documentation=false \
    -Dwayland:tests=false -Dwayland:dtd_validation=false
meson compile -C build
say "running the tests (headless)"
meson test -C build --suite sfwc --print-errorlogs || echo "SOME TESTS FAILED (see above); sfwc may still work"

SFWC="$ROOT/build/sfwc"
PLUGIN_PATH=""
if [ "$WITH_PLUGINS" = 1 ]; then
    say "building the plugins (they must share sfwc's wlroots, so sfwc is built again inside that tree)"
    PL="$HOME/sfwc-plugins"
    [ -d "$PL" ] || git clone https://github.com/L0rdr-ln/sfwc-plugins "$PL"
    mkdir -p "$PL/subprojects"
    [ -e "$PL/subprojects/sfwc" ] || git clone --depth 1 https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor "$PL/subprojects/sfwc"
    cd "$PL"
    [ -d build ] || meson setup build -Dwlroots:examples=false -Dwayland:documentation=false \
        -Dwayland:tests=false -Dwayland:dtd_validation=false
    meson compile -C build
    SFWC="$PL/build/subprojects/sfwc/sfwc"
    PLUGIN_PATH="$PL/build"
    mkdir -p "$HOME/.config/sfwc/themes"
    cp "$PL"/themes/*.theme "$HOME/.config/sfwc/themes/"
fi

mkdir -p "$HOME/.config/sfwc"
[ -e "$HOME/.config/sfwc/sfwc.conf" ] || cp "$ROOT/config/sfwc.conf" "$HOME/.config/sfwc/sfwc.conf"

say "writing the launcher ~/start-sfwc"
cat >"$HOME/start-sfwc" <<LAUNCH
#!/usr/bin/env bash
# Starts sfwc (built by tools/vm-setup.sh). Log: ~/sfwc-last.log
export SFWC_TERMINAL=\${SFWC_TERMINAL:-foot}
${PLUGIN_PATH:+export SFWC_PLUGIN_PATH="$PLUGIN_PATH"}
if [ "\${1:-}" = "--report" ]; then
    SFWC_BUILD_DIR="$ROOT/build" exec "$ROOT/tools/try-sfwc.sh" --my-config
fi
"$SFWC" "\$@" 2>&1 | tee "\$HOME/sfwc-last.log"
LAUNCH
chmod +x "$HOME/start-sfwc"

cat <<DONE

Done. Start it with:   ~/start-sfwc
 - in the VM's desktop it opens as a window (nested, the safe first test)
 - on a console (Ctrl+Alt+F3, log in, then run it there) it takes over the screen
Keys: Alt+Return terminal, Alt+Esc quit, Alt+1..4 workspaces, Alt+drag move windows.
Config: ~/.config/sfwc/sfwc.conf   (Alt+Shift+r reloads it)
${PLUGIN_PATH:+Plugins are built: add "[plugins]" "load = blur" (or animations, wobbly) to the config.}
DONE
