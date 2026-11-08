# Keybindings

`Mod` is **Super (Logo)** or **Alt**. The bindings are implemented in
[`src/input.c`](../src/input.c); related commands, distances, and other defaults are in
[`include/config.h`](../include/config.h). Press **`Mod + h`** at any time for the built-in
cheat sheet. Press `Mod + h` again, `Escape`, or click/tap to close it.

## Session

| Keys | Action |
| :--- | :--- |
| `Mod + h` | Show/hide the keybinding overlay |
| `Mod + Shift + l` or `Mod + Ctrl + l` | Lock with the built-in PAM screen locker |
| `Mod + Shift + q` | Quit purpwm+ and return to the login screen/host session |
| `Ctrl + Alt + F1` … `Ctrl + Alt + F12` | Switch Linux virtual terminal in a native `--drm` session |
| `Mod + q` | Close the focused window |

The lock can also be triggered by `XF86ScreenSaver` or by a signal:

```sh
kill -USR2 $(pidof 'purpwm+')
```

While locked, type your account password and press `Enter`. `BackSpace` deletes one character;
`Escape` or `Ctrl + u` clears the password field. Authentication uses the first applicable
PAM service from `PURPWM_PAM_SERVICE`, `purpwm+`, `purpwm`, `vlock`, or `login`.

## Launching and session tools

| Keys | Action |
| :--- | :--- |
| Tap `Mod` alone | Toggle Spotlight (or the configured launcher) |
| `Mod + p` | Toggle the launcher |
| `Mod + Return` | Open a terminal |
| `Mod + s` or ``Mod + ` `` | Show/hide the drop-down scratchpad terminal |
| `Mod + Escape` | Open the system monitor (`btop` or `top`) |
| `Mod + w` | Open the Wi-Fi manager |
| `Mod + Shift + w` | Toggle the Wi-Fi radio |
| `Mod + Shift + b` | Open the Bluetooth manager |
| `Mod + Ctrl + b` | Toggle Bluetooth power |

## Focus and windows

| Keys | Action |
| :--- | :--- |
| `Mod + j` / `Mod + k` | Focus next / previous window |
| `Mod + ,` / `Mod + .` | Focus previous / next monitor |
| `Mod + Shift + ,` / `Mod + Shift + .` | Move focused window to previous / next monitor |
| `Mod + Shift + Return` | Zoom: swap with the master window; on the master, swap with the next window |
| `Mod + Arrow` | Swap with the spatial neighbor in that direction (hopping to an adjacent monitor at the screen edge), or nudge a floating window by 40 logical pixels |
| `Mod + Shift + Arrow` | Resize the right/bottom edge or adjacent tiled split; a tap moves 16 px, holding accelerates |
| `Mod + Shift + Space` | Toggle floating |
| `Mod + Shift + f` or `Mod + F11` | Toggle fullscreen |
| `Mod + o` | Toggle fully opaque mode for video, images, or games |

Focus follows the pointer when it **enters** another window. Opening a window does not cause
focus to jump again merely because the pointer has not moved; menus and active drags do not
steal focus.

## Layouts

| Keys | Action |
| :--- | :--- |
| `Mod + t` | Tile `[]=` |
| `Mod + u` | Centered Master `|M|` |
| `Mod + g` | Grid `###` |
| `Mod + m` | Monocle `[M]` |
| `Mod + f` | Floating `><>` |
| `Mod + Space` | Switch between current and previous layout |
| `Mod + Ctrl + Space` | Cycle through all layouts |
| `Mod + [` / `Mod + ]` (or `Mod + l`) | Shrink / grow the master area by 0.05 |
| `Mod + i` / `Mod + d` | Increase / decrease the number of master windows |

Layout, master factor, and master count are remembered independently for each tag.

## Tags (workspaces)

Every monitor has its own independent `1`…`9` tag view, per-tag layout state, and status bar.
Tags are bitmasks: a window or view can belong to more than one tag.

| Keys | Action |
| :--- | :--- |
| `Mod + 1` … `Mod + 9` | View one tag |
| `Mod + Ctrl + 1` … `Mod + Ctrl + 9` | Toggle a tag in the current view |
| `Mod + Shift + 1` … `Mod + Shift + 9` | Move the focused window to one tag |
| `Mod + Ctrl + Shift + 1` … `Mod + Ctrl + Shift + 9` | Toggle a tag on the focused window |
| `Mod + 0` | View all tags |
| `Mod + Shift + 0` | Show the focused window on all tags |
| `Mod + Tab` | Return to the previously viewed tag set |

## Appearance

| Keys | Action |
| :--- | :--- |
| `Mod + Ctrl + =` / `Mod + Ctrl + -` | Increase / decrease UI scale in 0.25 steps and save it |
| `Mod + =` / `Mod + -` | Increase / decrease gaps |
| `Mod + Shift + g` | Toggle gaps |
| `Mod + Ctrl + 0` | Reset gaps to their compiled defaults |
| `Mod + b` | Show/hide the status bar |

Live scale changes are saved under `${XDG_CONFIG_HOME:-~/.config}/purpwm+/scale`. On multiple
monitors, per-output scales are managed in `monitors.json`; see
[Configuration](configuration.md#scale-hidpi).

## Hardware and screenshot keys

These work without `Mod`.

| Key | Action |
| :--- | :--- |
| Volume up / down / mute | Adjust the default sink with `wpctl`, falling back to `pactl` |
| Microphone mute | Toggle the default source with `wpctl` / `pactl` |
| Brightness up / down | Use `brightnessctl`, or the logind backlight fallback |
| Play/pause / next / previous | Control media with `playerctl` |
| `XF86ScreenSaver` | Lock the screen |
| `Print` | Capture the whole screen |
| `Shift + Print` or `Mod + Print` | Capture the focused window |

Screenshots are written atomically as
`~/Pictures/Screenshots/purpwm+-YYYYmmdd-HHMMSS.png` (with a numeric suffix if needed) and
offered to the clipboard as `image/png`. Files are private to the user. A script can request a
screenshot from a particular running instance with:

```sh
kill -USR1 <purpwm+-pid>
```

## Mouse and touch

| Action | Effect |
| :--- | :--- |
| `Mod + Left-drag` / `Mod + Touch-drag` | Float and move a window (after a 10 px drag threshold; releasing a dragged window at the top bar maximizes it, and dragging a maximized window down restores it) |
| `Mod + Right-drag` | Resize; for tiled windows, the nearest splits follow the pointer |
| `Mod + Middle-click` | Toggle floating |
| Tap a window, overlay, or popup | Focus it and forward native `wl_touch` events |
| Tap a status-bar tag or pill | Perform the corresponding action |
| Click or tap a notification toast | Dismiss it |

## Status bar

| Target | Left-click / tap | Right-click | Other |
| :--- | :--- | :--- | :--- |
| Tag `1`…`9` | View tag; `Shift` moves the focused window there | Toggle tag in view | Middle-click moves the focused window there |
| Layout symbol | Next layout | Previous layout | |
| Window-title area | Focus next window | | Middle-click opens a terminal |
| `Flameshot` button | Open `flameshot gui` through the native Screenshot portal | | |
| `WIFI` pill | Wi-Fi manager | Toggle Wi-Fi radio | |
| `BT` pill | Bluetooth manager | Toggle Bluetooth power | |
| `VOL` pill | Mute/unmute | Open the audio mixer | Scroll adjusts volume |
| `BAT` / `CPU` / `MEM` / clock | Open the system monitor | | |
| Empty bar area | | | Scroll switches tags |

The Flameshot button appears when the bar has enough space. Approve the portal's sharing
prompt, then select and annotate the capture in Flameshot. `Print` uses the compositor's
built-in capture path directly and does not open Flameshot.
