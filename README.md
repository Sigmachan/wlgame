# wlgame

A gaming-focused Wayland compositor built on **wlroots-0.21**, usable two ways:

- **Per-game wrapper** — `wlgame [opts] -- %command%`, like `gamescope -- %command%`.
  Launches one game, forwards the display, and exits with the game's exit code
  when it quits. Drop it straight into Steam launch options.
- **Standalone session** — boot into it from a display manager (`wlgame.desktop`).

It auto-tunes the environment per GPU (NVIDIA NVAPI/DLSS/RTX-SM, AMD RADV/ReBAR),
enables the gaming-relevant Wayland protocols (tearing-control, content-type,
color-management HDR, explicit DRM sync), and ships FSR1 / NIS / CAS Vulkan
post-process upscalers.

## Build

```sh
meson setup build
ninja -C build
sudo ninja -C build install      # installs binary, shaders, wayland-session
```

Runtime deps: `wlroots>=0.21`, `wayland`, `xkbcommon`, `libdrm`, `vulkan`,
`glslang` (build-time, for shaders), optional `xwayland` + `xcb` (X11 games),
optional `mangohud` (for `--mangoapp`).

## Usage

```
wlgame [options] [-- command [args...]]

Output / display:
  -o, --output WxH[@Hz]      Output mode (DRM) or window size (nested)
  -f, --fullscreen           Request a fullscreen window when nested
      --fps <N>              Cap presentation/frame-callbacks to N Hz
  -t, --tearing              Allow tearing (async page-flips) for games

Scaling:
  -F, --filter <mode>        Upscaler: none|cas|nis|fsr1 (default: auto)
  -s, --scale <mode>         Alias of --filter
      --sharpness <0.0-1.0>  Sharpening intensity (default: 0.8)
      --shader-dir <path>    Override SPIR-V shader directory

Game integration:
  -m, --mangoapp             Auto-spawn the mangoapp performance overlay
      --prefer-wayland       Hint Proton/SDL/Qt to use native Wayland

Misc:
  -d, --debug                Debug logging
  -h, --help                 Help
```

### Examples

```sh
# Steam launch options — render at the display's native mode with FSR1 sharpening:
wlgame -o 3840x2160@120 -F fsr1 -m -- %command%

# Nested inside your desktop, 1440p window, native-Wayland Proton:
wlgame -o 2560x1440 --prefer-wayland -- %command%

# Frame-cap a title to 116 fps to stay inside a 120 Hz VRR window:
wlgame --fps 116 -- %command%
```

When a `-- command` is given, wlgame launches it once the compositor socket is
live, exports `WAYLAND_DISPLAY` and (if XWayland is built) `DISPLAY`, and tracks
it via `SIGCHLD`: when the game exits, the compositor shuts down and returns the
game's exit code. `Super+Q` quits; `Alt+F4` closes the focused window.

## GPU auto-tuning

- **NVIDIA** — Vulkan renderer, `PROTON_ENABLE_NVAPI`/`DXVK_ENABLE_NVAPI` (DLSS,
  RTX, Reflex), NGX updater, DLDSR ×2.25, RTX Smooth Motion, nvidia VA-API.
- **AMD** — RADV with `sam,ngg,nggc,rebar`, DXVK async pipelines, Mesa GL 4.6.
- **Intel** — Vulkan renderer, iHD VA-API.

## Status & roadmap

Working: per-game wrapper with exit-code/signal propagation, nested + DRM
backends, output-mode selection, fps cap, FSR1/NIS/CAS post-process upscaling,
HDR signalling via color-management-v1 (parametric: PQ / BT.2020 / DCI-P3),
tearing, VRR auto-enable, layer-shell overlays, XWayland, explicit DRM sync.

Not yet (the remaining gamescope-parity gap): a **virtual second output** so the
game renders at an internal resolution decoupled from the scanout mode (the
true "render-low → FSR-upscale → present" path). wlroots' custom-swapchain
requires matching the output size, so this needs a dedicated render output —
scoped as the next milestone. Today, `-o` sets the real output mode and the
upscalers run as a post-process sharpen/scale on the composited frame.
