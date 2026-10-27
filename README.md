# yamashell

Small Wayland compositor and shell for the Nokia E6, part of [rapuyama-linux](https://github.com/fluffleswaffles/rapuyama-linux). Clients draw into `wl_shm` buffers, yamashell keeps one texture per surface, uploads only the damage and composes the scene with GLES 2 on the phone's VideoCore (`libvcgl` + `/dev/vc-khrn` from e6-linux). Output is two fullscreen RGB565 EGL surfaces presented in turn.

Status: early. Works on the phone with alacritty (drawn on the VC through libvcgl), touch and the keypad. The shell UI (home, app menu, shade, status bar) is a first draft. Landscape 640x480 (E6) and portrait (360x640, N8/E7) layouts, checked on the PC; only the E6 panel is driven by the kernel so far.

## Features

- `wl_compositor`, `wl_subcompositor`, `wl_shm`, `xdg-shell`, `wl_seat` (keyboard, pointer, touch), `wl_data_device` (clipboard), `xdg-decoration` (always server side, so apps draw no title bar)
- evdev input read directly, XKB keymap, tty1 owned while running
- home screen with the terminal, app menu with every installed app (`.desktop` files, the app's own PNG or SVG icon, SVG rasterized with the bundled [nanosvg](third_party/nanosvg)), shade, status bar that stays on top of apps; text via FreeType glyph atlas
- the app menu and the shade's notes scroll: drag, throw, springy ends; at the top of the menu a pull down closes it, at the end of the notes a push up closes the shade; the D-pad scrolls the focus into view
- the shade follows battery, charger, usb shell and memory card changes (kernel uevents, plus a 10 s check)
- notifications: yamashell is the session bus `org.freedesktop.Notifications` server, so `notify-send` and libnotify apps show up; a new one pops up as a toast (tap = shade, push up = hide, critical ones stay), in the shade it is swiped away sideways (or OK on it with the keys). The icon comes from the `image-path` hint, `app_icon` (name or path) or the sending app's .desktop entry; the status bar shows one small icon per app with notes waiting. session.sh starts the `dbus-daemon`
- the shade has brightness and volume sliders (drag, or left / right on the focused one). Brightness goes to the first `/sys/class/backlight` device (`rapuyama-vc` on the E6, never below 5%), volume is only remembered until the phone has an audio driver
- system apps live in `apps/`, each `apps/<name>/` builds into `yamashell-<name>`. They share `apps/common`: a window drawn with GLES on the VC through `libvcgl` (wayland-egl, only quads cross the bus) in the shell's look (its fonts, icons, colours and the same springs, `motion.c`), 1.25x bigger than the pixels, a glyph atlas and a frame loop that only runs while something moves
- settings survive a reboot in `$XDG_CONFIG_HOME/yamashell/settings.conf` (`~/.config/...`), `key=value` lines written by `config.c`: `wallpaper`, `brightness` (put back on the panel when the shell starts), `volume`. The shell watches the file and loads a new wallpaper at once
- `yamashell-settings` (`apps/settings`): sections Connections, Sounds and vibration, Notifications, Display, Wallpapers, Lock screen, Security, Apps, About phone. A page slides in over the list, the list throws and springs back at its ends, Display and Sounds carry the same sliders as the shade, Wallpapers picks the home screen picture from `wallpaper.jpg` and `wallpapers/` next to it. Apps lists the menu's apps; an app's page shows its size (its apk package's installed size, or the program file without an apk database) and opens it, uninstall is a placeholder. About phone shows the model with a picture of the phone (`yamashell-device-{e6,n8,e7}.svg` in `assets/icons`, picked by the devicetree's `compatible`), the rapuyama-linux version from `/etc/rapuyama-release`, processor, graphics, memory, zram, mounted storage, battery, kernel and userspace. The other sections are placeholders for now.
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

Both write `yamashell` and `yamashell-settings` into the out dir. Host needs `wayland-scanner` and the dev packages for wayland-server, wayland-client, xkbcommon, egl, glesv2, freetype2, libpng, libjpeg, libsystemd (sd-bus). `SANITIZE=1` builds the host binary with ASan/UBSan.

The phone build expects to sit at `linux/yamashell` in e6-linux: it uses e6-linux's alpine sysroot (`linux/initramfs/sysroot.sh`) and links `libvcgl` from the `vc-khrn` build next to the out dir. Normally `linux/kernel/build-vc.sh` builds it and puts it into the initramfs together with `session.sh` and `assets/`; `yamashell-settings` goes next to `yamashell` in `/usr/bin` (its menu entry is `assets/applications/yamashell-settings.desktop`, the icon `assets/icons/hicolor/scalable/apps/yamashell-settings.svg`). The apps link libvcgl, libpng and libjpeg too (wallpaper thumbnails), and `assets/wallpapers/` has to be installed next to `wallpaper.jpg`.

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
| `--restore-console` | give tty1 back and exit |

| Variable | Meaning |
| --- | --- |
| `YAMASHELL_STATS` | file with cumulative frame / upload / present counters, updated once a second |
| `YAMASHELL_SIZE` | output size `WxH`, default `640x480`; session.sh takes it from `yamashell.size=` on the kernel command line |
| `YAMASHELL_FPS` | repaint rate cap, 30-240 (session.sh unsets it) |
| `XDG_DATA_DIRS` | where `applications/*.desktop`, `icons/hicolor/*/apps/*.png` and `icons/hicolor/scalable/apps/*.svg` are looked up, after `/usr/share/yamashell/` |
| `YAMASHELL_WALLPAPER`, `YAMASHELL_FONT`, `YAMASHELL_FONT_LIGHT`, `YAMASHELL_ICONS` | replace the default assets, see [assets/README.md](assets/README.md) |

`session.sh` (installed as `yamashell-session`) waits for `/dev/vc-khrn`, starts the compositor on `wayland-vc`. Other clients connect with:

```sh
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-vc alacritty
```

## License

GPL-2.0-only, see [LICENSE](LICENSE). Bundled fonts, icons and wallpaper keep their own licenses, see [assets/README.md](assets/README.md); nanosvg is under the zlib license, see [third_party/nanosvg/LICENSE.txt](third_party/nanosvg/LICENSE.txt).