#!/usr/bin/python3
# try yamashell on the pc: builds the host binary, shows its frames in a window,
# the mouse is the touchscreen, the keyboard is the keyboard, F1 / F2 / F3 are
# the E6 Home / Messages / End keys (hold them for the long press: running
# apps / shade / close the app)
# notifications go to a bus of its own, the desktop's one already has a notification daemon
# usage: preview.py [--size 360x640]
import os
import struct
import subprocess
import sys
import time
import tkinter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get('XDG_CACHE_HOME', os.path.expanduser('~/.cache')), 'yamashell-preview')
FRAME = os.path.join(OUT, 'frame.ppm')
BUS = 'unix:path=' + os.path.join(OUT, 'bus')
SOCKET = 'wayland-yamashell-preview'
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0x2f, 0x39, 0x35, 0x36
# tk keysym -> E6 keys that a pc keyboard does not have
PHONE = {'F1': 172, 'F2': 0x1ae, 'F3': 0x1be}

size = '640x480'
if len(sys.argv) == 3 and sys.argv[1] == '--size':
	size = sys.argv[2]
elif len(sys.argv) != 1:
	sys.exit('usage: preview.py [--size 360x640]')
width, height = map(int, size.split('x'))

os.makedirs(OUT, exist_ok=True)
subprocess.run([os.path.join(HERE, 'build.sh'), os.path.join(OUT, 'build'), 'host'], check=True)
try:
	os.unlink(FRAME)
except FileNotFoundError:
	pass
assets = os.path.join(HERE, 'assets')
data_dirs = os.environ.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share'
try:
	os.unlink(os.path.join(OUT, 'bus'))
except FileNotFoundError:
	pass
bus = subprocess.Popen(['dbus-daemon', '--session', '--address=' + BUS, '--nofork', '--nopidfile', '--nosyslog'])
for _ in range(50):
	if os.path.exists(os.path.join(OUT, 'bus')):
		break
	time.sleep(.1)
env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=BUS, YAMASHELL_SIZE=size, XDG_DATA_DIRS=assets + ':' + data_dirs,
	XDG_CONFIG_DIRS=os.path.join(assets, 'xdg') + ':/etc/xdg',
	YAMASHELL_FONT=os.path.join(assets, 'Ubuntu-Regular.ttf'), YAMASHELL_FONT_LIGHT=os.path.join(assets, 'Ubuntu-Light.ttf'),
	YAMASHELL_ICONS=os.path.join(assets, 'MaterialIcons-Regular.ttf'), YAMASHELL_WALLPAPER=os.path.join(assets, 'wallpaper.jpg'))
read_end, write_end = os.pipe()
shell = subprocess.Popen([os.path.join(OUT, 'build', 'yamashell'), '--headless', '--ui', '--start-terminal', '--socket', SOCKET,
	'--input-fd', str(read_end), '--capture', FRAME], env=env, pass_fds=[read_end])
os.close(read_end)

def send(*events):
	try:
		for kind, code, value in events + ((EV_SYN, 0, 0),):
			os.write(write_end, struct.pack('@llHHi', 0, 0, kind, code, value))
	except BrokenPipeError:
		root.destroy()

def touch(event, state):
	x, y = max(0, min(width - 1, event.x)), max(0, min(height - 1, event.y))
	if state == 1:
		send((EV_ABS, ABS_MT_SLOT, 0), (EV_ABS, ABS_MT_TRACKING_ID, 1), (EV_ABS, ABS_MT_POSITION_X, x), (EV_ABS, ABS_MT_POSITION_Y, y))
	elif state == 2:
		send((EV_ABS, ABS_MT_POSITION_X, x), (EV_ABS, ABS_MT_POSITION_Y, y))
	else:
		send((EV_ABS, ABS_MT_TRACKING_ID, -1))

# x11 autorepeat sends release+press pairs (gnome: every 30 ms): a release only counts if no press follows soon
held, pending = set(), {}

def evdev(event):
	return PHONE.get(event.keysym, event.keycode - 8)

def key_press(event):
	code = evdev(event)
	if code in pending:
		root.after_cancel(pending.pop(code))
	if code not in held:
		held.add(code)
		send((EV_KEY, code, 1))

def key_release(event):
	code = evdev(event)
	def release():
		pending.pop(code, None)
		held.discard(code)
		send((EV_KEY, code, 0))
	pending[code] = root.after(150, release)

shown = None

def poll():
	global shown, image
	if shell.poll() is not None:
		root.destroy()
		return
	try:
		stamp = os.stat(FRAME).st_mtime_ns
		if stamp != shown:
			image = tkinter.PhotoImage(file=FRAME)
			screen.configure(image=image)
			shown = stamp
	except (FileNotFoundError, tkinter.TclError):
		pass
	root.after(16, poll)

root = tkinter.Tk()
root.title('yamashell ' + size)
root.resizable(False, False)
root.geometry(f'{width}x{height}')
screen = tkinter.Label(root, bd=0, bg='black', cursor='hand2')
screen.pack(fill='both', expand=True)
screen.bind('<ButtonPress-1>', lambda e: touch(e, 1))
screen.bind('<B1-Motion>', lambda e: touch(e, 2))
screen.bind('<ButtonRelease-1>', lambda e: touch(e, 0))
root.bind('<KeyPress>', key_press)
root.bind('<KeyRelease>', key_release)
image = None
poll()
print(f'yamashell on {SOCKET}: WAYLAND_DISPLAY={SOCKET} <app> opens an app in it')
print(f'notifications: DBUS_SESSION_BUS_ADDRESS={BUS} notify-send <title> [text]')
try:
	root.mainloop()
except KeyboardInterrupt:
	pass
finally:
	shell.terminate()
	shell.wait()
	bus.terminate()
	bus.wait()