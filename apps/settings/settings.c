// SPDX-License-Identifier: GPL-2.0-only
/* yamashell-settings: a list of sections, each opens a page that slides in from the right.
 * display and sounds carry the brightness / volume sliders (level.c, the same as the shade's),
 * wallpapers picks the home screen picture (the shell reloads it when config.c's file changes),
 * apps lists the menu's apps with a page for each, about phone shows sysinfo.c's view of the hardware,
 * the other sections are placeholders for later */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>
#include <math.h>
#include <linux/input.h>
#include "app.h"
#include "level.h"
#include "config.h"
#include "image.h"
#include "apps.h"
#include "sysinfo.h"

#define TOP 76 /* the large title above the list, the header on a page */
#define ROW 70
#define SLIDER_H 80
/* Material Icons code points */
#define I_BACK 0xe5c4
#define I_CHECK 0xe5ca
#define GAP 12
#define APP_ICON 44
#define BIG_ICON 72
#define DEVICE 150
#define INFOS 24

enum{K_NONE,K_WALLS,K_APPS,K_ABOUT};
struct section{const char *title,*sub;uint32_t icon;int level,kind;struct{const char *title;uint32_t icon;}rows[4];};
static const struct section sections[]={
	{"Connections","Wi-Fi, Bluetooth, SIM",0xe63e,-1,K_NONE,{{"Wi-Fi",0xe63e},{"Bluetooth",0xe1a7},{"SIM",0xe32b}}},
	{"Sounds and vibration","Volume, vibration",0xe050,LEVEL_VOLUME,K_NONE,{{"Vibration",0xe62d}}},
	{"Notifications","App notifications",0xe7f4,-1,K_NONE,{{0}}},
	{"Display","Brightness",0xe1ac,LEVEL_BRIGHTNESS,K_NONE,{{0}}},
	{"Wallpapers","Home screen picture",0xe1bc,-1,K_WALLS,{{0}}},
	{"Lock screen","Screen lock",0xe897,-1,K_NONE,{{0}}},
	{"Security","Passwords, encryption",0xe32a,-1,K_NONE,{{0}}},
	{"Apps","Installed apps",0xe5c3,-1,K_APPS,{{0}}},
	{"About phone","Model, Linux version",0xe88e,-1,K_ABOUT,{{0}}},
};
#define NSECTIONS (int)(sizeof(sections)/sizeof(*sections))

/* what a touch or OK hits on the screen in front, rebuilt with every frame */
enum{H_OPEN,H_BACK,H_SLIDER,H_WALL,H_APP,H_LAUNCH};
struct hit{int act,arg;float x,y,w,h;};
enum{G_NONE,G_TAP,G_SCROLL,G_SLIDER};

static struct hit *hits;
static int nhits,cap_hits,page=-1,focus=-1,pressed=-1,gesture;
static bool keys;

/* sub: an app's page over the apps list, the same way a page goes over the list */
static struct spring slide,sub,press,knob,pick,shown[LEVEL_COUNT];
static struct scroll list,pagel,subl;
/* level.c reads files, once per page and change, not per frame */
static int levels[LEVEL_COUNT];

struct wall{char path[600];unsigned char *thumb;bool tried,want;float fade;};
static struct wall *walls;
static int nwalls,cap_walls,thumb_w,thumb_h,picked=-1;
static struct app *apps;
static int napps,detail=-1;
static unsigned char *detail_icon;
static char detail_size[64],detail_pkg[128];
static struct info infos[INFOS];
static int ninfos;
static unsigned char *device;
static char model[128],release[128];
static double down_x,down_y,last_y,vel;
static float scroll_from;
static uint32_t move_time;

static void hit(int act,int arg,float x,float y,float w,float h){
	if(nhits==cap_hits){
		struct hit *n=realloc(hits,sizeof(*hits)*(size_t)(cap_hits ? cap_hits*2 : 64));
		if(!n){return;}
		hits=n;cap_hits=cap_hits ? cap_hits*2 : 64;
	}
	if(keys && nhits==focus && act!=H_BACK){fill(x+4,y+2,w-8,h-4,10,0x144c80,0x08335c,.35f);}
	hits[nhits++]=(struct hit){act,arg,x,y,w,h};
}

static void tile(float x,float y,float size,uint32_t cp,float a){
	fill(x,y,size,size,size*.27f,C_TILE,C_TILE2,a);
	icon(x+size/2,y+size/2,cp,(int)(size*.58f),C_WHITE,a);
}

static void row(float x,float y,uint32_t cp,const char *title,const char *sub,float a){
	tile(x+16,y+13,44,cp,a);
	text(F_TEXT,x+76,y+31,title,19,C_WHITE,a,0);
	text(F_TEXT,x+76,y+54,sub,15,C_WHITE,.65f*a,0);
}

static void separator(float x,float y,float right){fill(x,y,right-x-16,1,0,C_WHITE,C_WHITE,.12f);}

static void draw_list(float ox,bool front){
	float y=TOP-list.x;
	list.max=fmaxf(0,TOP+NSECTIONS*ROW+12-app_h);
	text(F_LIGHT,ox+20,y-20,"Settings",34,C_WHITE,1,0);
	for(int i=0;i<NSECTIONS;i++,y+=ROW){
		const struct section *s=&sections[i];
		if(front && i==pressed){fill(ox+6,y+2,app_w-12,ROW-4,10,0x144c80,0x08335c,.35f*press.x);}
		if(front){hit(H_OPEN,i,ox,y,app_w,ROW);}
		row(ox,y,s->icon,s->title,s->sub,1);
		if(i<NSECTIONS-1){separator(ox+76,y+ROW-1,ox+app_w);}
	}
}

static void slider(float ox,float y,int k,uint32_t cp,bool front){
	int v=levels[k];float a=v<0 ? .42f : 1,tx=ox+76,tw=app_w-100,cy=y+56;
	char buf[16];

	if(front && v>=0){hit(H_SLIDER,k,tx-12,y,tw+24,SLIDER_H);}
	tile(ox+16,y+12,44,cp,a);
	text(F_TEXT,tx,y+31,k==LEVEL_BRIGHTNESS ? "Brightness" : "Volume",19,C_WHITE,a,0);
	if(v>=0){snprintf(buf,sizeof(buf),"%d%%",v);}
	text(F_TEXT,ox+app_w-24,y+31,v>=0 ? buf : "Not available",15,C_WHITE,.65f*a,2);
	fill(tx,cy-3,tw,6,3,C_TRACK,C_TRACK,a);
	if(v<0){return;}
	float f=tw*clampf(shown[k].x,0,100)/100,r=10+3*knob.x;
	fill(tx,cy-3,f,6,3,C_ACCENT,C_ACCENT2,1);
	fill(tx+f-r,cy-r,2*r,2*r,r,C_WHITE,C_WHITE,1);
}

static int wall_cols(void){return app_w>app_h ? 4 : 3;}

static int cmp(const void *a,const void *b){return strcmp(a,b);}

static bool add_wall(const char *dir,const char *name){
	if(nwalls==cap_walls){
		struct wall *n=realloc(walls,sizeof(*walls)*(size_t)(cap_walls ? cap_walls*2 : 32));
		if(!n){return false;}
		walls=n;cap_walls=cap_walls ? cap_walls*2 : 32;
	}
	struct wall *w=&walls[nwalls++];
	*w=(struct wall){0};
	snprintf(w->path,sizeof(w->path),dir ? "%s/%s" : "%s",dir ? dir : name,name);
	return true;
}

static void load_walls(void){
	char dir[300],cur[600]="";DIR *d;struct dirent *e;
	for(int i=0;i<nwalls;i++){app_forget(walls[i].thumb);free(walls[i].thumb);}
	nwalls=0;picked=-1;
	if(!add_wall(NULL,wallpaper_default())){return;}
	snprintf(cur,sizeof(cur),"%s",wallpaper_default());
	snprintf(dir,sizeof(dir),"%s/wallpapers",dirname(cur));
	cur[0]=0;
	if((d=opendir(dir))){
		while((e=readdir(d))){
			const char *x=strrchr(e->d_name,'.');
			if(e->d_name[0]!='.' && x && (!strcasecmp(x,".jpg") || !strcasecmp(x,".jpeg") || !strcasecmp(x,".png")) && !add_wall(dir,e->d_name)){break;}
		}
		closedir(d);
		qsort(walls+1,(size_t)nwalls-1,sizeof(*walls),cmp);
	}
	thumb_w=(app_w-32-(wall_cols()-1)*GAP)/wall_cols();thumb_h=thumb_w*app_h/app_w;
	if(!config_get("wallpaper",cur,sizeof(cur))){snprintf(cur,sizeof(cur),"%s",walls[0].path);}
	for(int i=0;i<nwalls;i++){if(!strcmp(walls[i].path,cur)){picked=i;}}

	if(picked<0){picked=0;}
	pick=(struct spring){1,0,1};
}

static void draw_walls(float ox,float y,bool front){
	for(int i=0;i<nwalls;i++){
		float x=ox+16+(i%wall_cols())*(thumb_w+GAP),ty=y+(i/wall_cols())*(thumb_h+GAP);
		if(front){hit(H_WALL,i,x-4,ty-4,thumb_w+8,thumb_h+8);}

		if(front && keys && nhits-1==focus){fill(x-7,ty-7,thumb_w+14,thumb_h+14,19,C_WHITE,C_WHITE,.85f);fill(x-5,ty-5,thumb_w+10,thumb_h+10,17,0x1c1c1c,0x1c1c1c,1);}

		if(i==picked){float r=4*pick.x;fill(x-r,ty-r,thumb_w+2*r,thumb_h+2*r,12+r,C_ACCENT,C_ACCENT2,clampf(pick.x,0,1));}
		struct wall *w=&walls[i];
		w->want=ty+thumb_h>0 && ty<app_h;

		if(!w->thumb || w->fade<1){fill(x,ty,thumb_w,thumb_h,12,C_TILE,C_TILE2,1);icon(x+thumb_w/2.f,ty+thumb_h/2.f,0xe3f4,32,C_WHITE,w->tried && !w->thumb ? .5f : .2f);}
		if(w->thumb){picture(x,ty,PX(thumb_w),PX(thumb_h),w->thumb,12,clampf(w->fade,0,1));}
		if(i==pressed){fill(x,ty,thumb_w,thumb_h,12,0,0,.35f*press.x);}
		if(i==picked){
			float r=13*clampf(pick.x,0,1.2f);
			fill(x+thumb_w-30-r+13,ty+thumb_h-30-r+13,2*r,2*r,r,C_ACCENT,C_ACCENT2,1);
			icon(x+thumb_w-17,ty+thumb_h-17,I_CHECK,(int)(18*clampf(pick.x,0,1.2f))+1,C_WHITE,clampf(pick.x,0,1));
		}
	}
	pagel.max=fmaxf(0,y+pagel.x+(nwalls+wall_cols()-1)/wall_cols()*(thumb_h+GAP)+4-app_h);
}

static void header(float ox,float top,const char *title,bool front){
	fill(ox+14,16+top,40,40,20,C_BUBBLE,C_BUBBLE,.82f);
	icon(ox+34,36+top,I_BACK,24,C_WHITE,1);
	text(F_LIGHT,ox+68,46+top,title,28,C_WHITE,1,0);
	if(front){hit(H_BACK,0,ox,top,64,TOP);}
}

static void draw_about(float ox,float y){
	float h=DEVICE+24;
	if(device){bitmap(ox+8,y,PX(DEVICE),PX(DEVICE),device,1);}
	text(F_LIGHT,ox+DEVICE+20,y+DEVICE/2.f-8,model,24,C_WHITE,1,0);
	text(F_TEXT,ox+DEVICE+20,y+DEVICE/2.f+20,"rapuyama-linux",17,C_WHITE,1,0);
	text(F_TEXT,ox+DEVICE+20,y+DEVICE/2.f+42,release,15,C_WHITE,.65f,0);
	y+=h;
	for(int i=0;i<ninfos;i++,y+=ROW){
		struct info *r=&infos[i];
		row(ox,y,r->icon,r->label,r->value,1);
		if(r->bar>=0){
			float w=app_w-100;
			fill(ox+76,y+ROW-9,w,4,2,C_TRACK,C_TRACK,1);
			fill(ox+76,y+ROW-9,w*clampf(r->bar,0,1),4,2,C_ACCENT,C_ACCENT2,1);
		}
		if(i<ninfos-1){separator(ox+76,y+ROW-1,ox+app_w);}
	}
	pagel.max=fmaxf(0,y+pagel.x+12-app_h);
}

static void app_row(float ox,float y,int i,bool front){
	if(front && i==pressed){fill(ox+6,y+2,app_w-12,ROW-4,10,0x144c80,0x08335c,.35f*press.x);}
	if(front){hit(H_APP,i,ox,y,app_w,ROW);}
	if(apps[i].icon){bitmap(ox+16,y+13,PX(APP_ICON),PX(APP_ICON),apps[i].icon,1);}
	else{tile(ox+16,y+13,APP_ICON,0xe5c3,1);}
	text(F_TEXT,ox+76,y+42,apps[i].name,19,C_WHITE,1,0);
	if(i<napps-1){separator(ox+76,y+ROW-1,ox+app_w);}
}

static void draw_apps(float ox,float y,bool front){
	char buf[32];
	snprintf(buf,sizeof(buf),napps==1 ? "%d app" : "%d apps",napps);
	text(F_TEXT,ox+20,y+6,buf,15,C_WHITE,.65f,0);
	y+=18;
	for(int i=0;i<napps;i++,y+=ROW){
		/* only what's on screen costs a draw */
		if(y+ROW>0 && y<app_h){app_row(ox,y,i,front);}
		else if(front){hit(H_APP,i,ox,y,app_w,ROW);}
	}
	pagel.max=fmaxf(0,y+pagel.x+12-app_h);
}

static void button(float x,float y,float w,uint32_t cp,const char *label,bool on,bool focused){
	float a=on ? 1 : .42f;
	if(focused){fill(x-3,y-3,w+6,50,25,C_WHITE,C_WHITE,.85f);}
	fill(x,y,w,44,22,on ? C_ACCENT : C_TILE,on ? C_ACCENT2 : C_TILE2,a);
	float tw=text(F_TEXT,0,-100,label,17,C_WHITE,0,0);
	icon(x+w/2-tw/2-6,y+22,cp,22,C_WHITE,a);
	text(F_TEXT,x+w/2+12,y+28,label,17,C_WHITE,a,1);
}

static void draw_detail(float ox,bool front){
	const struct app *a=&apps[detail];float top=-subl.x,y=TOP+top,bw=(app_w-48)/2.f;
	header(ox,top,"App info",front);
	if(detail_icon){bitmap(ox+20,y,PX(BIG_ICON),PX(BIG_ICON),detail_icon,1);}
	else{tile(ox+20,y,BIG_ICON,0xe5c3,1);}
	text(F_LIGHT,ox+108,y+34,a->name,26,C_WHITE,1,0);
	text(F_TEXT,ox+108,y+60,detail_pkg[0] ? detail_pkg : "Program file",15,C_WHITE,.65f,0);
	y+=BIG_ICON+24;
	/* open, then the uninstall stub, which takes no focus or taps yet */
	if(front){hit(H_LAUNCH,0,ox+16,y,bw,44);}
	button(ox+16,y,bw,0xe89e,"Open",true,front && keys && focus==nhits-1);
	button(ox+32+bw,y,bw,0xe872,"Uninstall",false,false);
	y+=68;
	row(ox,y,0xe1db,"Storage",detail_size,1);
	separator(ox+76,y+ROW-1,ox+app_w);
	y+=ROW;
	row(ox,y,0xe86f,"Command",a->exec,1);
	subl.max=fmaxf(0,y+ROW+subl.x+12-app_h);
}

static void draw_page(float ox,bool front){
	const struct section *s=&sections[page];float top=-pagel.x,y=TOP+top;

	header(ox,top,s->title,front);
	if(s->kind==K_WALLS){draw_walls(ox,y,front);return;}
	if(s->kind==K_APPS){draw_apps(ox,y,front);return;}
	if(s->kind==K_ABOUT){draw_about(ox,y);return;}
	if(s->level>=0){
		slider(ox,y,s->level,s->icon,front);
		y+=SLIDER_H;
		if(s->rows[0].title){separator(ox+16,y-1,ox+app_w);}
	}
	for(int i=0;i<4 && s->rows[i].title;i++,y+=ROW){
		row(ox,y,s->rows[i].icon,s->rows[i].title,"Not available yet",.42f);
		if(i<3 && s->rows[i+1].title){separator(ox+76,y+ROW-1,ox+app_w);}
	}
	if(s->level<0 && !s->rows[0].title){
		float cy=TOP+(app_h-TOP)/2.f-20;
		fill(ox+app_w/2.f-44,cy-44,88,88,24,C_TILE,C_TILE2,.6f);
		icon(ox+app_w/2.f,cy,s->icon,48,C_WHITE,.5f);
		text(F_TEXT,ox+app_w/2.f,cy+80,"Nothing here yet",17,C_WHITE,.65f,1);
	}
}

void app_draw(void){
	float px=app_w*(1-slide.x),dx=app_w*(1-sub.x);
	bool on_page=is_open(&slide) && !is_open(&sub);
	nhits=0;
	if(px>0){
		clip(0,0,px,app_h);background();
		draw_list(-app_w*.3f*slide.x,!on_page);
		fill(0,0,px,app_h,0,0,0,.5f*clampf(slide.x,0,1));
	}
	if(page>=0 && px<app_w && dx>px){
		clip(px,0,dx,app_h);background();
		draw_page(px-app_w*.3f*sub.x,on_page);
		fill(px,0,dx-px,app_h,0,0,0,.5f*clampf(sub.x,0,1));
	}
	if(detail>=0 && dx<app_w){
		clip(dx,0,app_w,app_h);background();
		draw_detail(dx,is_open(&sub));
	}
}

bool app_step(float dt){
	bool m=spring_step(&slide,dt,260);
	m|=spring_step(&sub,dt,260);
	m|=spring_step(&press,dt,900);
	m|=spring_step(&knob,dt,700);
	m|=spring_step(&pick,dt,500);
	if(page>=0 && sections[page].kind==K_WALLS){
		bool loaded=false;
		for(int i=0;i<nwalls;i++){
			struct wall *w=&walls[i];
			if(w->want && !w->tried && !loaded){w->thumb=image_load(w->path,PX(thumb_w),PX(thumb_h));w->tried=loaded=true;}
			else if(w->want && !w->tried){m=true;} /* more next frame */
			if(w->thumb && w->fade<1){w->fade=fminf(1,w->fade+dt*5);m=true;}
		}
		m|=loaded;
	}

	if(page>=0 && sections[page].level>=0){int k=sections[page].level,v=levels[k];shown[k].to=v<0 ? 0 : (float)v;}
	for(int k=0;k<LEVEL_COUNT;k++){

		if(gesture==G_SLIDER){shown[k].x=shown[k].to;shown[k].v=0;}
		m|=spring_step(&shown[k],dt,400);
	}
	if(gesture!=G_SCROLL){scroll_step(&list,dt);scroll_step(&pagel,dt);scroll_step(&subl,dt);m|=scroll_moving(&list) || scroll_moving(&pagel) || scroll_moving(&subl);}

	if(!is_open(&sub) && !spring_moving(&sub) && detail>=0){detail=-1;subl=(struct scroll){0};app_forget(detail_icon);free(detail_icon);detail_icon=NULL;}
	if(!is_open(&slide) && !spring_moving(&slide)){page=-1;}
	if(press.x==0 && !spring_moving(&press)){pressed=-1;}
	if(page<0){pagel=(struct scroll){0};}
	return m || spring_moving(&slide) || spring_moving(&sub);
}

static void open_page(int i){
	page=i;spring_to(&slide,1);focus=-1;pressed=-1;
	if(sections[i].kind==K_WALLS){load_walls();}
	/* read again on every visit: apps get installed, the battery drains */
	if(sections[i].kind==K_APPS){
		for(int k=0;k<napps;k++){app_forget(apps[k].icon);}
		apps_free(apps,napps);napps=apps_load(&apps,PX(APP_ICON));
	}
	if(sections[i].kind==K_ABOUT){
		ninfos=sysinfo_read(infos,INFOS);
		sysinfo_model(model,sizeof(model));sysinfo_release(release,sizeof(release));
		if(!device){char name[64];snprintf(name,sizeof(name),"yamashell-device-%s",sysinfo_device());device=icon_load(name,PX(DEVICE));}
	}

	if(sections[i].level>=0){int k=sections[i].level,v=levels[k]=level_get(k);shown[k]=(struct spring){v,0,v};}
}

static void open_detail(int i){
	long long size;
	detail=i;spring_to(&sub,1);focus=-1;
	app_forget(detail_icon);free(detail_icon);detail_icon=icon_load(apps[i].icon_name,PX(BIG_ICON));
	if((size=sysinfo_app_size(apps[i].exec,detail_pkg,sizeof(detail_pkg)))<0){snprintf(detail_size,sizeof(detail_size),"Unknown");}
	else{sysinfo_size(size,detail_size,sizeof(detail_size));}
}

/* detached, the shell maps its window like any other */
static void launch(const char *exec){
	if(fork()==0){setsid();execl("/bin/sh","sh","-c",exec,(char *)NULL);_exit(127);}
}

static void back(void){
	if(is_open(&sub)){spring_to(&sub,0);focus=keys ? detail+1 : -1;return;}
	if(!is_open(&slide)){app_quit();return;}
	spring_to(&slide,0);
	focus=keys ? page : -1;
}

static void set_level(int k,int v){level_set(k,v);levels[k]=level_get(k);}
static void set_slider(struct hit *h,double x){set_level(h->arg,(int)lround((x-h->x-12)*100/(h->w-24)));}

static struct hit *hit_at(double x,double y){
	for(int i=0;i<nhits;i++){struct hit *h=&hits[i];if(x>=h->x && y>=h->y && x<h->x+h->w && y<h->y+h->h){return h;}}
	return NULL;
}

static struct scroll *scroller(void){return is_open(&sub) ? &subl : is_open(&slide) ? &pagel : &list;}

static void activate(struct hit *h){
	if(h->act==H_OPEN){open_page(h->arg);}
	else if(h->act==H_WALL && h->arg!=picked){picked=h->arg;config_set("wallpaper",walls[picked].path);pick=(struct spring){0,0,1};}
	else if(h->act==H_APP){open_detail(h->arg);}
	else if(h->act==H_LAUNCH){launch(apps[detail].exec);}
	else if(h->act==H_BACK){back();}
}

static void move_focus(int dir){
	int i=focus;
	do{i+=dir;}while(i>=0 && i<nhits && hits[i].act==H_BACK);
	if(focus<0){for(i=0;i<nhits && hits[i].act==H_BACK;i++){}}
	if(i<0 || i>=nhits){return;}
	focus=i;
	struct hit *h=&hits[i];
	if(h->act==H_OPEN || h->act==H_WALL || h->act==H_APP){
		struct scroll *sc=scroller();
		if(h->y<0){sc->x+=h->y-(i==0 || (h->act==H_WALL && h->arg<wall_cols()) || (h->act==H_APP && h->arg==0) ? TOP+18 : 0);}
		else if(h->y+h->h>app_h){sc->x+=h->y+h->h-app_h;}
		sc->x=clampf(sc->x,0,sc->max);sc->v=0;
	}
}

void app_key(uint32_t code){
	struct hit *f=focus>=0 && focus<nhits ? &hits[focus] : NULL;
	keys=true;

	int row_step=f && f->act==H_WALL ? wall_cols() : 1;
	if(code==KEY_UP){move_focus(-row_step);}
	else if(code==KEY_DOWN){move_focus(row_step);}
	else if(code==KEY_LEFT || code==KEY_RIGHT){
		for(int i=0;i<nhits && !(f && f->act==H_SLIDER);i++){if(hits[i].act==H_SLIDER){f=&hits[i];focus=i;}}
		if(f && f->act==H_SLIDER){set_level(f->arg,levels[f->arg]+(code==KEY_LEFT ? -10 : 10));}
		else{move_focus(code==KEY_LEFT ? -1 : 1);}
	}
	else if((code==KEY_OK || code==KEY_ENTER) && f){activate(f);}
	else if(code==KEY_ESC || code==KEY_BACKSPACE){back();}
}

static void track(double y){
	uint32_t now=app_ms();float dt=(now-move_time)/1000.f;
	if(dt>.0005f){vel=vel*.4+((last_y-y)/dt)*.6;}
	move_time=now;last_y=y;
}

void app_touch(int state,double x,double y){
	if(state==T_DOWN){
		struct hit *h=hit_at(x,y);
		down_x=x;down_y=y;last_y=y;vel=0;move_time=app_ms();keys=false;
		gesture=h && h->act==H_SLIDER ? G_SLIDER : G_TAP;
		if(gesture==G_SLIDER){focus=(int)(h-hits);set_slider(h,x);spring_to(&knob,1);}
		else if(h && (h->act==H_OPEN || h->act==H_WALL || h->act==H_APP)){pressed=h->arg;press=(struct spring){1,0,1};}
		scroller()->v=0;
		return;
	}
	if(state==T_MOVE){
		if(gesture==G_SLIDER){set_slider(&hits[focus],x);return;}
		if(gesture==G_TAP && (fabs(y-down_y)>10 || fabs(x-down_x)>10)){
			spring_to(&press,0);
			gesture=fabs(y-down_y)>fabs(x-down_x) ? G_SCROLL : G_NONE;
			scroll_from=scroller()->x+(float)(y-down_y);
		}
		if(gesture==G_SCROLL){track(y);scroller()->x=scroll_rubber(scroller(),scroll_from-(float)(y-down_y));}
		return;
	}
	struct hit *h=gesture==G_TAP ? hit_at(down_x,down_y) : NULL;
	if(gesture==G_SCROLL){scroller()->v=(float)vel;}
	spring_to(&knob,0);spring_to(&press,0);
	gesture=G_NONE;
	if(h){activate(h);}
}

int main(void){
	signal(SIGCHLD,SIG_IGN); /* launched apps are never waited for */
	return app_run("Settings","yamashell-settings");
}
