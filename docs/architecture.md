# Architecture

purpwm+ is a native C11 Wayland compositor built directly on `libwayland`, EGL/GLESv2, and
Linux display/input libraries. It does **not** use Smithay, winit, Rust, or a language runtime.
The compositor has one main event loop, with narrowly scoped helper threads for work such as
PAM conversations, D-Bus/status operations, and housekeeping.

## Processes

| Executable | Implementation | Responsibility |
| :--- | :--- | :--- |
| `build/purpwm+` | `src/*.c`, generated protocol code | Wayland server, backends, rendering, tiling, input, UI, notifications, locking, capture, and IPC |
| `build/purpwm+-polkit-agent` | `tools/purpwm-plus-polkit-agent.c` | GTK 4 authentication UI and `polkit-agent-1` registration |
| `build/purpwm+-portal` | `tools/purpwm-plus-portal.c` | GDBus Screenshot portal backend, consent dialog, and PNG delivery |
| `build/purpwm+-keyring` | `tools/purpwm-plus-keyring.c` | libsecret operations and `gnome-keyring-daemon` startup/environment import |

The three companions are separate native processes. They communicate through the system or
session D-Bus and, for screenshots, a private compositor capture socket. None requires Python
at runtime.

## Backends

The backend-independent window manager and UI sit behind the interface in
[`include/host_backend.h`](../include/host_backend.h).

### Native DRM/KMS — `--drm`

[`src/backend_drm.c`](../src/backend_drm.c) opens the GPU and input devices through `libseat`,
uses `libdrm` and `gbm` for modesetting and buffer allocation, `libudev` for device/hotplug
events, and `libinput` for keyboard, pointer, touch, and tablet input. Connected outputs have
their own hardware mode, scale, workspace state, and VBlank-driven presentation.

This is the normal backend for a display-manager or TTY login. It requires an active logind
session or a working `seatd` setup and permission to access the DRM and input devices.

### Nested Wayland — `--winit` or `--nested`

[`src/backend_wayland.c`](../src/backend_wayland.c) is a Wayland **client** of an existing
compositor and a Wayland **server** for purpwm+ clients. It uses EGL/GLESv2 and the generated
`xdg-shell` client bindings. The two spellings select the same native nested backend; the
`--winit` name is retained for command-line compatibility, but there is no winit dependency.

This backend is suitable for development and for running under gamescope. Virtual outputs can
be described with `PURPWM_VIRTUAL_MONITORS`; see
[Configuration](configuration.md#nested-virtual-monitors).

### Headless — `--headless`

The headless route runs the compositor without a DRM device or a host Wayland window. It keeps
the server, window-management, and IPC paths available without requiring a visible session,
which is useful for automation and diagnostics.

## Source map

| File | Responsibility |
| :--- | :--- |
| `src/main.c` | CLI selection, compositor lifecycle, event-loop/backend routing, and headless operation |
| `src/server.c` | Wayland display/global setup and shared server state |
| `src/surface.c` | `wl_surface`, subsurfaces, buffer/commit state, and surface-tree integration |
| `src/xdg_shell.c` | XDG toplevels, popups, grabs, and window lifecycle |
| `src/layer_shell.c` | `wlr-layer-shell` background, bottom, top, and overlay surfaces |
| `src/seat_data.c` | Seat capabilities, clipboard/data control, primary selection, and drag-and-drop data |
| `src/backend_drm.c` | DRM/KMS, GBM, libseat, libinput, udev, and native outputs |
| `src/backend_wayland.c` | Nested Wayland host connection, host surfaces, and host input |
| `src/render.c` | Per-output GLESv2 scene rendering, damage, transforms, and readback |
| `src/wm.c` | Tag masks, workspaces, focus/stack state, five layouts, and multi-monitor placement |
| `src/input.c` | Keybindings, solo-`Mod` tap, pointer/touch routing, bar actions, and interactive move/resize |
| `src/glide.c` | Accelerating held-arrow resize motion |
| `src/ipc.c` | Non-blocking JSON-line monitor IPC and `monitors.json` persistence |
| `src/capture.c` | Private, bounded framebuffer-capture socket used by the portal |
| `src/cursor.c` | XCursor theme loading and built-in purple cursor fallback |
| `src/wallpaper.c` | Cached, center-cropped per-output wallpaper images |
| `src/font.c` | FreeType 2 glyph rasterization, Fontconfig lookup, and embedded-font fallback |
| `src/bar.c` | Software-rasterized status bar, metrics, pills, and hit testing |
| `src/lock.c` | Full-screen lock UI and worker-thread PAM conversation |
| `src/notify.c` | `org.freedesktop.Notifications` GDBus service and toast model/rasterization |
| `src/help.c` | `Mod + h` multi-column keybinding overlay |

Public types and interfaces live in `include/`. User defaults are centralized in
[`include/config.h`](../include/config.h); server-wide structures are in `include/purpwm.h`,
renderer interfaces in `include/render.h`, and backend-neutral types in the pure headers.

## Generated code and header layering

`make protocols` runs `wayland-scanner` on the XML files bundled in `protocols/`:

- every protocol gets a server header and private marshalling implementation;
- `xdg-shell` additionally gets a client header for the nested backend;
- outputs are written below `build/protocols/`, never into the source directories.

The same target converts `assets/fonts/DejaVuSansMono.ttf` into
`build/embedded_font.c`. FreeType can therefore always rasterize the UI even if Fontconfig
finds no usable system font. Python is used only by this build-time conversion; it is not
embedded or invoked by the installed session.

`make check-headers` enforces two dependency layers:

- **Pure headers** — `config.h`, `types.h`, `wm.h`, `ipc.h`, `ui.h`, and `host_backend.h` —
  compile standalone without including Wayland client or server headers.
- **Server headers** — `purpwm.h` and `render.h` — use `wayland-server-core.h` and generated
  server protocol declarations.

It also compiles a nested client-side include combination. This prevents accidental
client/server symbol collisions and lets the deterministic tests link pure modules without
Wayland, EGL, or DRM.

## Wayland protocols

In addition to core `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_seat`, `wl_output`, and
data-device objects, the bundled/generated interfaces cover:

- `xdg-shell`, `xdg-decoration-unstable-v1`, `xdg-activation-v1`, and
  `xdg-output-unstable-v1`;
- `wlr-layer-shell-unstable-v1`, `wlr-data-control-unstable-v1`, and
  `primary-selection-unstable-v1`;
- `idle-inhibit-unstable-v1` and `linux-dmabuf-v1`;
- `viewporter`, `fractional-scale-v1`, and `single-pixel-buffer-v1`;
- `cursor-shape-v1` and `tablet-v2`.

The compositor uses server-side XDG decorations. Clipboard, primary selection, drag-and-drop
icons, touch, popups, layer surfaces, and fractional scaling are handled directly rather than
through an external desktop daemon.

## Event loop and rendering

Conceptually, each iteration performs the following work:

1. Dispatch backend input/output events and Wayland client requests.
2. Poll monitor IPC, capture requests, signals, status changes, notification/D-Bus work, and
   completed PAM operations.
3. Apply commits, focus/tag/layout changes, output hotplug or mode changes, and active
   move/resize/glide updates.
4. Relayout damaged outputs and render only outputs whose damage requires a new frame.
5. Queue or present native/nested buffers and send frame callbacks to visible surfaces.
   Hidden tags and the locked session do not keep clients rendering.

Idle iterations sleep briefly instead of spinning. Long-lived glyph, wallpaper, bar, and other
UI resources are cached. A periodic helper releases unused C-library heap back to the system.

All window-management coordinates are **logical pixels**. Output scale converts them to
physical pixels for rendering. Fractional-scale-aware clients receive their exact scale;
other clients use integer buffer scaling with compositor sampling. The bar, help, lock, and
notification text is rasterized at output resolution so it remains sharp.

The stacking model places fullscreen windows above ordinary tiled windows, with floating and
scratchpad windows above tiled ones. Popups and high-priority layer surfaces sit above window
content; background/bottom layers remain below. Rendering and pointer/touch hit testing use
the same window ordering. While locked, client focus is withdrawn and the fullscreen PAM UI is
the only interactive surface.

## Monitor IPC and screenshot capture

The monitor server is non-blocking and uses one JSON object per line. Its socket is exported
as `PURPWM_IPC_SOCKET`; display changes are validated, applied to live outputs, and persisted
to `monitors.json`. The compositor also maintains convenient runtime-directory symlinks for
Spotlight and older purpwm clients. See [Configuration](configuration.md#monitor-ipc).

The portal uses a separate socket named
`$XDG_RUNTIME_DIR/purpwm+-capture-$WAYLAND_DISPLAY.sock` (with
`PURPWM_CAPTURE_SOCKET` as an override). The backend asks for consent, receives a PNG frame,
and returns a private `file://` URI through `org.freedesktop.impl.portal.Screenshot` version 2.
Only that portal frontend may drive the backend. Lock-screen policy is enforced by the
compositor capture path.

## Tests

`make test` builds and runs `build/test_purpwm_plus`. The test executable links pure modules
such as tiling, glide, IPC, cursor, wallpaper, font, bar, lock, notification, and help code;
it does not need a running Wayland compositor or a DRM device.

Use `make check` to run both those tests and the header-layer checks. Build/test commands do
not start a session or modify login configuration.
