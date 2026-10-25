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
# session bus: notifications (notify-send, foot) go through it to the shell
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
# yamashell.size=360x640 on the kernel command line for portrait panels
size=$(sed -n 's/.*yamashell\.size=\([0-9]*x[0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$size" ] || export YAMASHELL_SIZE="$size"
compositor=
bus=
cleanup() {
	trap - EXIT TERM INT
	[ -z "$compositor" ] || { kill "$compositor" 2>/dev/null; wait "$compositor" 2>/dev/null; }
	[ -z "$bus" ] || kill "$bus" 2>/dev/null
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
# without a bus the shell still runs, just without notifications
[ -s /etc/machine-id ] || dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null
rm -f "$XDG_RUNTIME_DIR/bus"
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --nofork --nopidfile --nosyslog &
bus=$!
n=0
while [ ! -S "$XDG_RUNTIME_DIR/bus" ] && [ "$n" -lt 50 ]; do
	n=$((n + 1))
	sleep 0.1
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
# a pio boot is slower on screen, tell the user
[ "$(cat /sys/module/videocore/parameters/dma 2>/dev/null)" != N ] ||
	notify-send -a yamashell "Graphics in PIO mode" "videocore.dma=0, the screen updates slower"
wait "$compositor"
status=$?
compositor=
exit "$status"