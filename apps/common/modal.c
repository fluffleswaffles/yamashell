// SPDX-License-Identifier: GPL-2.0-only
/* modal: the app dims, a card springs up with a title, text and up to three buttons.
 * while it is open it takes every key and touch: left / right pick a button, OK presses it,
 * end or a tap beside the card cancels */
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "app.h"

#define PAD 20
#define LINES 8
#define LINE_H 22
#define BTN_H 44
#define GAP 10

static struct{
	char title[96],lines[LINES][160],buttons[MODAL_BUTTONS][32];
	int n,nlines,focus,pressed;
	void (*done)(int);
	struct spring show,press;
	bool open,keys,outside;
	double tx,ty;
}m;

static float card_w(void){return fminf(app_w-48,440);}
static float card_h(void){return PAD+26+12+m.nlines*LINE_H+18+BTN_H+PAD;}
static float card_x(void){return (app_w-card_w())/2;}

static float card_y(void){return (app_h-card_h())/2+(1-m.show.x)*40;}

static void button_box(int i,float *x,float *y,float *w){
	*w=(card_w()-2*PAD-(m.n-1)*GAP)/m.n;
	*x=card_x()+PAD+i*(*w+GAP);
	*y=card_y()+card_h()-PAD-BTN_H;
}

static void wrap(const char *body){
	float max=card_w()-2*PAD;
	m.nlines=0;
	while(*body && m.nlines<LINES){
		char *l=m.lines[m.nlines++];size_t n=0,fit=0;
		while(body[n] && body[n]!='\n' && n<sizeof(m.lines[0])-1){
			size_t end=n;
			while(body[end]==' '){end++;}
			while(body[end] && body[end]!=' ' && body[end]!='\n'){end++;}
			if(end>=sizeof(m.lines[0])){break;}
			memcpy(l,body,end);l[end]=0;
			if(fit && text(F_TEXT,0,0,l,16,C_WHITE,0,0)>max){break;}
			n=fit=end;
		}

		if(!fit && *body!='\n'){fit=sizeof(m.lines[0])-1;}
		memcpy(l,body,fit);l[fit]=0;
		body+=fit;
		while(*body==' '){body++;}
		if(*body=='\n'){body++;}
	}
}

void modal(const char *title,const char *body,const char *const *buttons,int n,void (*done)(int)){
	if(n<1){n=1;}
	if(n>MODAL_BUTTONS){n=MODAL_BUTTONS;}
	snprintf(m.title,sizeof(m.title),"%s",title);
	for(int i=0;i<n;i++){snprintf(m.buttons[i],sizeof(m.buttons[i]),"%s",buttons[i]);}
	m.n=n;m.done=done;m.focus=0;m.pressed=-1;m.open=true;m.outside=false;
	wrap(body);
	spring_to(&m.show,1);
	app_redraw();
}

bool modal_shown(void){return m.open;}

static void finish(int answer){
	m.open=false;spring_to(&m.show,0);m.pressed=-1;
	if(m.done){m.done(answer);}
	app_redraw();
}

bool modal_step(float dt){
	bool moving=spring_step(&m.show,dt,300);
	return spring_step(&m.press,dt,900) || moving;
}

void modal_draw(void){
	float s=clampf(m.show.x,0,1);
	if(s<=0){return;}
	clip(0,0,app_w,app_h);
	fill(0,0,app_w,app_h,0,0,0,.55f*s);
	float x=card_x(),y=card_y(),w=card_w();
	fill(x,y,w,card_h(),18,0x343434,0x232323,s);
	text(F_TEXT,x+PAD,y+PAD+20,m.title,21,C_WHITE,s,0);
	for(int i=0;i<m.nlines;i++){text(F_TEXT,x+PAD,y+PAD+26+12+16+i*LINE_H,m.lines[i],16,C_WHITE,.75f*s,0);}
	for(int i=0;i<m.n;i++){
		float bx,by,bw;button_box(i,&bx,&by,&bw);
		bool main=i==m.n-1 && m.n>1;
		if(m.keys && i==m.focus){fill(bx-3,by-3,bw+6,BTN_H+6,(BTN_H+6)/2.f,C_WHITE,C_WHITE,.85f*s);}
		fill(bx,by,bw,BTN_H,BTN_H/2.f,main ? C_ACCENT : C_TILE,main ? C_ACCENT2 : C_TILE2,s);
		if(i==m.pressed){fill(bx,by,bw,BTN_H,BTN_H/2.f,0,0,.3f*m.press.x*s);}
		text(F_TEXT,bx+bw/2,by+28,m.buttons[i],17,C_WHITE,s,1);
	}
}

static int button_at(double x,double y){
	for(int i=0;i<m.n;i++){
		float bx,by,bw;button_box(i,&bx,&by,&bw);
		if(x>=bx && x<bx+bw && y>=by && y<by+BTN_H){return i;}
	}
	return -1;
}

bool modal_touch(int state,double x,double y){
	if(!m.open){return false;}
	if(state==T_DOWN){
		m.keys=false;m.tx=x;m.ty=y;
		m.pressed=button_at(x,y);
		if(m.pressed>=0){m.press=(struct spring){1,0,1};}
		m.outside=x<card_x() || x>=card_x()+card_w() || y<card_y() || y>=card_y()+card_h();
		return true;
	}
	if(state==T_MOVE){m.tx=x;m.ty=y;if(m.pressed>=0 && button_at(x,y)!=m.pressed){spring_to(&m.press,0);}return true;}
	int b=button_at(m.tx,m.ty);
	spring_to(&m.press,0);
	if(m.pressed>=0 && b==m.pressed){finish(b);}
	else if(m.outside && b<0){finish(-1);}
	m.pressed=-1;
	return true;
}

bool modal_key(uint32_t key){
	if(!m.open){return false;}
	bool first=!m.keys;
	m.keys=true;
	if(key==KEY_ESC || key==KEY_BACKSPACE){finish(-1);}
	else if(first){app_redraw();}
	else if((key==KEY_LEFT || key==KEY_UP) && m.focus>0){m.focus--;}
	else if((key==KEY_RIGHT || key==KEY_DOWN) && m.focus<m.n-1){m.focus++;}
	else if(key==KEY_OK || key==KEY_ENTER){finish(m.focus);}
	return true;
}
