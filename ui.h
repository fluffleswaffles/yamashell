// SPDX-License-Identifier: GPL-2.0-only
/* shared by the shell ui: ui.c home, app menu, gestures, keys; draw.c atlas, text, shapes, wallpaper;
 * status.c system state and the status bar; shade.c shade, toast, notifications; tasks.c running apps */
#ifndef UI_H
#define UI_H
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "yamashell.h"
#include "level.h"
#include "motion.h"

#define STATUS 30
#define EDGE 24 /* back zone at the left/right edge */
#define BOTTOM 36 /* home / app menu zone at the bottom edge */
#define BACK_DIST 110 /* finger travel that arms back / home */
#define TASKS_DEAD 90 /* past home, travel that does nothing before running apps */
#define FLICK 700 /* px/s: a quick short swipe counts as a full one */
#define HOLD 500
#define ATLAS_W 512
#define ATLAS_H 1024 /* glyphs in the top half, app icons below */
#define GLYPHS_H 512
#define ICON_SLOTS (ATLAS_W/ICON*(ATLAS_H-GLYPHS_H)/ICON)
#define NOTE_SLOTS 8 /* the last icon slots: notification icons, 40 px + 20 px side by side */
#define APP_SLOTS (ICON_SLOTS-NOTE_SLOTS)
#define ASSETS "/usr/share/yamashell/"
#define TOAST_H 64
#define TOAST_MS 4000
#define MAX_TASKS 16

/* L_APPS and L_NOTES scroll; their items belong to the app menu and the shade */
enum{L_HOME,L_MENU,L_APPS,L_SHADE,L_NOTES,L_SHADE_TOP,L_TOAST,L_STATUS,L_COUNT};
enum{A_NONE,A_LAUNCH,A_MENU,A_TOGGLE,A_CLOSE,A_NOTE,A_SLIDER};
enum{G_NONE,G_SHADE_OPEN,G_SHADE,G_MENU_OPEN,G_MENU,G_HOME,G_BACK_L,G_BACK_R,G_TAP,G_HELD,G_TASKS,G_TOAST,G_NOTE,G_MENU_SCROLL,G_SHADE_SCROLL,G_SLIDER};
enum{F_TEXT,F_LIGHT,F_ICON,F_COUNT};
struct col{float r,g,b,a;};
struct vtx{float x,y,u,v,r,g,b,a,hw,hh,rad,mode;};
struct layer{GLuint vbo;struct vtx *v;int n,cap,drawn;};
struct item{int layer,act,arg;float x,y,w,h;};
struct glyph{uint32_t cp;int face,size,x,y,w,h,left,top,adv;};
struct toggle{const char *name;uint32_t icon;bool on,na;};
struct status{int pct;bool charging,charger,usb,card;};
struct nicon{char name[160];bool ok;uint32_t used;};
struct ui{
	GLuint program,atlas,wall;
	GLint off;
	FT_Library ft;FT_Face face[F_COUNT];
	unsigned char *pixels;
	int ax,ay,arow,dirty_y0,dirty_y1;
	struct glyph glyphs[640];int nglyphs;
	struct layer layers[L_COUNT],dynamic,taskl;
	struct item items[256];int nitems;
	struct app *apps;int napps,terminal;
	struct status st;
	uint32_t toast_id;
	struct nicon nicons[NOTE_SLOTS];uint32_t nicon_tick;
	uint32_t swipe_id;float swipe_y;bool swipe_out; /* a shade note being swiped away, drawn on its own */
	bool dirty,home,keys,armed,pull_home,task_hint;
	struct spring shade,menu,pull,tasks,task_pos,hint,app,fly,toast,swipe;
	float zoom_x,zoom_y;bool zoom_from; /* where the next app opens from */
	int fly_i;pid_t fly_pid; /* a task card flying off before its app is closed */
	pid_t closing[MAX_TASKS];uint32_t closing_at[MAX_TASKS];
	int task_focus,task_lift,task_drag,gap_i;
	struct spring lift,gap,dim;
	float dock[4]; /* the home bar: x, y, w, h; dragging it pulls the app menu */
	struct scroll apps_scroll,notes_scroll;float scroll_from,list_top,list_bottom; /* list_* = the shade's notes window */
	int focus,gesture,last_minute,held_key,uevent_fd,udc_fd;
	double x0,y0,x,y,vel;
	uint32_t anim_time,move_time;
	struct wl_event_source *clock,*hold,*press,*child,*poll,*uevent,*udc,*toast_hide,*conf;
	char wall_conf[300]; /* the config wallpaper line the texture was made from */
};

extern struct toggle toggles[];
/* Material Icons code points, system pictograms only: apps bring their own icons */
#define I_USB 0xe1e0
#define I_BATTERY 0xe1a5
#define I_CHARGING 0xe1a3
#define I_CARD 0xe623
#define I_NOTE 0xe7f4
#define I_BACK 0xe5c4
#define I_HOME 0xe88a
#define I_TASKS 0xe8eb
#define I_BRIGHTNESS 0xe1ac
#define I_VOLUME 0xe050

static inline struct col rgb(uint32_t c,float a){return (struct col){(c>>16&255)/255.f*a,(c>>8&255)/255.f*a,(c&255)/255.f*a,a};}
static inline struct col white(float a){return (struct col){a,a,a,a};}
static inline int owner(int l){return l==L_APPS ? L_MENU : l==L_NOTES ? L_SHADE : l;}

/* draw.c */
uint32_t utf8(const char **p);
void dirty_rows(struct ui *u,int y0,int y1);
void rect(struct layer *l,float x,float y,float w,float h,float rad,struct col a,struct col b);
void quad_atlas(struct layer *l,int ax,int ay,int aw,int ah,float x,float y,float w,float h,struct col c,float mode);
float text(struct ui *u,struct layer *l,int face,float x,float y,const char *s,int size,struct col c,int align);
void fit_text(struct ui *u,int face,const char *s,int size,float max,char *out,size_t n);
void icon(struct ui *u,struct layer *l,float cx,float cy,uint32_t cp,int size,struct col c);
void item(struct ui *u,int layer,int act,int arg,float x,float y,float w,float h);
void wallpaper(struct ui *u);
void draw_layer(struct ui *u,int l,float x,float y);
void draw_dynamic(struct ui *u,struct layer *l);
void begin(struct server *s);
void end(struct server *s);
/* status.c */
struct status read_status(void);
void status_bar(struct ui *u,const char *hm);
void status_init(struct server *s);
void status_finish(struct ui *u);
/* shade.c */
int note_slot(struct ui *u,const struct note *n);
void note_icon(struct ui *u,struct layer *l,float cx,float cy,int slot,uint32_t ic,int size,float a);
void shade_build(struct ui *u);
/* a slider item
* set from the finger at x, or moved by step percent from the keys */
void slider_set(struct server *s,const struct item *it,double x);
void slider_step(struct server *s,const struct item *it,int step);
bool toast_shown(struct ui *u);
void open_shade(struct server *s,bool on);
void swipe_start(struct ui *u,uint32_t id);
void swipe_away(struct ui *u,float dir);
void shade_step(struct ui *u,float dt);
void shade_draw_swipe(struct ui *u,struct layer *dyn);
void shade_init(struct server *s);
void shade_finish(struct ui *u);
/* tasks.c */
struct surface *top_window(struct server *s);
int task_list(struct server *s,struct surface **out);
void card(struct ui *u,int i,float *x,float *y,float *w,float *h);
void open_tasks(struct server *s,bool on);
pid_t window_pid(struct surface *v);
void close_windows(struct server *s,pid_t pid);
void task_dismiss(struct server *s,int i);
void zoom_from_card(struct ui *u,int i);
float card_alpha(struct ui *u,int i);
void tasks_step(struct server *s,float dt);
void draw_tasks(struct server *s);
#endif