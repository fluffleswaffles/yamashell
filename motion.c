// SPDX-License-Identifier: GPL-2.0-only
#include <math.h>
#include "motion.h"

bool spring_step(struct spring *s,float dt,float k){
	if(s->x==s->to && s->v==0){return false;}
	float c=2*.72f*sqrtf(k);
	for(float t=0;t<dt;t+=.004f){
		float h=fminf(.004f,dt-t);
		s->v+=(-k*(s->x-s->to)-c*s->v)*h;s->x+=s->v*h;
	}
	if(fabsf(s->x-s->to)<.002f && fabsf(s->v)<.02f){s->x=s->to;s->v=0;}
	return true;
}

void scroll_step(struct scroll *s,float dt){
	for(float t=0;t<dt;t+=.004f){
		float h=fminf(.004f,dt-t),end=clampf(s->x,0,s->max);
		if(s->x!=end){s->v+=(-400*(s->x-end)-40*s->v)*h;}
		else{s->v*=expf(-3.f*h);}
		s->x+=s->v*h;
	}
	float end=clampf(s->x,0,s->max);
	if(fabsf(s->v)<10 && fabsf(s->x-end)<.5f){s->x=end;s->v=0;}
}

/* the finger pulls a third as far past an end */
float scroll_rubber(struct scroll *s,float raw){return raw<0 ? raw*.35f : raw>s->max ? s->max+(raw-s->max)*.35f : raw;}

void fling(struct spring *s,float v){
	s->v=v;
	s->to=s->x+v*.12f>.5f ? 1 : 0;
}
