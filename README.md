# Screen Dim (wldim)

Software screen dimmer for KDE Plasma 6 / Wayland — dims below the hardware
backlight minimum using a click-through black layer-shell overlay.

## Why

Built for the Asus ROG Z13 2025: the laptop panel (amdgpu_bl1) bottoms out at
`/sys/class/backlight/amdgpu_bl1/brightness = 1` which is still too bright in
dark environments. This Plasma 6.7 build has no Night Color "strength"
slider and KWin dropped the wlr gamma-control protocol, so gammastep/wl-gammactl
don't work. A semi-transparent overlay on the `zwlr_layer_shell_v1` OVERLAY layer
with an empty input region is the remaining option.

## How it works

- `wldim.cpp` — single file, two halves:
  - **Overlay thread** (raw C, libwayland): connects to the compositor, creates one
    layer surface per output (anchored top/left/right, `set_size(0, 4000)` so it
    also covers the bottom panel), fills a per-output ARGB shm buffer with
    `alpha = dim*255/100`, repaints on demand via an eventfd.
  - **Qt6 UI** (main thread): QSlider "Brightness" (0–100, left=dim), QCheckBox
    "Autostart" (toggles `~/.config/autostart/com.wurztha.screendim.desktop`),
    embedded SVG window icon (Qt sends it to KWin via xdg-external-image — this is
    why the UI is Qt6, not GTK3: GTK3 cannot send Wayland window icons).
- Config: `~/.config/wldim.conf` (single int 0–90 = dim amount).
- `SIGUSR1` reloads the config (handy for testing without the slider).

## Build

Requires flatpak runtimes: `org.kde.Sdk//6.11` (Qt6 + wayland-scanner) and
`org.freedesktop.Sdk` (fallback). `appimagetool` from
https://github.com/AppImage/appimagetool for the AppImage.

```sh
make            # builds wldim, installs binary + desktop entry + icon to ~/.local
make appimage   # builds Screen-Dim-x86_64.AppImage (~29 MB, bundles Qt6)
make clean
```

Desktop entry and icon sources live in `share/`; `mkappdir.sh` assembles the
AppDir: bundles Qt6 + icu/harfbuzz/freetype/etc.
(excluding glibc/GL/X/glib), and the Qt plugins `platforms/{wayland,xcb}`,
`imageformats/svg`, `iconengines/svgicon`, plus the critical
`wayland-shell-integration/`, `wayland-graphics-integration-client/` and
`wayland-decoration-client/` plugin dirs (missing shell-integration =
"No shell integration named xdg-shell" at runtime).

## Implementation notes

- KWin gives layer surfaces the work area, not the full output, when anchored
  to all 4 edges; anchoring top/left/right + tall size covers the panel too.
- `wp_single_pixel_buffer` + `wp_viewporter` does NOT scale on KWin layer
  surfaces (renders 1x1) — use a real shm buffer sized from the configure event.
- Registry globals arrive in arbitrary order; outputs must be queued until
  compositor/shm/layer_shell are bound (`pending` list in the code).
- The app id must match the `.desktop` filename (`com.wurztha.screendim`) or
  KWin shows a wrong/fallback icon in the taskbar.
- Launching from a terminal inside another app can make KWin borrow the
  launcher's icon; launching via desktop file / systemd-run gives the right one.
