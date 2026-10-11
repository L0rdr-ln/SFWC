# Testing sfwc on real hardware

CI only runs sfwc headless, so real machines (GPUs, monitors, touchpads, console sessions) are
untested. Your reports are the most useful thing right now. This takes about five minutes.

## The easy way

```sh
git clone https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor
cd Simple-Floating-Wayland-Compositor
tools/try-sfwc.sh
```

The script builds sfwc if needed (the first build also builds wlroots and takes a few minutes;
install the packages from the README's *Build and run* section first), starts it with a
terminal, and when you quit it asks eleven yes/no questions and writes a report to
`~/sfwc-report-<time>.txt`. Read the report, then paste it into a
[new issue](https://github.com/L0rdr-ln/Simple-Floating-Wayland-Compositor/issues/new/choose).

It starts with the example config, not yours (`--my-config` uses yours).

## Two levels, safest first

1. **Nested** – run the script inside your normal desktop. sfwc opens as a window. This tests
   rendering and input through your real GPU and cannot hurt your session.
2. **On the console** – the real test. Press **Ctrl+Alt+F3**, log in, run the script there
   (it detects that no desktop is running). You need seat access: systemd-logind gives it to the
   logged-in console user; otherwise run `seatd` and join its group. Do not start it over SSH.

A virtual machine (QEMU with virtio-gpu) also works for the console level; it checks the DRM
path but not real GPU quirks.

## In a virtual machine

A VM is a good place for the first real run (GPU path, real input stack, console session).
With QEMU: `-device virtio-vga-gl -display gtk,gl=on` (or `virtio-gpu-pci`), a USB tablet or mouse
and keyboard, `-smp 2 -m 4G`. Inside the guest install the packages from the README, then run
`tools/try-sfwc.sh` on the console (Ctrl+Alt+F3), or from the guest's desktop for the nested level.
Add a second virtual monitor with `-device virtio-vga,max_outputs=2` to try multi-monitor.

Things worth trying once it runs (each line is one thing to look at; the tools are in most
distributions):

| What | How | Should |
|---|---|---|
| outputs | `wlr-randr`, then `wlr-randr --output NAME --scale 1.5`, `--off`, `--on`, `--pos 1920,0` | every change is applied; windows leave an output that is switched off; the last output cannot be switched off |
| key events | `wev` | keys, pointer and relative motion are reported; a click focuses |
| cursor shapes | open a text field in `foot`/a browser | the I-beam appears over text |
| fractional scale | `wlr-randr --output NAME --scale 1.5`, start `foot` | the text stays sharp (no blurry upscaling) |
| workspaces in a bar | `waybar` with the `ext/workspaces` module | the bar lists 1-4 and clicking switches |
| night light | `gammastep -O 3500` (or `wlsunset`) | the screen turns warm; `gammastep -x` resets it |
| screen off/on | `wlopm --off '*'`, then any key / `wlopm --on '*'` | the screen goes dark and comes back |
| idle | `swayidle -w timeout 5 'wlopm --off "*"' resume 'wlopm --on "*"'` | dark after 5 s, back on input |
| pointer lock | a game or `wev`-like tool that locks the pointer (e.g. a Minecraft/SDL3 game) | the cursor stays, the view turns |
| rules | add `[rule:foot]` with `workspace = 2` to the config, reload | the next `foot` opens on workspace 2 |
| touchpad | `[input:touchpad]` with `tap = true`, `natural_scroll = true`, reload (`Alt+Shift+r`) | the settings apply without restart |
| blur | install the `blur` plugin (sfwc-plugins), a wallpaper with `swaybg -i FILE`, a translucent terminal | the wallpaper behind the terminal is frosted |

Anything that does not behave: attach the report of `tools/try-sfwc.sh` (the log has the
protocol requests at debug level: `SFWC_LOG_LEVEL=debug`).

## Getting out

- **Alt+Esc** quits sfwc.
- **Ctrl+Alt+F1…F12** switches to another console (untested on hardware, so also keep SSH ready).
- From another machine: `ssh you@host pkill sfwc`.

## Keys

Alt+Return terminal · Alt+drag move · Alt+right-drag resize · Alt+f maximize · Alt+m minimize ·
Alt+1…4 workspaces · Alt+Esc quit. All of them are in the [README](../README.md#default-keys).

## If something is wrong

| Problem | Try |
|---|---|
| cursor invisible or corrupted | `WLR_NO_HARDWARE_CURSORS=1 tools/try-sfwc.sh` |
| wrong GPU used | `WLR_DRM_DEVICES=/dev/dri/card1 tools/try-sfwc.sh` |
| black screen or crash, to rule out the GPU renderer | `WLR_RENDERER=pixman tools/try-sfwc.sh` |
| no terminal opens | `SFWC_TERMINAL=xterm tools/try-sfwc.sh` |

Environment variables are listed in `man sfwc`. The report contains the log, the environment
variables starting with `WLR_`/`SFWC_`, your distribution, kernel and GPU, and your answers.
