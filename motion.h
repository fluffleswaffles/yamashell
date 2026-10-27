// SPDX-License-Identifier: GPL-2.0-only
/* springs and thrown lists: the same feel in the shell and in the system apps */
#ifndef MOTION_H
#define MOTION_H
#include <stdbool.h>
/* critically-ish damped spring a little overshoot when it lands */
struct spring{float x,v,to;};
/* a list offset in pixels, 0..max, thrown with v */
struct scroll{float x,v,max;};
static inline float clampf(float v,float lo,float hi){return v<lo ? lo : v>hi ? hi : v;}
static inline void spring_to(struct spring *s,float to){s->to=to;}
static inline bool spring_moving(struct spring *s){return s->x!=s->to || s->v!=0;}
static inline bool is_open(struct spring *s){return s->to>.5f;}
static inline bool scroll_moving(struct scroll *s){return s->v!=0 || s->x<0 || s->x>s->max;}
static inline float rubber(float x){return x>1 ? 1+(x-1)*.3f : x<0 ? 0 : x;}
bool spring_step(struct spring *s,float dt,float k);
void fling(struct spring *s,float v);
void scroll_step(struct scroll *s,float dt);
float scroll_rubber(struct scroll *s,float raw);
#endif
