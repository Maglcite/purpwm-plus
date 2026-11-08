# purpwm+

**purpwm+** is an ultra-lightweight, `dwm`-inspired dynamic tiling **Wayland compositor**
with a translucent purple glass theme. It is a ground-up **pure C11** rewrite of
[purpwm](https://github.com/Maglcite/purpwm): the compositor, `org.freedesktop.Notifications`
service, and all companion daemons compile directly to native C executables — with no
Rust, Smithay, winit, wlroots, or Python runtime dependency.

- **dwm workflow**: nine bitmask tags, master/stack tiling, per-tag layouts, `mfact`,
  `nmaster`, zoom, and a drop-down scratchpad.
- **Five layouts**: Tile `[]=`, Centered Master `|M|`, Grid `###`, Monocle `[M]`, and
  Floating `><>`.
- **Purple glass UI**: focus borders and glow, translucent windows, a clickable status bar,
  notification toasts, and a built-in `Mod + h` help overlay.
- **HiDPI and multiple monitors**: per-output modes and fractional scales, independent tags,
  hotplug support, and live display arrangement through JSON-line IPC.
- **Native input**: keyboard, pointer, `wl_touch`, tablet protocol support, tap-to-focus,
  touch window dragging, and status-bar touch controls.
- **Session essentials**: a PAM lock screen, `org.freedesktop.Notifications`, screenshots and
  clipboard, drag and drop, layer shell, idle inhibit, and server-side decorations.
- **Three backends**: native DRM/KMS (`--drm`), nested Wayland/gamescope
  (`--winit` / `--nested`), and headless (`--headless`).
- **Fast, boring builds**: `make`, a C compiler, and `pkg-config`; no Cargo or Meson.
  Protocol bindings and the fallback font are generated at build time.

## Native programs

`make` creates these programs in `build/`:

| Program | Purpose |
| :--- | :--- |
| `purpwm+` | Wayland compositor, renderer, window manager, status UI, notifications, and PAM locker |
| `purpwm+-polkit-agent` | GTK 4 polkit authentication agent and purple password dialog |
| `purpwm+-portal` | GTK 4 / GDBus `org.freedesktop.impl.portal.Screenshot` backend, version 2 |
| `purpwm+-keyring` | Native libsecret/GDBus Secret Service helper: `start`, `status`, `unlock`, `manage` |

The full installer also installs the POSIX-shell session launcher and Wi-Fi/Bluetooth
helpers. The daemons no longer run Python code.

## Quick start

Clone, build all four native executables, and run the deterministic C unit test suite:

```sh
git clone https://github.com/Maglcite/purpwm-plus.git "purpwm+"
cd "purpwm+"
make -j"$(nproc)"
make check
```

Try a nested instance inside an existing Wayland session without starting session-wide
helpers:

```sh
./build/purpwm+ --nested --no-autostart
```

For a complete login-session installation, run the installer as your normal user and select
a user or system installation:

```sh
./install.sh --user
# or, for a login-screen session:
./install.sh --system
```

A system installation registers **purpwm+** with the display manager. Log out, select that
session, and log in. Quit with **`Mod + Shift + q`**.

`Mod` means **Super (Logo)** or **Alt**. Press **`Mod + h`** for the built-in cheat sheet,
tap `Mod` alone to toggle the Spotlight launcher, and press **`Mod + Return`** for a terminal.
The complete binding list is in [Keybindings](docs/keybindings.md).

## Build targets

| Command | Result |
| :--- | :--- |
| `make` | Build all four native programs and `build/test_purpwm_plus` |
| `make protocols` | Generate Wayland protocol sources and the embedded DejaVu Sans Mono font |
| `make compositor` | Build only `build/purpwm+` |
| `make tools` | Build the polkit agent, portal, and keyring helper |
| `make test` | Build and run the deterministic C unit tests |
| `make check-headers` | Check standalone headers and client/server Wayland symbol separation |
| `make check` | Run `check-headers` and `test` |
| `make install` | Install the four programs and licenses under `PREFIX` (not the session files) |
| `make uninstall` | Remove files installed by `make install` |
| `make clean` | Remove `build/` |

See [Installation](docs/installation.md) for dependencies, backend selection, `DESTDIR`
packaging, and the full session installer.

## Configuration

Like `dwm`, purpwm+ keeps its theme, geometry, commands, and window rules in a compile-time
[`include/config.h`](include/config.h). Edit it and run `make`; incremental rebuilds are
intended to take only a couple of seconds on a typical development machine.

Runtime configuration is separate:

- `session.conf` supplies environment defaults to `start-purpwm+`.
- `${XDG_CONFIG_HOME:-~/.config}/purpwm+/monitors.json` stores display layouts.
- `$PURPWM_IPC_SOCKET` accepts newline-delimited JSON commands such as `get_monitors`,
  `apply_monitors`, `focus_monitor`, and `move_window_to_monitor`.

The legacy `~/.config/purpwm` display settings are read as a fallback; new settings are
written under `~/.config/purpwm+`. See [Configuration](docs/configuration.md).

## Documentation

| Guide | Contents |
| :--- | :--- |
| [Installation](docs/installation.md) | Requirements, builds, installation, login sessions, portal and keyring setup |
| [Configuration](docs/configuration.md) | `include/config.h`, environment variables, scaling, commands, monitors, and IPC |
| [Keybindings](docs/keybindings.md) | Keyboard, mouse, touch, hardware keys, and status-bar actions |
| [Architecture](docs/architecture.md) | C modules, backends, protocols, rendering, input, IPC, and test layering |
| [Troubleshooting](docs/troubleshooting.md) | Logs, safe testing, build failures, session access, fonts, and helpers |

## License

[MIT](LICENSE). The bundled DejaVu Sans Mono font has its own license in
[`assets/fonts/DejaVuSansMono-LICENSE`](assets/fonts/DejaVuSansMono-LICENSE).
