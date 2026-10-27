// SPDX-License-Identifier: GPL-2.0-only
#ifndef YAMASHELL_H
#define YAMASHELL_H
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-server.h"

/* output size, YAMASHELL_SIZE=WxH, default the E6 640x480 */
extern int screen_w, screen_h;
struct server;
struct ui;
struct power;
struct rect{int64_t x, y, w, h;};
struct surface;
struct damage_rect{struct wl_list link;struct rect rect;bool buffer;};
struct surface_state{
	struct surface *surface;
	struct wl_resource *buffer;
	struct buffer_use *use;
	struct wl_listener buffer_destroy;
	struct wl_list callbacks, damages;
	int scale;
	int64_t x, y;
	struct rect opaque_region;
	bool attached, scale_set, opaque_set;
};
struct surface{
	struct server *server;
	struct wl_resource *resource, *xdg, *role;
	struct wl_list link, callbacks;
	struct surface *parent, *sibling;
	struct surface_state pending, cache;
	struct surface_state shown, retired;
	GLuint texture;
	int width, height, scale;
	int64_t x, y, pending_x, pending_y, content_x, content_y;
	int geometry_x, geometry_y;
	struct rect opaque_region;
	bool rgb565, vc;
	bool mapped, opaque, configured, acked, cursor;
	bool subsurface, sync, cached, position_pending, place_pending, above;
	uint32_t configure_serial;
};
struct input_resource{
	struct server *server;
	struct wl_list link;
	struct wl_resource *resource;
};
struct server{
	void *clipboard;
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_event_source *repaint, *stats;
	struct wl_list surfaces, keyboards, pointers, touches, devices, outputs, buffers;
	struct surface *focus, *pointer_focus, *touch_focus, *cursor;
	int hotspot_x, hotspot_y, ntouches, input_fd;
	bool tab_grabbed;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *keys;
	int keymap_fd, console_fd, console_mode, keyboard_mode;
	size_t keymap_size;
	uint32_t pressed[256], npressed;
	double pointer_x, pointer_y;
	bool repaint_pending, dirty, headless, stopped, retire;
	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLSurface egl_surface;
	GLuint program, quad, opaque_program;
	GLint alpha, native, bounds, opaque_bounds;
	const char *capture;
	uint64_t frames, uploaded, uploads, upload_ms, present_ms;
	uint32_t repaint_started,repaint_interval;
	int inset; /* status bar height above the windows, 0 without the shell UI */
	float zoom, zoom_x, zoom_y; /* windows scaled by zoom around (zoom_x, zoom_y): open / home animation */
	const char *stats_path;
	struct ui *ui;
	struct power *power;
};
uint32_t milliseconds(void);
void power_init(struct server *s);
void power_finish(struct server *s);
void power_sleep(struct server *s);
bool power_asleep(struct server *s);
bool power_key(struct server *s, uint32_t key, uint32_t state);
bool power_touch(struct server *s, int id, int state);
bool power_pointer(struct server *s);
void schedule_repaint(struct server *s, bool dirty);
void surface_apply(struct surface *v);
bool surface_synced(struct surface *v);
bool surface_visible(struct surface *v);
void surface_focus(struct surface *v);
void surface_focus_next(struct server *s);
void surface_restack(struct surface *v,struct surface *sibling,bool above);
void surface_position(struct surface *v, int64_t *x, int64_t *y);
void shell_init(struct server *s);
void data_init(struct server *s);
void data_focus(struct server *s);
void data_finish(struct server *s);
void console_init(struct server *s);
void console_finish(struct server *s);
void console_restore(void);
void input_init(struct server *s);
void input_finish(struct server *s);
void input_focus(struct server *s, struct surface *v);
void input_key(struct server *s, uint32_t time, uint32_t key, uint32_t state);
void input_pointer(struct server *s, uint32_t time, double x, double y);
void input_button(struct server *s, uint32_t time, uint32_t button, uint32_t state);
void input_touch(struct server *s, uint32_t time, int id, double x, double y, int state);
void input_key_tap(struct server *s, uint32_t key);
#define ICON 64 /* app icons in the menu */
#include "apps.h"
/* a notification from the session bus, expire = milliseconds() when it closes itself, 0 = stays */
#define MAX_NOTES 32
/* icon = icon name or path from the sender (image-path hint, app_icon, desktop-entry), may be empty */
struct note{uint32_t id, expire;char app[48], icon[160], title[96], body[192];bool critical;};
void notify_init(struct server *s);
void notify_finish(void);
int notify_list(const struct note **out);
void notify_close(uint32_t id);
/* new_id = the note that just came or changed, 0 when one only went away */
void ui_notes_changed(struct server *s, uint32_t new_id);
void ui_init(struct server *s);
void ui_finish(struct server *s);
bool ui_key(struct server *s, uint32_t key, uint32_t state);
bool ui_touch(struct server *s, int id, double x, double y, int state);
bool ui_draw_under(struct server *s);
void ui_draw_over(struct server *s);
bool ui_animating(struct server *s);
void ui_window_mapped(struct server *s, struct surface *v);
bool renderer_init(struct server *s);
bool renderer_upload(struct surface *v, struct wl_shm_buffer *buffer, struct rect r, bool fresh, bool opaque);
/* the texture is bound when the buffer is made */
struct vc_buffer{struct server *server; uint32_t id0, id1; int width, height; uint32_t format; GLuint texture;};
bool renderer_vc_buffer(struct vc_buffer *b);
void renderer_vc_free(struct vc_buffer *b);
void renderer_import(struct surface *v, const struct vc_buffer *b);
bool renderer_frame(struct server *s);
void renderer_quad(struct server *s, float l, float t, float r, float b, bool opaque);
void renderer_thumb(struct surface *v, float x, float y, float w, float h);
void renderer_drop(struct surface *v);
void renderer_finish(struct server *s);
#endif