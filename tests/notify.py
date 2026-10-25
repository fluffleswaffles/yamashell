#!/usr/bin/env python3
# notifications over d-bus: replace, timeout, critical, CloseNotification, tap to dismiss
# runs the host build on a private session bus: dbus-run-session -- tests/notify.py
import os, shutil, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix='ysn', dir='/tmp') # short: the socket path has a 108 byte limit
assets = os.path.join(HERE, 'assets')
subprocess.run([os.path.join(HERE, 'build.sh'), os.path.join(work, 'build'), 'host'], check=True)
env = dict(os.environ, XDG_RUNTIME_DIR=work, XDG_DATA_DIRS=assets, YAMASHELL_FONT=assets + '/Ubuntu-Regular.ttf',
	YAMASHELL_FONT_LIGHT=assets + '/Ubuntu-Light.ttf', YAMASHELL_ICONS=assets + '/MaterialIcons-Regular.ttf')
closed = os.path.join(work, 'closed.txt')
mon = subprocess.Popen(['dbus-monitor', "interface='org.freedesktop.Notifications',member='NotificationClosed'"], stdout=open(closed, 'w'))
r, w = os.pipe()
shell = subprocess.Popen([os.path.join(work, 'build', 'yamashell'), '--headless', '--ui', '--socket', 'wl-test', '--input-fd', str(r)], env=env, pass_fds=[r])
os.close(r)

def notify(*args):
	return int(subprocess.run(['notify-send', '-p'] + list(args), env=env, check=True, capture_output=True, text=True).stdout)

def call(method, *args):
	subprocess.run(['gdbus', 'call', '--session', '-d', 'org.freedesktop.Notifications', '-o', '/org/freedesktop/Notifications',
		'-m', 'org.freedesktop.Notifications.' + method] + list(args), check=True, capture_output=True)

def events(*evs):
	for ev in evs + ((0, 0, 0),):
		os.write(w, struct.pack('@llHHi', 0, 0, *ev))

def swipe(x0, y, x1, steps=8):
	events((3, 0x2f, 0), (3, 0x39, 1), (3, 0x35, x0), (3, 0x36, y))
	for i in range(1, steps + 1):
		time.sleep(.02)
		events((3, 0x35, x0 + (x1 - x0) * i // steps))
	time.sleep(.3 if steps > 1 else .05) # slow end: no flick
	events((3, 0x39, -1))

def tap(x, y):
	swipe(x, y, x, 1)

try:
	# the bus name is taken before the wayland socket shows up
	for _ in range(100):
		if os.path.exists(os.path.join(work, 'wl-test')):
			break
		time.sleep(.1)
	call('GetServerInformation')
	a = notify('Download', '10%')
	assert notify('-r', str(a), 'Download', '90%') == a, 'replace keeps the id'
	b = notify('-t', '500', 'Short', 'expires')
	c = notify('-u', 'critical', 'Battery low', 'stays')
	d = notify('Closed by call')
	call('CloseNotification', str(d))
	time.sleep(1.5)
	# status bar tap opens the shade, the critical note (newest left) is the top row
	tap(320, 10)
	time.sleep(1)
	tap(320, 180) # a tap keeps it
	swipe(320, 180, 400) # too short, springs back
	time.sleep(1)
	swipe(320, 180, 600) # out
	time.sleep(1.5)
finally:
	shell.terminate()
	shell.wait()
	time.sleep(.3)
	mon.terminate()

nums = [int(l.split()[1]) for l in open(closed) if l.strip().startswith('uint32')]
got = set(zip(nums[0::2], nums[1::2]))
want = {(b, 1), (d, 3), (c, 2)}
assert want <= got and not any(i == a for i, _ in got), f'closed {sorted(got)}, want {sorted(want)}'
shutil.rmtree(work, ignore_errors=True)
print('notify: ok')