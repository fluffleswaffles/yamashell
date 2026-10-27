# yamashell assets

Installed to `/usr/share/yamashell/` by `linux/kernel/build-vc.sh`. The fonts and the wallpaper can be replaced at runtime with an environment variable.

| File | Variable | Source | License |
| --- | --- | --- | --- |
| `wallpaper.jpg` | `YAMASHELL_WALLPAPER` (JPEG or PNG, any size, cropped to cover the screen) | GNOME `adwaita-d.jpg` from [gnome-backgrounds 42.0](https://gitlab.gnome.org/GNOME/gnome-backgrounds/-/tree/42.0/backgrounds), scaled down to 640x480 | CC BY-SA 3.0, GNOME Foundation |
| `Ubuntu-Regular.ttf`, `Ubuntu-Light.ttf` | `YAMASHELL_FONT`, `YAMASHELL_FONT_LIGHT` | [google/fonts](https://github.com/google/fonts/tree/bed55e0de5cfa8f5fb94f9a1f49409b5ec504469/ufl/ubuntu) | Ubuntu Font Licence 1.0, see `UFL.txt` |
| `MaterialIcons-Regular.ttf` | `YAMASHELL_ICONS` | [material-design-icons 4.0.0](https://github.com/google/material-design-icons/tree/4.0.0/font) | Apache License 2.0, see `LICENSE-material-icons.txt` |

Icons are looked up by Unicode code point (the font has no glyph names); the code points in `ui.c` come from the font's `codepoints` file and were checked against this exact font.

`applications/Alacritty.desktop` overrides the stock entry: alacritty shows as "Terminal" (the app marked `TerminalEmulator` goes on the home screen). Its icon is the stock scalable one from the alacritty package. The font size and the renderer come from `/etc/alacritty/alacritty.toml` (`linux/initramfs/alacritty.toml` in the e6-linux tree).

`wallpapers/` is listed by the settings app next to whichever file is the default wallpaper (`wallpaper.jpg`, or `YAMASHELL_WALLPAPER`'s directory); any JPEG or PNG put there shows up in Settings > Wallpapers.