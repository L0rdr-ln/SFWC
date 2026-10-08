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
terminal, and when you quit it asks nine yes/no questions and writes a report to
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
