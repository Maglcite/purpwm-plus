# Troubleshooting

## Enable debug logs

purpwm+ uses native C logging, not Rust's `tracing`/`RUST_LOG`. Enable debug messages with
`PURPWM_DEBUG=1` and capture stderr:

```sh
PURPWM_DEBUG=1 ./build/purpwm+ --nested --no-autostart 2> purpwm+.log
```

When launched by a display manager, session output goes to that manager's log. On systemd
systems, useful starting points are:

```sh
journalctl -b
journalctl -b --user
```

For helper failures, run the relevant native binary from a terminal **inside** purpwm+ so it
inherits the correct Wayland and D-Bus environment.

## Test changes safely

Build and run the deterministic tests without starting a desktop:

```sh
make check
```

Test a new compositor nested, with session helpers disabled:

```sh
env -u PURPWM_STANDALONE ./build/purpwm+ --nested --no-autostart
```

- `PURPWM_STANDALONE` is exported by standalone sessions; unset it so the development copy
  does not unexpectedly request fullscreen/standalone behavior.
- `--no-autostart` avoids a second polkit agent, portal/keyring startup, Spotlight restart,
  custom autostart commands, and radio changes on the host.
- Use `--headless --no-autostart` when neither a host Wayland window nor DRM is available.
- Rebuilding `build/purpwm+` does not replace the already mapped program in a running session;
  restart/log in again to use the new build.
- `kill -USR1 <pid>` asks a particular instance to save a screenshot, which can help inspect
  rendering without focusing its window.

## Build problems

**`pkg-config` cannot find a package**

Install the development package that provides the named `.pc` file. The exact module list is
in [Build requirements](installation.md#build-requirements). Native companions need GTK 4,
polkit agent/gobject, and libsecret in addition to the compositor libraries; these are C
headers/libraries, not Python GI packages.

**Protocol headers or `build/embedded_font.c` are missing**

Generate build artifacts explicitly and inspect the first failing command:

```sh
make protocols
```

This requires `wayland-scanner`, Python 3, the bundled `protocols/*.xml`, and
`assets/fonts/DejaVuSansMono.ttf`. Python is not needed after generation/building. Override
`WAYLAND_SCANNER` or `PYTHON` if the tools have nonstandard names.

**A warning is treated as an error**

Normal project builds intentionally use `-Werror`. Prefer fixing the source or updating the
compiler/dependencies. For a local diagnostic build only, `make WERROR=` removes that one flag;
it does not suppress real compiler/linker errors.

**Stale generated/dependency files cause confusing errors**

```sh
make clean
make -j"$(nproc)"
make check
```

`make clean` removes only `build/`, including generated protocols and objects. It does not
remove user configuration or installed session files.

## Session and backend problems

**purpwm+ is not listed at the login screen**

Use `./install.sh --system`, then check that the selected session directory contains
`purpwm+.desktop` and that its `Exec`/`TryExec` paths exist. Many display managers do not read
per-user Wayland session directories. The entry uses `X-DesktopNames=purpwm+` for desktop-file
validation compatibility.

**The session returns immediately to the login screen**

Run `start-purpwm+` from a TTY with `PURPWM_DEBUG=1` to see the error. Check the paths in the
installed `session.conf`, `PURPWM_BIN`, and the compositor's shared-library dependencies.
Preview filesystem changes with `./install.sh --system --dry-run`.

For the default native backend, verify all of the following:

- an active `systemd-logind` session, or `seatd` running with suitable user seat access;
- permission to open `/dev/dri/card*` and the relevant input devices;
- a working EGL/GLESv2/GBM driver stack;
- no other compositor exclusively owning the same DRM master device.

Set `PURPWM_DRM_DEVICE=/dev/dri/cardN` to select a GPU explicitly. If installed,
`PURPWM_BACKEND=gamescope` is an alternative standalone host route; without gamescope that
setting correctly fails rather than silently selecting another backend.

**A nested instance opens fullscreen or changes host services**

Stop it and restart with:

```sh
env -u PURPWM_STANDALONE ./build/purpwm+ --nested --no-autostart
```

The nested backend requires a host **Wayland** display. The `--winit` spelling is only a
compatibility alias; the C rewrite does not provide the old winit X11 host path. Use `--drm`
on a TTY or `--headless` when no host display exists.

**An application fails because it requires X11/XWayland**

purpwm+ is a Wayland compositor and does not provide XWayland. Enable the application's native
Wayland backend where available. For Chromium/Electron this is commonly
`--ozone-platform=wayland`; Qt/GTK applications use their Wayland platform backends through the
session environment.

## Fonts, scaling, and rendering

**The bar or overlay shows missing glyphs**

Set `PURPWM_FONT` to a valid TrueType/OpenType font, or fix Fontconfig's monospace match:

```sh
fc-match monospace
```

purpwm+ tries `PURPWM_FONT`, `PURP_FONT_PATHS`, Fontconfig, and finally its compiled-in DejaVu
Sans Mono fallback. If even the fallback is absent, rebuild with `make protocols && make` so
`build/embedded_font.c` is regenerated and linked. Installed sessions must reinstall/restart
the updated binary.

**Everything is too large or too small**

Use `Mod + Ctrl + =` and `Mod + Ctrl + -`. The value is saved in
`${XDG_CONFIG_HOME:-~/.config}/purpwm+/scale`. To discard a saved override, move that file
aside while the compositor is stopped; also check legacy `~/.config/purpwm/scale`, which is
read as a fallback. `PURPWM_SCALE` has still higher priority, and `PURPWM_DEFAULT_SCALE` is
used when no saved value exists.

For one output in a multi-monitor setup, change its `scale` through Display Settings/
`apply_monitors`; per-output values live in `monitors.json`.

**An application is blurry**

Check whether it supports native Wayland fractional scaling. Clients without
`wp_fractional_scale_v1` use integer buffer scaling and compositor sampling. Very old clients
that render only at 1x can remain blurry at a larger scale. Enable the application's Wayland
mode where possible; XWayland is not a fallback here.

**Window transparency does not match the theme**

purpwm+ blends the alpha supplied by the application with `PURP_OPACITY_ACTIVE` /
`PURP_OPACITY_INACTIVE`. Check the client's own opacity/background setting. Fullscreen windows
and rules using `PURP_RULE(..., 1.0f)` are intentionally opaque. `Mod + o` toggles a focused
window's forced-opaque state.

For Alacritty specifically, a malformed configuration can cause the entire file—including its
window opacity—to be ignored. Start Alacritty from another terminal and fix any reported
duplicate key/table parse error before changing purpwm+ colors.

## Input, launcher, and desktop tools

**The launcher does not open**

Tapping `Mod` first tries Spotlight's runtime socket and then the configured launcher command.
Ensure `spotlight` is installed and running, set `PURPWM_SPOTLIGHT` to its executable in
`session.conf`, or adjust `PURP_LAUNCHER_COMMANDS` in `include/config.h`. `none` intentionally
disables the fallback. `Mod + p` uses the same path.

**Wi-Fi or Bluetooth shows `N/A`**

Check the services and command-line tools:

```sh
systemctl status NetworkManager bluetooth
command -v nmcli bluetoothctl rfkill
```

Try `purpwm+-wifi` or `purpwm+-bt` inside the session for a more direct error. Radio toggles do
not install or enable NetworkManager/BlueZ automatically. If your wireless interface is not
`wlan0`, update `PURP_WIFI_AUTOCONNECT_DEVICE` for the startup reconnect attempt.

**Volume, media, or brightness keys do nothing**

Volume/microphone commands need `wpctl` or `pactl`; media keys need `playerctl`. Right-clicking
`VOL` opens `pavucontrol` when available. Install `brightnessctl` for the simplest backlight
path. Without it, purpwm+ asks logind to set brightness for a device under
`/sys/class/backlight`; inspect logind/session permissions if that fallback fails.

**Touch or pointer coordinates look wrong after a scale change**

Restart the new build nested and reproduce with one output first. Check the active scale and
`PURPWM_VIRTUAL_MONITORS` specification, then test a second output. Report the backend,
output mode/scale, client, and whether the offset affects native touch, pointer input, or only
one X11-only application; those are different paths.

## Lock screen and polkit

**The lock screen rejects a valid password**

Inspect `/etc/pam.d/purpwm+` and the distribution PAM stack it includes. Service lookup order
is `PURPWM_PAM_SERVICE`, `purpwm+`, legacy `purpwm`, `vlock`, then `login`. Set
`PURPWM_PAM_SERVICE` to an appropriate existing service, or use `PURPWM_PAM_CONFDIR` for an
alternate `pam.d` directory. Reinstall the session PAM file with `install.sh --system` if it is
missing; do not confuse lock PAM with display-manager login/keyring PAM.

**No administrator password dialog appears**

Check that the native agent is running:

```sh
pgrep -fa purpwm+-polkit-agent
```

Run `purpwm+-polkit-agent` manually inside the session. Successful registration prints
`purpwm+ Polkit Authentication Agent successfully registered.` Another desktop's agent can
already own the login session's polkit agent role. Also verify that `polkitd` and the system
D-Bus are running. GTK 4 and polkit libraries are required; Python/PyGObject is not.

## Keyring

**Applications cannot save passwords**

Run:

```sh
purpwm+-keyring status
purpwm+-keyring start
```

Install `gnome-keyring-daemon` and libsecret if the helper reports that the service is missing.
`start` imports `WAYLAND_DISPLAY` and related variables into D-Bus activation so the system
prompter can display inside purpwm+. Ensure a GNOME/GCR password prompter is installed.

**The keyring asks for a password at every login**

Run `purpwm+-keyring unlock` to create/unlock the default collection. Starting the daemon alone
does not decrypt it. Automatic password-login unlocking requires GNOME Keyring hooks in the
display manager/login PAM stack and a keyring password matching the account password; the
purpwm+ installer does not modify those login services. Autologin usually needs manual unlock.
Use `purpwm+-keyring manage` (Seahorse) to inspect or change collections.

## Screenshots and portal

**A saved screenshot has no clipboard image after purpwm+ exits**

The compositor itself serves the clipboard selection, so that clipboard offer ends with the
compositor. The PNG remains in `~/Pictures/Screenshots` with the `purpwm+-` prefix. Copy the
image into a persistent clipboard manager if it must outlive the session.

**Flameshot cannot capture the screen**

Install `flameshot`, `xdg-desktop-portal`, and a GTK portal backend for other interfaces, then
log in again. Check both processes and the frontend log:

```sh
pgrep -fa purpwm+-portal
journalctl --user -u xdg-desktop-portal.service
```

Repair the per-user setup from inside purpwm+:

```sh
purpwm+-portal --install
purpwm+-portal --session
```

Check `~/.config/xdg-desktop-portal/purpwm+-portals.conf` and the merged directory under
`${XDG_DATA_HOME:-~/.local/share}/purpwm+/portals`. Screenshot selection must name the `purpwm`
backend. `XDG_CURRENT_DESKTOP=purpwm` is intentional even though the binary is `purpwm+`.

The compositor must be new enough to serve `purpwm+-capture-*.sock`; restart it after upgrading.
`PURPWM_CAPTURE_SOCKET` can diagnose a nonstandard socket location. Captures are refused while
locked, color picking/screen recording are not implemented, and a nested `--no-autostart`
instance intentionally leaves the host portal untouched. Do not use `--no-ask` unless bypassing
sharing consent is deliberate.

## Monitor configuration and IPC

**Display Settings cannot connect**

From a terminal inside the same purpwm+ instance, check the exported path and query it:

```sh
printf 'PURPWM_IPC_SOCKET=%s\n' "$PURPWM_IPC_SOCKET"
printf '%s\n' '{"cmd":"get_monitors"}' \
  | socat - UNIX-CONNECT:"$PURPWM_IPC_SOCKET"
```

Use the environment path rather than assuming a socket name. The usual endpoint is
`$XDG_RUNTIME_DIR/purpwm+-ipc-$WAYLAND_DISPLAY.sock`; convenience links are
`purpwm+-ipc.sock` and legacy `purpwm-ipc.sock`. A second nested compositor must not steal a
live instance's link. `PURPWM_UPDATE_IPC_LINK=1` explicitly requests link replacement for a
development instance.

**A saved monitor layout is ignored or rejected**

Check `${XDG_CONFIG_HOME:-~/.config}/purpwm+/monitors.json` first; legacy
`~/.config/purpwm/monitors.json` is only a read fallback, and `PURPWM_MONITORS_CONFIG` overrides
both. Validate JSON syntax, connector names, advertised width/height modes, positions, and
millihertz refresh values (`60000`, not `60`, for 60 Hz). Scales are rounded to 0.05 and clamped
to 1.0–3.0.

Use `get_monitors` to obtain current names/modes and `apply_monitors` to write a complete valid
layout instead of editing a live session's file concurrently. If the file is malformed, move
it aside while purpwm+ is stopped and reapply a known-good layout.

See [Configuration](configuration.md#monitor-ipc) for the full request fields and limits.
