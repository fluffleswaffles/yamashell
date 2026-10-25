#!/usr/bin/env bash
# host = mesa surfaceless, arm = the phone's libvcgl
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOP="$HERE/../.."
OUT="${1:?usage: build.sh <out dir> [host]}"
mkdir -p "$OUT"
wayland-scanner server-header "$HERE/protocol/xdg-shell.xml" "$OUT/xdg-shell-server.h"
wayland-scanner client-header "$HERE/protocol/xdg-shell.xml" "$OUT/xdg-shell-client.h"
wayland-scanner private-code "$HERE/protocol/xdg-shell.xml" "$OUT/xdg-shell.c"
wayland-scanner server-header "$HERE/protocol/xdg-decoration-unstable-v1.xml" "$OUT/xdg-decoration-server.h"
wayland-scanner private-code "$HERE/protocol/xdg-decoration-unstable-v1.xml" "$OUT/xdg-decoration.c"
if [ "${2:-}" = host ]; then
	CC=("${HOST_CC:-cc}" -DYAMASHELL_HOST)
	if [ -n "${SANITIZE:-}" ]; then
		CC=("${HOST_CC:-clang}" -DYAMASHELL_HOST -fsanitize=address,undefined -fno-omit-frame-pointer)
	fi
	read -ra FLAGS <<< "$(pkg-config --cflags --libs wayland-server xkbcommon egl glesv2 freetype2 libpng libjpeg libsystemd) -lm"
else
	SYSROOT="$TOP/work/sysroot-alpine"
	"$TOP/linux/initramfs/sysroot.sh" "$SYSROOT"
	CC=(clang --target=armv6-alpine-linux-musleabihf -march=armv6kz -mfloat-abi=hard -mfpu=vfp
		--sysroot="$SYSROOT" --ld-path=/usr/bin/arm-linux-gnu-ld -static-libgcc)
	[ -z "${TEST_INPUT:-}" ] || CC+=(-DYAMASHELL_TEST)
	FLAGS=(-I"$SYSROOT/usr/include/freetype2" -I"$SYSROOT/usr/include/elogind" -L"$OUT/../vc-khrn" -lvcgl -lwayland-server -lxkbcommon -lfreetype -lpng16 -ljpeg -lelogind -lm)
fi
"${CC[@]}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -I"$OUT" -I"$HERE" \
	"$HERE/server.c" "$HERE/render.c" "$HERE/shell.c" "$HERE/input.c" "$HERE/data.c" "$HERE/console.c" "$HERE/ui.c" "$HERE/draw.c" "$HERE/status.c" "$HERE/shade.c" "$HERE/tasks.c" "$HERE/apps.c" "$HERE/notify.c" "$OUT/xdg-shell.c" "$OUT/xdg-decoration.c" \
	-o "$OUT/yamashell" "${FLAGS[@]}"