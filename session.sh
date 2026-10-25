#!/bin/sh
# the compositor owns the display; clients use shm, the VC composes their textures
unset YAMASHELL_FPS
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
export WAYLAND_DISPLAY=wayland-vc SHELL=/bin/sh
# our foot.ini (bigger font) comes before the stock one
export XDG_CONFIG_DIRS=/usr/share/yamashell/xdg:/etc/xdg
export YAMASHELL_STATS="$XDG_RUNTIME_DIR/yamashell.stats"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# yamashell.size=360x640 on the kernel command line for portrait panels
size=$(sed -n 's/.*yamashell\.size=\([0-9]*x[0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$size" ] || export YAMASHELL_SIZE="$size"
compositor=
cleanup() {
	trap - EXIT TERM INT
	[ -z "$compositor" ] || { kill "$compositor" 2>/dev/null; wait "$compositor" 2>/dev/null; }
	/usr/bin/yamashell --restore-console
}
trap cleanup EXIT
trap 'exit 0' TERM INT
n=0
while [ ! -c /dev/vc-khrn ]; do
	n=$((n + 1))
	[ "$n" -lt 30 ] || { echo "no /dev/vc-khrn after 30 s"; exit 1; }
	sleep 1
done
# one terminal at start; after that apps are started from the shell's menu
/usr/bin/yamashell --socket "$WAYLAND_DISPLAY" --start-terminal &
compositor=$!
n=0
while [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; do
	kill -0 "$compositor" 2>/dev/null || { wait "$compositor"; exit 1; }
	n=$((n + 1))
	[ "$n" -lt 15 ] || exit 1
	sleep 1
done
wait "$compositor"
status=$?
compositor=
exit "$status"