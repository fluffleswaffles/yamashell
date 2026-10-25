# yamashell

Small Wayland compositor and shell for the Nokia E6, part of [rapuyama-linux](https://github.com/fluffleswaffles/rapuyama-linux). Clients draw into `wl_shm` buffers, yamashell keeps one texture per surface, uploads only the damage and composes the scene with GLES 2 on the phone's VideoCore (`libvcgl` + `/dev/vc-khrn` from e6-linux). Output is two fullscreen RGB565 EGL surfaces presented in turn.

Status: early. Works on the phone with foot, touch and the keypad. The shell UI (home, app menu, shade, status bar) is a first draft. Landscape 640x480 (E6) and portrait (360x640, N8/E7) layouts, checked on the PC; only the E6 panel is driven by the kernel so far.

## Features

- `wl_compositor`, `wl_subcompositor`, `wl_shm`, `xdg-shell`, `wl_seat` (keyboard, pointer, touch), `wl_data_device` (clipboard), `xdg-decoration` (always server side, so apps like foot draw no title bar)
- evdev input read directly, XKB keymap, tty1 owned while running
- home screen with the terminal, app menu with every installed app (`.desktop` files, the app's own PNG icon), shade, status bar that stays on top of apps; text via FreeType glyph atlas
- the app menu and the shade's notes scroll: drag, throw, springy ends; at the top of the menu a pull down closes it, at the end of the notes a push up closes the shade; the D-pad scrolls the focus into view
- the shade follows battery, charger, usb shell and memory card changes (kernel uevents, plus a 10 s check)
- notifications: yamashell is the session bus `org.freedesktop.Notifications` server, so `notify-send`, libnotify apps and foot's notifications show up; a new one pops up as a toast (tap = shade, push up = hide, critical ones stay), in the shade it is swiped away sideways (or OK on it with the keys). The icon comes from the `image-path` hint, `app_icon` (name or path) or the sending app's .desktop entry; the status bar shows one small icon per app with notes waiting. session.sh starts the `dbus-daemon`
- sheets move on a spring: they follow the finger, a fling decides open or closed, and they land with a small bounce
- touch: left/right edge = back (pull until the arrow pops, Esc to the app), bottom handle = home, pulled further past a dead zone = running apps (on home too), dragging the home bar = app menu, status bar = shade, long press on a running app's icon = close it
- running apps: a card per window with its live picture; tap opens, swipe sideways browses, swipe a card up closes the app
- keys: Home = home / app menu (long = running apps: left/right, OK opens, up or long End closes), Messages = app menu (long = shade), End = back (long = close the app), D-pad + OK to navigate, Alt+Tab = next window

Not done: Wayland EGL clients, dmabuf, multiple outputs.

## Build

```sh
./build.sh <out dir> host # pc, mesa surfaceless EGL, for development
./build.sh <out dir> # phone, needs the e6-linux tree around it
```

Host needs `wayland-scanner` and the dev packages for wayland-server, xkbcommon, egl, glesv2, freetype2, libpng, libjpeg, libsystemd (sd-bus). `SANITIZE=1` builds the host binary with ASan/UBSan. Tests: `tests/scroll.py`, `dbus-run-session -- tests/notify.py`.

The phone build expects to sit at `linux/yamashell` in e6-linux: it uses e6-linux's alpine sysroot (`linux/initramfs/sysroot.sh`) and links `libvcgl` from the `vc-khrn` build next to the out dir. Normally `linux/kernel/build-vc.sh` builds it and puts it into the initramfs together with `session.sh` and `assets/`.

## Try it on a PC

```sh
./preview.py # or ./preview.py --size 360x640
```

Builds the host binary into `~/.cache/yamashell-preview` and shows yamashell in a window: the mouse is the touchscreen, the keyboard is the keyboard, F1 / F2 / F3 are the E6 Home / Messages / End keys (hold for the long press: running apps / shade / close). The app menu lists the PC's own apps; any Wayland app that can draw with `wl_shm` opens inside with `WAYLAND_DISPLAY=wayland-yamashell-preview <app>` (GTK 4 apps with `GSK_RENDERER=cairo`, there is no dmabuf). Notifications go to a private session bus that preview.py starts (the desktop's own bus already has a notification daemon), it prints the `DBUS_SESSION_BUS_ADDRESS=... notify-send <title> [text]` line to use; apps started from the shell's menu get that bus too. Needs Python's tkinter (`python3-tkinter` on Fedora).

## Run

```sh
yamashell --socket wayland-vc --ui
```

| Option | Meaning |
| --- | --- |
| `--socket <name>` | Wayland socket name |
| `--ui` | draw the shell UI (off with `--headless`) |
| `--headless` | no output, for tests |
| `--capture <file>` | write frames to a file (tests) |
| `--input-fd <fd>` | read input events from an fd instead of evdev (tests) |
| `--start-terminal` | start the terminal app once the socket is up |
| `--restore-console` | give tty1 back and exit |

| Variable | Meaning |
| --- | --- |
| `YAMASHELL_STATS` | file with cumulative frame / upload / present counters, updated once a second |
| `YAMASHELL_SIZE` | output size `WxH`, default `640x480`; session.sh takes it from `yamashell.size=` on the kernel command line |
| `YAMASHELL_FPS` | repaint rate cap, 30-240 (session.sh unsets it) |
| `XDG_DATA_DIRS` | where `applications/*.desktop` and `icons/hicolor/*/apps/*.png` are looked up, after `/usr/share/yamashell/` |
| `YAMASHELL_WALLPAPER`, `YAMASHELL_FONT`, `YAMASHELL_FONT_LIGHT`, `YAMASHELL_ICONS` | replace the default assets, see [assets/README.md](assets/README.md) |

`session.sh` (installed as `yamashell-session`) waits for `/dev/vc-khrn`, starts the compositor on `wayland-vc` with one terminal and points foot at the bundled `foot.ini`. Other clients connect with:

```sh
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-vc foot
```

## License

GPL-2.0-only, see [LICENSE](LICENSE). Bundled fonts, icons and wallpaper keep their own licenses, see [assets/README.md](assets/README.md).