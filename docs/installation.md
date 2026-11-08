# Installation

purpwm+ builds with a C11 toolchain and `make`. The compositor, polkit agent, screenshot
portal, and keyring helper are all native C programs. Neither the installed session nor any
companion daemon needs Rust, Cargo, Python, or PyGObject at runtime. Python 3 is used only by
the Makefile's build-time font-to-C conversion.

## Full session installation

From a source checkout, run as your regular user:

```sh
./install.sh --user
```

For a session that appears reliably in a display manager:

```sh
./install.sh --system
```

The installer builds with `make` unless `--no-build` is supplied, then installs the native
programs, POSIX-shell helpers, session configuration, PAM lock configuration where applicable,
and `purpwm+.desktop`. System paths and privilege elevation are handled by the script; do not
run the whole build as root.

The installer is deliberately smaller than the old purpwm wizard. It does not install distro
packages or interactively choose a terminal, launcher, font, wallpaper, scale, or resolution.
Install prerequisites first and edit `session.conf` for those preferences; see
[Configuration](configuration.md#session-settings-sessionconf).

### Installer options

| Option | Effect |
| :--- | :--- |
| `--user` | Install for the current user (the default for a regular user) |
| `--system` | Install system-wide and register the login session/PAM service (the default for root) |
| `--prefix PATH` | Override the installation prefix; `--prefix=PATH` also works |
| `--destdir PATH` | Stage files below a packaging root without changing embedded runtime paths; `--destdir=PATH` also works |
| `--dry-run` | Report the installation plan without building or changing files |
| `--no-build` | Reuse already-built programs instead of running `make` |
| `--binary FILE` | Use a specific purpwm+ compositor executable and skip the build; `--binary=FILE` also works |
| `-h`, `--help` | Print installer usage |

`--binary` replaces only the compositor artifact; the three companion executables must still
be available in the checkout or on `PATH`.

Examples:

```sh
# Inspect a system installation first.
./install.sh --system --dry-run

# Build, then install below ~/.local for one user.
./install.sh --user

# Reuse a tree just built with make -j.
./install.sh --user --no-build

# Custom runtime prefix.
./install.sh --system --prefix /opt/purpwm+

# Stage a system package; runtime paths still use /usr/local.
./install.sh --system --destdir /tmp/purpwm-plus-package
```

With default system paths, the full installation includes:

| File | Purpose |
| :--- | :--- |
| `/usr/local/bin/purpwm+` | Compositor |
| `/usr/local/bin/purpwm+-polkit-agent` | Native polkit authentication dialog |
| `/usr/local/bin/purpwm+-portal` | Native Screenshot portal backend |
| `/usr/local/bin/purpwm+-keyring` | Native Secret Service helper |
| `/usr/local/bin/start-purpwm+` | Session environment/backend launcher |
| `/usr/local/bin/purpwm+-wifi`, `/usr/local/bin/purpwm+-bt` | Network manager helpers |
| `/usr/local/share/purpwm+/session.conf` | Session defaults skeleton |
| `/etc/pam.d/purpwm+` | Lock-screen authentication service, with legacy `purpwm` fallback support |
| `/usr/share/wayland-sessions/purpwm+.desktop` | Display-manager session entry |

A user installation places executables under `~/.local/bin` and installed defaults under
`~/.local/share/purpwm+/session.conf` by default. Optional overrides belong in
`${XDG_CONFIG_HOME:-~/.config}/purpwm+/session.conf`. Many display managers ignore per-user
Wayland session directories, so use `--system` if **purpwm+** is not offered at the login screen.

After installation, log out, choose **purpwm+**, and log in. Quit with `Mod + Shift + q`.

## Build requirements

Required tools:

- a C compiler supporting `-std=gnu11` (GCC or Clang), `make`, and `pkg-config`;
- `wayland-scanner`;
- Python 3 for the build-time embedded-font generator only;
- development headers/libraries for every module below.

The compositor's `pkg-config` modules, as listed in the Makefile, are:

```text
wayland-server wayland-client wayland-egl egl glesv2 libdrm gbm
libinput libseat libudev xkbcommon freetype2 fontconfig gdk-pixbuf-2.0
libpng xcursor gio-2.0 glib-2.0 pam
```

The native companions additionally require:

```text
gtk4 polkit-agent-1 polkit-gobject-1 libsecret-1
```

Install the corresponding development packages with your distribution's package manager.
Package names differ, but the `pkg-config` names above are the exact build interface. Check
that all of them resolve before building:

```sh
pkg-config --print-errors \
  wayland-server wayland-client wayland-egl egl glesv2 libdrm gbm \
  libinput libseat libudev xkbcommon freetype2 fontconfig gdk-pixbuf-2.0 \
  libpng xcursor gio-2.0 glib-2.0 pam \
  gtk4 polkit-agent-1 polkit-gobject-1 libsecret-1
```

The compositor also links libm and pthreads. No X11 development package, Cargo, Meson, or
CMake is needed.

## Building and testing

```sh
make -j"$(nproc)"
make check
```

`make` builds:

```text
build/purpwm+
build/purpwm+-polkit-agent
build/purpwm+-portal
build/purpwm+-keyring
build/test_purpwm_plus
```

All project objects use `-std=gnu11 -Wall -Wextra -Werror` plus the additional warning and
feature flags in the Makefile. The default optimization flags are `-O2 -g`. Useful targets
and overrides:

| Target/variable | Purpose |
| :--- | :--- |
| `make protocols` | Generate `build/protocols/*` with `wayland-scanner` and `build/embedded_font.c` |
| `make compositor` | Build only the compositor |
| `make tools` | Build only the three companions |
| `make test` | Run `build/test_purpwm_plus` |
| `make check-headers` | Verify header self-containment and Wayland client/server separation |
| `make check` | Run header checks and unit tests |
| `make clean` | Remove `build/` |
| `CC=clang` | Select another C compiler |
| `OPTFLAGS='-O0 -g3'` | Change optimization/debug flags |
| `WERROR=` | Relax `-Werror` for a local toolchain diagnostic |
| `CFLAGS=...`, `LDFLAGS=...` | Append compiler/linker flags |
| `WAYLAND_SCANNER=...`, `PYTHON=...` | Select build tools explicitly |

Generated protocol files and the embedded font source are build artifacts. Do not edit them;
change `protocols/*.xml`, `assets/fonts/DejaVuSansMono.ttf`, or the Makefile as appropriate.

## Binary-only `make install`

For packaging or a minimal binary installation without session scripts:

```sh
make
make install
# or stage it:
make DESTDIR=/tmp/purpwm-plus-package PREFIX=/usr install
```

`make install` installs only the four native executables and the project/font licenses. It
does **not** install `start-purpwm+`, network helpers, `session.conf`, PAM configuration, or the
desktop session; use `install.sh` for those. Defaults and overrides:

| Variable | Default |
| :--- | :--- |
| `PREFIX` | `/usr/local` |
| `BINDIR` | `$(PREFIX)/bin` |
| `DATADIR` | `$(PREFIX)/share` |
| `DESTDIR` | empty |

`make uninstall` removes the files owned by that target. `CC`, `PKG_CONFIG`, `INSTALL`, and the
flag variables can also be overridden on the `make` command line.

## Running from the build directory

### Nested Wayland

Inside an existing Wayland session:

```sh
./build/purpwm+ --nested --no-autostart
```

`--winit` is an alias retained from purpwm. This C implementation is a native Wayland host
backend, not an X11/winit backend. The compositor prints its new `WAYLAND_DISPLAY` socket;
use that value to start a client inside it:

```sh
WAYLAND_DISPLAY=wayland-2 foot
```

Replace `wayland-2` with the socket actually reported by your instance.

### Native DRM/KMS

From a TTY with seat and GPU access:

```sh
./build/purpwm+ --drm
```

An active `systemd-logind` session normally supplies access through `libseat`. Alternatively,
run `seatd` and grant the user its seat access (commonly the `seat` group). The user also needs
access to `/dev/dri/card*` and input devices. Set `PURPWM_DRM_DEVICE=/dev/dri/cardN` if automatic
GPU selection chooses the wrong device.

### Headless

To run without a host compositor or DRM display:

```sh
./build/purpwm+ --headless --no-autostart
```

### Compositor command-line options

| Option | Effect |
| :--- | :--- |
| `--standalone`, `--fullscreen` | Request standalone/borderless fullscreen behavior; `PURPWM_STANDALONE=1` is equivalent |
| `--drm` | Use native multi-monitor DRM/KMS + GBM + libseat + libinput |
| `--winit`, `--nested` | Use the nested Wayland host backend |
| `--headless` | Run without a visible DRM or host-Wayland backend |
| `--spawn-terminal` | Launch a terminal after startup |
| `--no-autostart` | Skip Spotlight, session companions, radio bring-up, and custom autostart commands |
| `-h`, `--help` | Print command usage and keybinding summary |

`--no-autostart` is strongly recommended for a nested development instance so it does not
start a second polkit agent, replace the active portal, restart the launcher on itself, or
toggle the host's radios.

## What `start-purpwm+` does

The full session launcher:

1. Loads the installation `share/purpwm+/session.conf`, then the user's
   `~/.config/purpwm+/session.conf`, and finally legacy `~/.config/purpwm/session.conf` when
   readable. These files are sourced cumulatively, so later explicit assignments win.
2. Locates `purpwm+` next to the launcher, in the checkout's `build/`, or on `PATH`;
   `PURPWM_BIN` overrides this choice. Installed helper directories are put on `PATH`.
3. Establishes `XDG_RUNTIME_DIR`, the user D-Bus address, and standard Wayland desktop
   variables. `XDG_CURRENT_DESKTOP=purpwm` is retained for portal compatibility.
4. With autostart enabled, starts/imports GNOME Keyring, selects an available SSH agent,
   imports the session environment into `systemd --user` and D-Bus activation, and brings up
   networking/Bluetooth in the background.
5. Runs directly in an existing Wayland session. On a TTY it selects `purpwm+ --drm` by
   default. `PURPWM_BACKEND=gamescope` instead wraps the standalone compositor in gamescope
   using `PURPWM_RESOLUTION` and `PURPWM_REFRESH_RATE`.

`PURPWM_AUTOSTART=0` or passing `--no-autostart` suppresses helper/radio startup. The installed
session continues to work if the source checkout is moved or deleted.

## Optional desktop tools

| Feature | External program/service |
| :--- | :--- |
| Terminal/scratchpad | Alacritty, foot, kitty, WezTerm, COSMIC Terminal, GNOME Terminal, weston-terminal, or xterm |
| Launcher | Spotlight (`spotlight --toggle`), or another launcher configured in `include/config.h` |
| Wi-Fi | NetworkManager, `nmcli`, and optionally `nmtui` / `nm-connection-editor` |
| Bluetooth | BlueZ, `bluetoothd`, `bluetoothctl`, and `rfkill` |
| Audio | PipeWire/WirePlumber with `wpctl`, or PulseAudio-compatible `pactl`; `pavucontrol` for the mixer |
| Media keys | `playerctl` and a compatible player |
| Brightness | `brightnessctl`, or logind's backlight D-Bus API |
| System monitor | `btop` or `top` |
| Portal capture UI | `flameshot`, `xdg-desktop-portal`, and a GTK portal backend for unrelated portal interfaces |
| Keyring service | `gnome-keyring-daemon`; `seahorse` for the optional manager; a GNOME/GCR prompter for unlock dialogs |
| Polkit | `polkitd` and its system D-Bus service |
| Nested gamescope route | `gamescope` |

Missing optional tools degrade only their associated pill/keybinding. The native helpers do
not need GI typelibs or Python bindings. NetworkManager, Bluetooth, PipeWire, polkit, and the
keyring system services still need to be enabled/configured by your distribution or
administrator.

## OS keyring

Applications use the standard Secret Service API supplied by GNOME Keyring. The native
`purpwm+-keyring` executable starts that service and imports the Wayland display into D-Bus
activation so password prompts appear inside purpwm+.

```sh
purpwm+-keyring start    # Start gnome-keyring-daemon and set up the default alias
purpwm+-keyring status   # List collections and lock state; never prints secrets
purpwm+-keyring unlock   # Create/unlock the default keyring through the system prompter
purpwm+-keyring manage   # Exec seahorse (Passwords and Keys)
purpwm+-keyring --help
```

With no action, `status` is the default. If a login collection exists and no default has been
selected, `start` points the default alias at it without replacing an existing selection.
Starting the daemon does not by itself decrypt a locked keyring. Password login can unlock it
automatically when your display manager/login PAM stack includes the distribution's optional
GNOME Keyring PAM integration and the keyring password matches the account password. Autologin
and fingerprint login generally still require `purpwm+-keyring unlock`.

The `/etc/pam.d/purpwm+` service used by the **screen locker** is separate from login PAM. The
small installer does not rewrite display-manager PAM services. See
[GNOME's PAM integration guide](https://wiki.gnome.org/Projects/GnomeKeyring/Pam) if you want
login-time unlocking.

## Screenshot portal and Flameshot

`purpwm+-portal` implements `org.freedesktop.impl.portal.Screenshot` **version 2** natively with
GTK 4 and GDBus. In a full session the compositor starts it with `--session`. It asks for
sharing consent, obtains a PNG over the compositor's private capture socket, and returns a
`file://` URI to `xdg-desktop-portal`. Flameshot can then provide its selection/annotation UI.

For manual per-user setup inside the session:

```sh
purpwm+-portal --install   # Install/repair backend selection and D-Bus activation
purpwm+-portal --session   # Start the adapter and refresh the frontend
```

The setup creates `~/.config/xdg-desktop-portal/purpwm+-portals.conf`, a merged descriptor
directory under `${XDG_DATA_HOME:-~/.local/share}/purpwm+/portals`, and a per-user D-Bus
service. Existing non-purpwm portal descriptors are retained. The frontend selects `purpwm`
for screenshots; `XDG_CURRENT_DESKTOP=purpwm` therefore remains intentional.

Portal PNGs are private temporary files under `$XDG_RUNTIME_DIR/purpwm-screenshots` and expire
after about an hour. They do not overwrite the clipboard or populate `~/Pictures/Screenshots`.
The direct `Print` binding does both of those things independently. Screen recording and color
picking are not implemented. `--no-ask` (or `PURPWM_PORTAL_AUTO_APPROVE=1`) suppresses the
consent dialog and should only be used when that policy is acceptable.

A nested `--no-autostart` instance does not take over the host desktop's active portal.

## Updating

Rebuild and repeat the same installation scope:

```sh
make -j"$(nproc)"
./install.sh --user --no-build
# or ./install.sh --system --no-build
```

Log out and back in to run the new compositor and companions. For a source update that also
changes scripts or session files, run the installer without `--no-build` if you want it to
perform the build as well.
