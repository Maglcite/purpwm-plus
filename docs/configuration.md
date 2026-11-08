# Configuration

purpwm+ follows `dwm`'s configuration model: persistent program defaults live in
[`include/config.h`](../include/config.h), while session and display settings can change at
runtime. There is no Rust `config.rs` and no Python configuration layer.

After editing `include/config.h`, rebuild and restart the compositor:

```sh
make -j"$(nproc)"
```

For an installed session, reinstall the updated binaries with `./install.sh --no-build` (or
`make install` if you use only the binary install target), then log out and back in. A nested
instance can simply be restarted from `build/purpwm+`.

## Session settings (`session.conf`)

`start-purpwm+` sources shell assignments from the installation's
`share/purpwm+/session.conf`, then the per-user
`${XDG_CONFIG_HOME:-~/.config}/purpwm+/session.conf` when that is a different file. It then
also sources legacy `${XDG_CONFIG_HOME:-~/.config}/purpwm/session.conf` if readable.
`PURPWM_SESSION_CONFIG` selects a different installation defaults file.

Generated defaults fill variables that are unset or empty, so existing environment values can
override them. Sourcing is cumulative: a later explicit assignment wins, including one left in
the legacy file. Remove or migrate stale legacy assignments after upgrading. When running
`build/purpwm+` directly rather than through the launcher, set the desired variables yourself.

Example user `session.conf`:

```sh
export PURPWM_TERMINAL=/usr/bin/foot
export PURPWM_SPOTLIGHT="$HOME/apps/spotlight"
export PURPWM_DEFAULT_SCALE=1.5
export PURPWM_REFRESH_RATE=144
export PURPWM_WALLPAPER="$HOME/Pictures/wallpaper.jpg"
```

`PURPWM_TERMINAL` and `PURPWM_SPOTLIGHT` accept one executable name or path, not a shell
command line. `auto` uses normal detection, and `none` disables that command. The launcher
and terminal command arrays below provide the fallback candidates.

### Environment variables

| Variable | Meaning |
| :--- | :--- |
| `PURPWM_TERMINAL` | Terminal executable for `Mod + Return` and the scratchpad; `auto` or `none` is also accepted |
| `PURPWM_SPOTLIGHT` | Spotlight executable; `none` disables daemon startup and launcher fallback |
| `PURPWM_FONT` | TrueType/OpenType font file for the bar, help, notifications, and lock UI |
| `PURPWM_WALLPAPER` | Optional image loaded through GdkPixbuf and center-cropped over the desktop background |
| `PURPWM_SCALE` | Highest-priority UI scale for a launch, for example `1.75` |
| `PURPWM_DEFAULT_SCALE` | Default used when no saved scale exists |
| `PURPWM_AUTOSTART` | `0` disables session helper startup; default is enabled |
| `PURPWM_STANDALONE` | When set, requests standalone/full-screen behavior |
| `PURPWM_BACKEND` | Launcher backend selection: `drm` (default) or `gamescope` for a standalone login |
| `PURPWM_BIN` | Explicit compositor executable used by `start-purpwm+` |
| `PURPWM_SESSION_CONFIG` | Alternate installation `session.conf` read by the launcher |
| `PURPWM_DRM_DEVICE` | Explicit DRM device such as `/dev/dri/card0` |
| `PURPWM_RESOLUTION` | Initial standalone resolution, `auto` in the launcher or `WIDTHxHEIGHT` |
| `PURPWM_REFRESH_RATE` | Initial standalone/DRM refresh rate in Hz |
| `PURPWM_MONITORS_CONFIG` | Explicit path replacing the normal `monitors.json` location |
| `PURPWM_VIRTUAL_MONITORS` | Comma-separated nested virtual-output description; see below |
| `PURPWM_PAM_SERVICE` | Explicit PAM service for the lock screen |
| `PURPWM_PAM_CONFDIR` | Alternate PAM configuration directory, passed to `pam_start_confdir()` |
| `PURPWM_NOTIFY_DBUS` | Enables the notification D-Bus service even with autostart disabled |
| `PURPWM_DEBUG` | Enables compositor debug logging (use `1`) |
| `PURPWM_CAPTURE_SOCKET` | Explicit portal capture socket; normally discovered automatically |
| `PURPWM_PORTAL_AUTO_APPROVE` | `1` makes `purpwm+-portal` skip its screenshot consent dialog; use with care |
| `PURPWM_IPC_SOCKET` | **Exported by the compositor**: absolute path of the active monitor IPC socket |
| `PURPWM_UPDATE_IPC_LINK` | `1` or `true` forces refresh of the convenient IPC symlink in a nested instance |
| `PURPWM_CURRENT_DESKTOP` | Desktop identity supplied by the session integration; kept as `purpwm` for portal compatibility |

The launcher exports the usual Wayland session variables (`XDG_SESSION_TYPE=wayland`,
`QT_QPA_PLATFORM`, `GDK_BACKEND`, `MOZ_ENABLE_WAYLAND`, and related settings).
`XDG_CURRENT_DESKTOP` intentionally remains `purpwm`, because the portal descriptor's
`UseIn=purpwm` and existing desktop integration key on that name.

The cursor independently honors standard `XCURSOR_THEME` and `XCURSOR_SIZE` settings. Cursor
size defaults to 24 before output scaling; if no usable theme is found, the built-in purple
arrow is used.

## Scale (HiDPI)

The principal constants are:

| Constant | Default | Meaning |
| :--- | :--- | :--- |
| `PURP_DEFAULT_SCALE` | `1.5` | Compiled fallback scale |
| `PURP_SCALE_STEP` | `0.25` | Step for `Mod + Ctrl + =` / `Mod + Ctrl + -` |
| `PURP_SCALE_MIN`, `PURP_SCALE_MAX` | `1.0`, `3.0` | Runtime scale bounds |
| `PURP_MONITOR_SCALE_ROUNDING` | `0.05` | Rounding applied to per-monitor scales received over IPC |

Startup scale precedence is:

1. `PURPWM_SCALE`;
2. `${XDG_CONFIG_HOME:-~/.config}/purpwm+/scale`, with legacy `~/.config/purpwm/scale` as a
   read fallback;
3. `PURPWM_DEFAULT_SCALE`;
4. `PURP_DEFAULT_SCALE`.

Changing scale with the keyboard writes the new `purpwm+/scale` file. In a multi-monitor DRM
session, that global setting applies to the primary output; individual output scales belong
in `monitors.json` and are restricted to `1.0`…`3.0` in `0.05` increments.

Clients supporting `wp_fractional_scale_v1` render at the exact scale. Other clients use an
integer buffer scale and compositor sampling. Geometry constants are in **logical pixels**
and are multiplied by the output scale for rendering.

## Layout, geometry, and motion

| Constant | Default | Meaning |
| :--- | :--- | :--- |
| `PURP_BAR_HEIGHT` | `28` | Status bar height |
| `PURP_BAR_FONT_SIZE` | `13.5` | Status bar font size |
| `PURP_BORDER_WIDTH` | `2` | Window border width |
| `PURP_GAP_INNER` / `PURP_GAP_OUTER` | `6` / `8` | Gaps between windows / at output edges |
| `PURP_GAP_INNER_MAX` / `PURP_GAP_OUTER_MAX` | `48` / `64` | Runtime gap limits |
| `PURP_GAP_STEP` | `2` | Gap change per key press |
| `PURP_DEFAULT_MFACT` | `0.55` | Master-area width factor; runtime bounds `0.10`…`0.90` |
| `PURP_MFACT_STEP` | `0.05` | Master-factor keybinding step |
| `PURP_DEFAULT_NMASTER` | `1` | Number of master windows |
| `PURP_MOVE_STEP` | `40` | Floating-window keyboard move distance |
| `PURP_RESIZE_STEP` | `16.0` | Distance for one resize-arrow tap |
| `PURP_RESIZE_SPEED_MIN` / `PURP_RESIZE_SPEED_MAX` | `240.0` / `1400.0` | Initial / maximum held-arrow resize speed in logical px/s |
| `PURP_RESIZE_RAMP_SECS` | `0.8` | Time to reach full resize speed |
| `PURP_TAG_NAMES` | `"1"` … `"9"` | Labels for the nine bitmask tags |

The header also documents minimum tiled/client/drag sizes, cascade placement, keyboard
repeat, backend tick rates, and activation-token lifetime. Those values affect behavior but
do not require a separate runtime file.

## Theme

Renderer colors use floating-point RGBA (`purp_color_t`, components `0.0`…`1.0`). Software UI
colors use 8-bit straight RGBA (`purp_rgba_t`) and are premultiplied by the rasterizer.

| Constant group | Used for |
| :--- | :--- |
| `PURP_COLOR_BG_ROOT` | Desktop background |
| `PURP_COLOR_BORDER_ACTIVE`, `PURP_COLOR_BORDER_INACTIVE` | Focused / unfocused borders |
| `PURP_COLOR_BORDER_GLOW` | Focused-window halo |
| `PURP_COLOR_FULLSCREEN_BACKDROP` | Black fullscreen backdrop |
| `PURP_OPACITY_ACTIVE`, `PURP_OPACITY_INACTIVE` | Focus-dependent window opacity |
| `PURP_OPACITY_POPUP`, `PURP_OPACITY_DND_ICON` | Popup/layer and drag-icon alpha |
| `PURP_BAR_*_RGBA` | Bar, tags, layout pills, text, urgent state, and Bluetooth state |
| `PURP_LOCK_*_RGBA`, `PURP_TOAST_*_RGBA`, `PURP_HELP_*_RGBA` | Lock screen, notifications, and help overlay |
| `PURP_WALLPAPER_BG_RGBA` | Opaque color beneath wallpaper |
| `PURP_CURSOR_*_RGBA` | Built-in fallback cursor |

Fullscreen windows are always opaque. Window rules can also force a fixed opacity.

## Fonts and wallpaper

`PURP_FONT_PATHS` lists system monospace font files tried after `PURPWM_FONT`. Next purpwm+
asks Fontconfig for `monospace`; if that fails, it uses the DejaVu Sans Mono font compiled
into `build/embedded_font.c`. Thus a missing system font does not leave the bar textless.

`PURPWM_WALLPAPER` accepts formats supported by the installed GdkPixbuf loaders (commonly PNG,
JPEG, WebP, and BMP). The image is center-cropped to cover each output without stretching and
cached at the required resolution. Leave it unset for the purple background. Layer-shell
wallpaper clients remain supported above this built-in image.

## Window rules

`PURP_WINDOW_RULES` is checked as a window maps and announces its XDG `app_id` and title. The
first matching rule wins. Every non-`NULL` matcher must be a non-empty substring of its field;
`NULL` is a wildcard, and a rule with both matchers `NULL` never matches. Matching is settled
after a hit or once both `app_id` and title are known.

Use the supplied macros when adding entries:

```c
/* Float a utility without overriding focus-dependent opacity. */
PURP_RULE_NO_OPACITY("pavucontrol", NULL, 0, true),

/* Force a video player fully opaque without forcing it to float. */
PURP_RULE("mpv", NULL, 0, false, 1.0f),
```

Arguments are `(app_id_substring, title_substring, tag_mask, floating, opacity)`. Tag mask `0`
keeps the current tags; otherwise bit 0 represents tag 1, bit 1 tag 2, and so on. Rule-floated
windows open centered at a client-compatible size and are cascaded to avoid exact overlap.
Built-in rules cover Spotlight and purpwm control windows, polkit/pinentry dialogs,
Flameshot, common settings/file chooser windows, media players, Sober, and Steam games.

## Commands and autostart

Each command table is an ordered array of NULL-terminated argument vectors. purpwm+ tries the
first entry whose `argv[0]` exists on `PATH` (or at an absolute path) and does not invoke a
shell. Add a candidate as `{ { "program", "argument", NULL } }`.

| Constant | Trigger |
| :--- | :--- |
| `PURP_LAUNCHER_COMMANDS` | Tap `Mod`, `Mod + p`; used if the Spotlight socket fast path is unavailable |
| `PURP_TERMINAL_COMMANDS` | `Mod + Return`, scratchpad |
| `PURP_SYSTEM_MONITOR_COMMANDS` | `Mod + Escape`, metrics/clock clicks |
| `PURP_WIFI_COMMANDS`, `PURP_BLUETOOTH_COMMANDS` | Network pills and manager keybindings |
| `PURP_VOL_UP/DOWN/MUTE_COMMANDS`, `PURP_MIC_MUTE_COMMANDS` | Volume and microphone keys/pill |
| `PURP_BRIGHTNESS_UP/DOWN_COMMANDS` | Brightness keys; logind is the fallback |
| `PURP_MEDIA_PLAY/NEXT/PREV_COMMANDS` | Media keys |
| `PURP_MIXER_COMMANDS` | Right-click on `VOL` |
| `PURP_FLAMESHOT_COMMANDS` | Flameshot status-bar button |
| `PURP_SCREENSHOT_PORTAL_COMMANDS` | Starts `purpwm+-portal --session` |
| `PURP_KEYRING_COMMANDS` | Starts `purpwm+-keyring start` |
| `PURP_POLKIT_COMMANDS` | Starts the native polkit agent |
| `PURP_AUTOSTART_COMMANDS` | Additional login commands; empty by default |

Do not add another notification daemon to autostart: purpwm+ owns
`org.freedesktop.Notifications`, and a second daemon can take the bus name away from the
built-in toasts. Screenshots use `PURP_SCREENSHOT_DIR` (`Pictures/Screenshots` under `$HOME`)
and `PURP_SCREENSHOT_PREFIX` (`purpwm+-`). `PURP_WIFI_AUTOCONNECT_DEVICE` selects the interface
used for the post-startup Wi-Fi reconnect attempt (default `wlan0`).

The POSIX-shell helpers `scripts/purpwm+-wifi` and `scripts/purpwm+-bt` provide the interactive
network frontends. They require NetworkManager/`nmcli` or BlueZ/`bluetoothctl`, respectively;
see [Installation](installation.md#optional-desktop-tools).

## Multi-monitor configuration (`monitors.json`)

Live display configuration is stored in:

```text
${XDG_CONFIG_HOME:-~/.config}/purpwm+/monitors.json
```

The legacy `~/.config/purpwm/monitors.json` is read if no new file exists; writes always use
`purpwm+`, unless `PURPWM_MONITORS_CONFIG` selects another path. Applied output names,
positions, modes, refresh rates, primary flags, enablement, and fractional scales are restored
at startup and after hotplug. Refresh rates in this file and IPC are integer **millihertz**
(`60000` = 60 Hz).

A typical entry contains:

```json
{
  "name": "eDP-1",
  "enabled": true,
  "primary": true,
  "width": 1920,
  "height": 1080,
  "refresh_mhz": 60000,
  "scale": 1.25,
  "x": 0,
  "y": 0
}
```

Positions are logical desktop coordinates. Query `get_monitors` before constructing an apply
request so you use valid connector names and advertised modes.

## Monitor IPC

The compositor exports `PURPWM_IPC_SOCKET` to its children. The normal path is:

```text
$XDG_RUNTIME_DIR/purpwm+-ipc-$WAYLAND_DISPLAY.sock
```

Convenient `purpwm+-ipc.sock` and legacy `purpwm-ipc.sock` symlinks are maintained in the
runtime directory when doing so would not replace another live nested instance's endpoint.
Set `PURPWM_UPDATE_IPC_LINK=1` to force the link for a development instance.

Requests and responses are UTF-8 JSON objects terminated by a newline. Supported commands:

| `cmd` | Additional fields | Action |
| :--- | :--- | :--- |
| `get_monitors` | none | Return connected outputs, current state, and available modes |
| `apply_monitors` | `monitors`: array of entries like the example above | Validate, apply, and save a display layout |
| `focus_monitor` | optional integer `delta`, default `1` | Focus the previous/next output |
| `move_window_to_monitor` | optional integer `delta`, default `1` | Move the focused window to another output |

For example, with `socat` installed:

```sh
printf '%s\n' '{"cmd":"get_monitors"}' \
  | socat - UNIX-CONNECT:"$PURPWM_IPC_SOCKET"

printf '%s\n' '{"cmd":"focus_monitor","delta":-1}' \
  | socat - UNIX-CONNECT:"$PURPWM_IPC_SOCKET"
```

`apply_monitors` accepts up to 32 entries and requests are capped at 64 KiB. Width and height
must be in `100`…`32768`, refresh in `1000`…`1000000` millihertz, and positions in
`-131072`…`131072`. Omitted `enabled` defaults to `true`, omitted `primary` to `false`,
omitted/null positions to `0`, and omitted/null refresh to `60000`. The parser accepts scales
in `0.5`…`4.0`; applying the configuration rounds them to 0.05 and clamps them to the runtime
`1.0`…`3.0` range. Use one complete layout per request rather than concurrently editing the
JSON file.

## Nested virtual monitors

For nested development, `PURPWM_VIRTUAL_MONITORS` uses the C backend's format:

```text
name:WIDTHxHEIGHT[@HZ][:SCALE],name:WIDTHxHEIGHT[@HZ][:SCALE],...
```

For example:

```sh
PURPWM_VIRTUAL_MONITORS='one:1920x1080@60:1.0,two:2560x1440@60:1.25' \
  ./build/purpwm+ --nested --no-autostart
```

This exercises independent tags, scales, and monitor focus/movement without multiple physical
panels. It is separate from the saved DRM `monitors.json` layout.
