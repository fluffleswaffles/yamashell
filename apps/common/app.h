// SPDX-License-Identifier: GPL-2.0-only
/* shared system apps: one window in the shell's app area drawn with GLES on the VC,
 * in the shell's look, touch, keys and the shell's springs.
 * an app defines the app_* callbacks and calls app_run */
#ifndef APP_H
#define APP_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "motion.h"

/* colors. tiles and the accent are gradients top to bottom */
#define C_WHITE 0xffffff
#define C_TILE 0x484848
#define C_TILE2 0x1f1f1f
#define C_ACCENT 0x3cb2ff
#define C_ACCENT2 0x0b72c9
#define C_TRACK 0x484848
#define C_BUBBLE 0x1a1a1a
enum{F_TEXT,F_LIGHT,F_ICON,F_COUNT};
enum{T_DOWN,T_MOVE,T_UP};

/* logical pixels, app_scale physical pixels each */
extern int app_w,app_h;
extern const float app_scale;
/* logical size -> pixels to load a picture or icon with */
#define PX(v) ((int)lroundf((v)*app_scale))

void app_draw(void);
/* advance the animations by dt seconds */
bool app_step(float dt);
/* the first finger only */
void app_touch(int state,double x,double y);
/* evdev code of a pressed key */
void app_key(uint32_t key);

int app_run(const char *title,const char *id);
void app_redraw(void);
void app_quit(void);
uint32_t app_ms(void);

/* drawing 
* inside the clip rectangle only */
void clip(float x0,float y0,float x1,float y1);
void fill(float x,float y,float w,float h,float rad,uint32_t top,uint32_t bottom,float a);
/* utf-8
* align 0 left, 1 centre, 2 right of x; returns the width */
float text(int face,float x,float y,const char *s,int size,uint32_t c,float a,int align);
void icon(float cx,float cy,uint32_t cp,int size,uint32_t c,float a);
/* w*h rgb pixels at (x, y), w/app_scale logical wide,
* with rounded corners; the texture is kept until app_forget */
void picture(float x,float y,int w,int h,const unsigned char *rgb,float rad,float a);
/* w*h premultiplied rgba pixels at (x, y) like the shell's app icons, sized like picture */
void bitmap(float x,float y,int w,int h,const unsigned char *rgba,float a);
/* drop the texture of pixels about to be freed */
void app_forget(const unsigned char *px);
/* the shade's background */
void background(void);

#define MODAL_BUTTONS 3
void modal(const char *title,const char *body,const char *const *buttons,int n,void (*done)(int));
bool modal_shown(void);
bool modal_step(float dt);
void modal_draw(void);
bool modal_touch(int state,double x,double y);
bool modal_key(uint32_t key);
#endif
