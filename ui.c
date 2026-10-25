// SPDX-License-Identifier: GPL-2.0-only
/* This is a symbian+android style-shell. daily reminder: emmgee can suck a fart.
  * home screen, app menu, notification shade and edge gestures. Every layer is
  * one VBO rebuilt only when its content changes. Animation only mobed a layer
  * with a uniform so nothing crosses the bus per frame (except commands). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input.h>
#include "ui.h"

static bool running(struct app *a){return a->pid>0 && kill(a->pid,0)==0;}
/* the app's own icon, or its first letter on a tile when it has none */
static void app_icon(struct ui *u,struct layer *l,float x,float y,int i){
	struct app *a=&u->apps[i];
	if(a->icon && i<APP_SLOTS){
		quad_atlas(l,i%(ATLAS_W/ICON)*ICON,GLYPHS_H+i/(ATLAS_W/ICON)*ICON,ICON,ICON,x,y,ICON,ICON,white(1),2);
	}
	else{
		uint32_t h=2166136261u;
		for(const char *p=a->name;*p;p++){h=(h^(unsigned char)*p)*16777619u;}
		rect(l,x+4,y+4,ICON-8,ICON-8,14,rgb(0x404040|(h&0x3f3f3f)<<1,1),rgb(0x202020|(h&0x3f3f3f),1));
		char first[8]={0};const char *p=a->name;utf8(&p);memcpy(first,a->name,(size_t)(p-a->name));
		text(u,l,F_TEXT,x+ICON/2.f,y+ICON/2.f+11,first,30,white(1),1);
	}
	if(running(a)){rect(l,x+ICON/2.f-3,y+ICON+4,6,6,3,white(.9f),white(.9f));}
}

static void app_cell(struct ui *u,struct layer *l,int layer,int i,float x,float y,float w){
	char name[96];
	app_icon(u,l,x+(w-ICON)/2,y+8,i);
	fit_text(u,F_TEXT,u->apps[i].name,15,w-10,name,sizeof(name));
	text(u,l,F_TEXT,x+w/2,y+8+ICON+28,name,15,white(1),1);
	item(u,layer,A_LAUNCH,i,x+2,y,w-4,ICON+40);
}

/* layer contents */
static const char *days[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static void build(struct ui *u){
	struct layer *home=&u->layers[L_HOME],*menu=&u->layers[L_MENU];
	for(int i=0;i<L_COUNT;i++){u->layers[i].n=0;}
	u->nitems=0;
	time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
	char buf[96],hm[8];
	strftime(hm,sizeof(hm),"%H:%M",&tm);

	status_bar(u,hm);

	/* home: clock, date, the terminal, the handle that hints at the app menu */
	int big=(screen_w<screen_h ? screen_w : screen_h)/4;
	float base=STATUS+30+big*.72f;
	text(u,home,F_LIGHT,34,base+2,hm,big,(struct col){0,0,0,.3f},0);
	text(u,home,F_LIGHT,32,base,hm,big,white(1),0);
	snprintf(buf,sizeof(buf),"%s, %d %s",days[tm.tm_wday],tm.tm_mday,months[tm.tm_mon]);
	text(u,home,F_TEXT,35,base+39,buf,22,(struct col){0,0,0,.3f},0);
	text(u,home,F_TEXT,34,base+38,buf,22,white(.95f),0);
	if(u->terminal>=0){
		float cw=120,ch=ICON+48,cx=(screen_w-cw)/2,cy=screen_h-BOTTOM-ch;
		u->dock[0]=cx;u->dock[1]=cy;u->dock[2]=cw;u->dock[3]=ch;
		rect(home,cx,cy,cw,ch,16,(struct col){0,0,0,.42f},(struct col){0,0,0,.5f});
		app_cell(u,home,L_HOME,u->terminal,cx,cy+2,cw);
	}
	rect(home,screen_w/2.f-36,screen_h-14,72,6,3,white(.75f),white(.75f));
	item(u,L_HOME,A_MENU,0,screen_w/2.f-80,screen_h-BOTTOM,160,BOTTOM);

	/* app menu: every installed app. the sheet reaches past the screen so the spring overshoot never shows a gap */
	rect(menu,0,0,screen_w,screen_h*1.25f,0,(struct col){0,0,0,.93f},(struct col){0,0,0,.96f});
	int cols=screen_w>screen_h ? 6 : 4;float mc=(float)screen_w/cols,row=ICON+52;
	for(int i=0;i<u->napps;i++){app_cell(u,&u->layers[L_APPS],L_APPS,i,mc*(i%cols),STATUS+16+(i/cols)*row,mc);}
	u->apps_scroll.max=fmaxf(0,STATUS+16+(u->napps+cols-1)/cols*row+16-screen_h);

	if(!u->napps){text(u,menu,F_TEXT,screen_w/2.f,screen_h/2.f,"No apps installed",18,white(.7f),1);}

	shade_build(u);

	for(int i=0;i<L_COUNT;i++){
		struct layer *l=&u->layers[i];
		if(!l->vbo){glGenBuffers(1,&l->vbo);}
		glBindBuffer(GL_ARRAY_BUFFER,l->vbo);
		glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(sizeof(*l->v)*l->n),l->v,GL_STATIC_DRAW);
		l->drawn=l->n;
	}

	glBindBuffer(GL_ARRAY_BUFFER,0);
	if(u->dirty_y1>u->dirty_y0){
		glBindTexture(GL_TEXTURE_2D,u->atlas);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,u->dirty_y0,ATLAS_W,u->dirty_y1-u->dirty_y0,GL_RGBA,GL_UNSIGNED_BYTE,u->pixels+(size_t)u->dirty_y0*ATLAS_W*4);
		u->dirty_y0=ATLAS_H;u->dirty_y1=0;
	}

	u->last_minute=tm.tm_hour*60+tm.tm_min;
	u->dirty=false;
}

static void go_home(struct server *s){
	struct ui *u=s->ui;
	u->home=true;spring_to(&u->menu,0);spring_to(&u->shade,0);spring_to(&u->tasks,0);u->focus=-1;
	/* the window shrinks into the home handle */
	u->zoom_x=screen_w/2.f;u->zoom_y=(float)screen_h;spring_to(&u->app,0);
	input_focus(s,NULL);
	schedule_repaint(s,true);
}

static void show_app(struct server *s,struct surface *v){
	struct ui *u=s->ui;
	/* from home the window grows out of what was tapped, or the middle */
	if(u->app.x<.5f && !u->zoom_from){u->zoom_x=screen_w/2.f;u->zoom_y=screen_h/2.f;}
	u->zoom_from=false;
	u->home=false;spring_to(&u->menu,0);spring_to(&u->shade,0);spring_to(&u->tasks,0);spring_to(&u->app,1);
	if(v){surface_focus(v);}
	schedule_repaint(s,true);
}

void ui_window_mapped(struct server *s,struct surface *v){
	if(s->ui){show_app(s,v);}
}

static void launch(struct server *s,int i){
	struct ui *u=s->ui;struct surface *v;
	if(running(&u->apps[i])){
		wl_list_for_each(v,&s->surfaces,link){
			if(v->parent || !v->xdg || !surface_visible(v)){continue;}
			if(window_pid(v)==u->apps[i].pid){show_app(s,v);return;}
		}
	}
	pid_t pid=fork();
	if(pid==0){
		/* the event loop blocks SIGCHLD/SIGTERM/SIGINT for signalfd; apps must not inherit that */
		sigset_t none;sigemptyset(&none);sigprocmask(SIG_SETMASK,&none,NULL);
		setsid();execl("/bin/sh","sh","-c",u->apps[i].exec,(char *)NULL);_exit(127);
	}
	if(pid>0){u->apps[i].pid=pid;u->dirty=true;}
	spring_to(&u->menu,0);schedule_repaint(s,true);
}

void ui_start_terminal(struct server *s){
	if(s->ui && s->ui->terminal>=0){launch(s,s->ui->terminal);}
}

static int reap(int sig,void *data){
	struct server *s=data;struct ui *u=s->ui;
	(void)sig;
	pid_t pid;
	while((pid=waitpid(-1,NULL,WNOHANG))>0){for(int i=0;i<u->napps;i++){if(u->apps[i].pid==pid){u->apps[i].pid=0;u->dirty=true;}}}
	if(u->dirty){schedule_repaint(s,true);}
	return 0;
}

static int clock_tick(void *data){
	struct server *s=data;struct ui *u=s->ui;
	time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
	if(tm.tm_hour*60+tm.tm_min!=u->last_minute){u->dirty=true;schedule_repaint(s,true);}
	wl_event_source_timer_update(u->clock,(60-tm.tm_sec)*1000+20);
	return 0;
}

static int active_layer(struct ui *u){
	if(is_open(&u->shade)){return L_SHADE;}
	if(is_open(&u->menu)){return L_MENU;}
	return u->home ? L_HOME : -1;
}

static void layer_offset(struct ui *u,int l,float *x,float *y){
	*x=0;*y=0;
	if(owner(l)==L_SHADE){*y=-(1-u->shade.x)*screen_h;}
	if(owner(l)==L_MENU){*y=(1-u->menu.x)*screen_h;}
	if(l==L_APPS){*y-=u->apps_scroll.x;}
	if(l==L_NOTES){*y-=u->notes_scroll.x;}
}

static bool item_shown(struct ui *u,struct item *it,float y){
	if(it->layer==L_APPS){return y+it->h>STATUS;}
	if(it->layer==L_NOTES){return y+it->h>u->list_top && y<u->list_bottom;}
	return true;
}

static void reveal(struct ui *u){
	struct item *it=&u->items[u->focus];
	struct scroll *sc=it->layer==L_APPS ? &u->apps_scroll : it->layer==L_NOTES ? &u->notes_scroll : NULL;
	if(!sc){return;}
	float top=it->layer==L_APPS ? STATUS+16 : u->list_top,bottom=it->layer==L_APPS ? screen_h-16 : u->list_bottom;
	if(it->y-sc->x<top){sc->x=it->y-top;}
	else if(it->y+it->h-sc->x>bottom){sc->x=it->y+it->h-bottom;}
	sc->x=clampf(sc->x,0,sc->max);sc->v=0;
}

static void back(struct server *s){
	struct ui *u=s->ui;
	if(is_open(&u->shade)){spring_to(&u->shade,0);}
	else if(is_open(&u->tasks)){spring_to(&u->tasks,0);}
	else if(is_open(&u->menu)){spring_to(&u->menu,0);}
	/* inside an app back is Esc, the usual linux cancel */
	else if(!u->home){input_key_tap(s,KEY_ESC);}
	schedule_repaint(s,true);
}

static void open_menu(struct server *s,bool on){
	struct ui *u=s->ui;
	if(on && !u->home){u->home=true;input_focus(s,NULL);}
	spring_to(&u->menu,on ? 1 : 0);spring_to(&u->shade,0);spring_to(&u->tasks,0);u->focus=-1;
	schedule_repaint(s,true);
}

static void activate(struct server *s,struct item *it){
	struct ui *u=s->ui;
	if(it->act==A_LAUNCH){
		float ox,oy;layer_offset(u,it->layer,&ox,&oy);
		u->zoom_x=it->x+ox+it->w/2;u->zoom_y=it->y+oy+ICON/2.f+8;u->zoom_from=true;
		launch(s,it->arg);
	}
	else if(it->act==A_MENU){open_menu(s,true);}
	else if(it->act==A_CLOSE){spring_to(&u->shade,0);}
	else if(it->act==A_NOTE){const struct note *n;if(it->arg<notify_list(&n)){swipe_start(u,n[it->arg].id);swipe_away(u,1);}}
	else if(it->act==A_TOGGLE && !toggles[it->arg].na){toggles[it->arg].on=!toggles[it->arg].on;u->dirty=true;}
	schedule_repaint(s,true);
}

static struct item *item_at(struct ui *u,double x,double y){
	int l=active_layer(u);
	for(int i=0;i<u->nitems;i++){
		struct item *it=&u->items[i];float ox,oy;layer_offset(u,it->layer,&ox,&oy);
		if(owner(it->layer)==l && item_shown(u,it,it->y+oy) && x>=it->x+ox && y>=it->y+oy && x<it->x+ox+it->w && y<it->y+oy+it->h){return it;}
	}
	return NULL;
}

/* keys. home, messages, end, d-pad (only when the sheell has focus) */
/* long presses: home (running apps), messages (shade), end (close app) */
static int hold_fire(void *data){
	struct server *s=data;struct ui *u=s->ui;int key=u->held_key;
	u->held_key=0;
	if(key==KEY_HOMEPAGE){open_tasks(s,!is_open(&u->tasks));}
	else if(key==KEY_MESSENGER){open_shade(s,!is_open(&u->shade));}
	else if(key==KEY_HANGUP_PHONE && is_open(&u->tasks)){task_dismiss(s,u->task_focus,0);}
	else if(key==KEY_HANGUP_PHONE && !u->home){struct surface *v=top_window(s);if(v){close_windows(s,window_pid(v));}}
	return 0;
}

static void move_focus(struct ui *u,int dx,int dy,struct server *s){
	int l=active_layer(u),best=-1;float bd=1e9f;
	if(l<0){return;}
	if(u->focus<0 || owner(u->items[u->focus].layer)!=l){
		if(l==L_HOME && dy<0){open_shade(s,true);}
		else{for(int i=0;i<u->nitems;i++){if(owner(u->items[i].layer)==l){u->focus=i;reveal(u);break;}}}
		schedule_repaint(s,true);
		return;
	}

	struct item *a=&u->items[u->focus];float ox,oy,by;layer_offset(u,a->layer,&ox,&oy);
	float ax=a->x+a->w/2,ay=a->y+oy+a->h/2;
	for(int i=0;i<u->nitems;i++){
		struct item *b=&u->items[i];
		if(i==u->focus || owner(b->layer)!=l){continue;}
		layer_offset(u,b->layer,&ox,&by);
		float vx=b->x+b->w/2-ax,vy=b->y+by+b->h/2-ay,along=dx ? vx*dx : vy*dy;
		if(along<=4){continue;}
		float d=along+(dx ? fabsf(vy) : fabsf(vx))*2.2f;
		if(d<bd){bd=d;best=i;}
	}

	if(best>=0){u->focus=best;reveal(u);}
	else if(l==L_HOME && dy<0){open_shade(s,true);}
	else if(l==L_SHADE && dy>0){open_shade(s,false);}
	schedule_repaint(s,true);
}

bool ui_key(struct server *s,uint32_t key,uint32_t state){
	struct ui *u=s->ui;
	if(!u){return false;}
	if(key==KEY_HOMEPAGE || key==KEY_MESSENGER || key==KEY_HANGUP_PHONE){
		if(state){if(!u->held_key){u->held_key=key;wl_event_source_timer_update(u->hold,HOLD);}return true;}
		if(u->held_key!=(int)key){return true;} /* the long press already fired */
		u->held_key=0;wl_event_source_timer_update(u->hold,0);
		if(key==KEY_HOMEPAGE){
			if(!u->home || is_open(&u->shade) || is_open(&u->tasks)){go_home(s);}
			else{open_menu(s,!is_open(&u->menu));}
		}

		else if(key==KEY_MESSENGER){open_menu(s,!is_open(&u->menu));}

		else{back(s);}
		return true;
	}
	if(is_open(&u->tasks)){
		struct surface *list[MAX_TASKS];
		int n=task_list(s,list);
		if(!state){return true;}
		u->keys=true;
		if(key==KEY_LEFT && u->task_focus>0){u->task_focus--;}
		else if(key==KEY_RIGHT && u->task_focus<n-1){u->task_focus++;}
		else if((key==KEY_OK || key==KEY_ENTER) && u->task_focus<n){zoom_from_card(u,u->task_focus);show_app(s,list[u->task_focus]);}

		else if(key==KEY_UP || key==KEY_DELETE){task_dismiss(s,u->task_focus,0);}
		else if(key==KEY_BACKSPACE || key==KEY_ESC){back(s);}
		spring_to(&u->task_pos,(float)u->task_focus);
		schedule_repaint(s,true);
		return true;
	}

	if(active_layer(u)<0){return false;}
	if(!state){return key==KEY_UP || key==KEY_DOWN || key==KEY_LEFT || key==KEY_RIGHT || key==KEY_OK || key==KEY_ENTER;}
	u->keys=true;
	if(key==KEY_UP){move_focus(u,0,-1,s);return true;}
	if(key==KEY_DOWN){move_focus(u,0,1,s);return true;}
	if(key==KEY_LEFT){move_focus(u,-1,0,s);return true;}
	if(key==KEY_RIGHT){move_focus(u,1,0,s);return true;}
	if(key==KEY_OK || key==KEY_ENTER){
		if(u->focus>=0 && owner(u->items[u->focus].layer)==active_layer(u)){activate(s,&u->items[u->focus]);}
		else{move_focus(u,0,0,s);}
		return true;
	}

	if(key==KEY_BACKSPACE || key==KEY_ESC){back(s);return true;}
	return u->home;
}

static int press_fire(void *data){
	struct server *s=data;struct ui *u=s->ui;
	if(u->gesture!=G_TAP && u->gesture!=G_MENU && u->gesture!=G_MENU_OPEN){return 0;}
	struct item *it=item_at(u,u->x0,u->y0);
	if(!it || it->act!=A_LAUNCH || !running(&u->apps[it->arg])){return 0;}
	close_windows(s,u->apps[it->arg].pid);
	u->gesture=G_HELD;
	return 0;
}

static void track(struct ui *u,double pos){
	uint32_t now=milliseconds();
	float dt=(now-u->move_time)/1000.f;
	if(dt>.0005f){u->vel=u->vel*.4+((pos-u->x)/dt)*.6;}
	u->move_time=now;u->x=pos;
}

bool ui_touch(struct server *s,int id,double x,double y,int state){
	struct ui *u=s->ui;
	if(!u || (id!=0 && u->gesture==G_NONE)){return false;}
	if(id!=0){return true;}
	if(state==1){
		u->x0=x;u->y0=y;u->y=y;u->keys=false;u->vel=0;u->move_time=milliseconds();u->armed=false;
		if(toast_shown(u) && y>=STATUS+6 && y<STATUS+6+TOAST_H){u->gesture=G_TOAST;}
		else if(x<EDGE){u->gesture=G_BACK_L;}
		else if(x>screen_w-EDGE){u->gesture=G_BACK_R;}
		else if(is_open(&u->shade)){u->gesture=G_SHADE;}
		else if(y<STATUS+6){u->gesture=G_SHADE_OPEN;}
		else if(is_open(&u->tasks)){u->gesture=G_TASKS;}
		else if(is_open(&u->menu)){u->gesture=G_MENU;}
		/* the bottom handle is home / running apps everywhere, on home the bar pulls the app menu */
		else if(y>screen_h-BOTTOM){u->gesture=G_HOME;}
		else if(u->home && u->terminal>=0 && x>=u->dock[0] && y>=u->dock[1] && x<u->dock[0]+u->dock[2] && y<u->dock[1]+u->dock[3]){u->gesture=G_MENU_OPEN;}
		else if(u->home){u->gesture=G_TAP;}
		else{return false;}
		u->x=u->gesture==G_SHADE_OPEN || u->gesture==G_SHADE ? u->shade.x : u->gesture==G_MENU_OPEN || u->gesture==G_MENU ? u->menu.x : 0;
		u->pull_home=u->gesture==G_HOME;u->task_hint=false;u->task_lift=-1;u->lift=0;
		if(u->gesture==G_TASKS){
			struct surface *list[MAX_TASKS];int n=task_list(s,list);
			for(int i=0;i<n;i++){float cx,cy,cw,ch;card(u,i,&cx,&cy,&cw,&ch);if(x>=cx && x<cx+cw && y>=cy && y<cy+ch){u->task_lift=i;}}
		}
		if(u->gesture==G_TAP || u->gesture==G_MENU || u->gesture==G_MENU_OPEN){wl_event_source_timer_update(u->press,HOLD);}
		return true;
	}
	if(u->gesture==G_NONE){return false;}
	double dx=x-u->x0,dy=y-u->y0;
	bool moved=fabs(dx)>10 || fabs(dy)>10;
	if(moved){wl_event_source_timer_update(u->press,0);}
	if(u->gesture==G_HELD){if(state==0){u->gesture=G_NONE;}return true;}
	if(state==2){
		u->y=y;
		bool vertical=moved && fabs(dy)>fabs(dx);
		if(u->gesture==G_MENU && vertical && u->menu.x>=1 && u->apps_scroll.max>0 && !(dy>0 && u->apps_scroll.x<=.5f)){
			u->gesture=G_MENU_SCROLL;u->scroll_from=u->apps_scroll.x+(float)dy;u->x=u->apps_scroll.x;
		}
		if(u->gesture==G_SHADE && vertical && u->shade.x>=1 && u->notes_scroll.max>0 && u->y0>=u->list_top && u->y0<u->list_bottom && !(dy<0 && u->notes_scroll.x>=u->notes_scroll.max-.5f)){
			u->gesture=G_SHADE_SCROLL;u->scroll_from=u->notes_scroll.x+(float)dy;u->x=u->notes_scroll.x;
		}
		if(u->gesture==G_MENU_SCROLL){track(u,u->scroll_from-dy);u->apps_scroll.x=scroll_rubber(&u->apps_scroll,(float)u->x);u->apps_scroll.v=0;}
		if(u->gesture==G_SHADE_SCROLL){track(u,u->scroll_from-dy);u->notes_scroll.x=scroll_rubber(&u->notes_scroll,(float)u->x);u->notes_scroll.v=0;}
		if(u->gesture==G_SHADE_OPEN && moved){track(u,y/screen_h);u->shade.x=u->shade.to=rubber((float)u->x);u->shade.v=0;}
		/* sideways on a note swipes it, up still closes the shade */
		if(u->gesture==G_SHADE && u->shade.x>=1 && fabs(dx)>10 && fabs(dx)>fabs(dy)){
			struct item *it=item_at(u,u->x0,u->y0);const struct note *n;
			if(it && it->act==A_NOTE && it->arg<notify_list(&n)){swipe_start(u,n[it->arg].id);u->gesture=G_NOTE;u->x=0;}
		}
		if(u->gesture==G_NOTE){track(u,dx/screen_w);u->swipe.x=u->swipe.to=(float)u->x;u->swipe.v=0;}
		if(u->gesture==G_SHADE && dy<-10){track(u,1+dy/screen_h);u->shade.x=u->shade.to=rubber((float)u->x);u->shade.v=0;}
		if(u->gesture==G_MENU_OPEN && moved){track(u,-dy/screen_h);u->menu.x=u->menu.to=rubber((float)u->x);u->menu.v=0;}
		if(u->gesture==G_MENU && dy>10){track(u,1-dy/screen_h);u->menu.x=u->menu.to=rubber((float)u->x);u->menu.v=0;}
		/* back / home: hard to pull out, then it pops once the travel arms it */
		if(u->gesture==G_BACK_L || u->gesture==G_BACK_R || u->gesture==G_HOME){
			float travel=(float)(u->gesture==G_HOME ? -dy : u->gesture==G_BACK_L ? dx : -dx),p=travel/BACK_DIST;
			track(u,travel);
			/* home, then a dead zone, then running apps; a little back off on the way down */
			if(u->gesture==G_HOME){
				if(!u->task_hint && travel>BACK_DIST+TASKS_DEAD){u->task_hint=true;}
				else if(u->task_hint && travel<BACK_DIST+TASKS_DEAD-30){u->task_hint=false;}
				spring_to(&u->hint,u->task_hint ? 1 : 0);
			}
			if(!u->armed && p>=1){u->armed=true;}
			else if(u->armed && p<.8f){u->armed=false;}
			spring_to(&u->pull,u->armed ? 1 : .6f*clampf(p,0,1));
		}
		if(u->gesture==G_TASKS && moved){
			float cw,cx,cy,ch;card(u,0,&cx,&cy,&cw,&ch);
			if(fabs(dx)>fabs(dy)){u->task_pos.x=u->task_pos.to=u->task_focus-(float)dx/(cw+28);u->task_pos.v=0;u->lift=0;}
			else if(u->task_lift>=0){u->lift=(float)fmin(0,dy);}
		}
		schedule_repaint(s,true);
		return true;
	}

	int g=u->gesture;u->gesture=G_NONE;
	wl_event_source_timer_update(u->press,0);
	if(g==G_MENU_SCROLL){u->apps_scroll.v=(float)u->vel;}
	else if(g==G_SHADE_SCROLL){u->notes_scroll.v=(float)u->vel;}
	/* a third of the screen or a flick throws the note out, less springs it back */
	else if(g==G_NOTE){
		float v=(float)u->vel*screen_w;
		if(fabsf(u->swipe.x)>.33f || (fabsf(v)>FLICK && v*u->swipe.x>0)){swipe_away(u,u->swipe.x);}
		else{spring_to(&u->swipe,0);}
		u->swipe.v=(float)u->vel;
	}
	else if(g==G_TOAST){
		if(dy<-20){spring_to(&u->toast,0);}
		else if(!moved){open_shade(s,true);}
	}
	else if(g==G_TASKS){
		struct surface *list[MAX_TASKS];int n=task_list(s,list);
		if(!moved){
			if(u->task_lift>=0){zoom_from_card(u,u->task_lift);show_app(s,list[u->task_lift]);}
			else if(top_window(s) && !u->home){show_app(s,top_window(s));}
			else{open_tasks(s,false);}
		}else if(fabs(dx)>fabs(dy)){
			u->task_focus=(int)lroundf(clampf(u->task_pos.x,0,(float)(n>0 ? n-1 : 0)));
			spring_to(&u->task_pos,(float)u->task_focus);
		}else if(dy<-80 && u->task_lift>=0){task_dismiss(s,u->task_lift,u->lift);}
		u->lift=0;
	}

	else if(g==G_BACK_L || g==G_BACK_R || g==G_HOME){
		bool flick=u->vel>FLICK && (g==G_HOME ? -dy : g==G_BACK_L ? dx : -dx)>30;
		if(g==G_HOME && u->task_hint){open_tasks(s,true);}
		else if(u->armed || flick){if(g==G_HOME){go_home(s);}else{back(s);}}
		/* a tap on the home handle opens the app menu */
		else if(g==G_HOME && !moved && u->home){struct item *it=item_at(u,x,y);if(it){activate(s,it);}}
		u->armed=false;u->task_hint=false;spring_to(&u->pull,0);spring_to(&u->hint,0);
	}

	else if((g==G_SHADE_OPEN || g==G_SHADE) && moved){fling(&u->shade,(float)u->vel);}
	else if(g==G_SHADE_OPEN){open_shade(s,true);}
	else if((g==G_MENU_OPEN || g==G_MENU) && moved){fling(&u->menu,(float)u->vel);}
	else if(!moved){
		struct item *it=item_at(u,x,y);
		if(it && it->act!=A_NOTE){u->focus=(int)(it-u->items);activate(s,it);}
	}
	schedule_repaint(s,true);
	return true;
}

bool ui_animating(struct server *s){
	struct ui *u=s->ui;
	if(!u){return false;}
	bool sheet=u->gesture!=G_SHADE_OPEN && u->gesture!=G_SHADE && u->gesture!=G_MENU_OPEN && u->gesture!=G_MENU;
	/* new glyphs drawn this frame reach the atlas on the next one */
	return u->dirty || (sheet && (spring_moving(&u->shade) || spring_moving(&u->menu))) || spring_moving(&u->pull) || spring_moving(&u->tasks) || spring_moving(&u->task_pos) || spring_moving(&u->hint) || spring_moving(&u->app) || spring_moving(&u->toast) || (u->gesture!=G_MENU_SCROLL && scroll_moving(&u->apps_scroll)) || (u->gesture!=G_SHADE_SCROLL && scroll_moving(&u->notes_scroll)) || (u->swipe_id && u->gesture!=G_NOTE) || u->fly_i>=0;
}
/* before the windows: wallpaper and home; returns whether app windows are drawn */
bool ui_draw_under(struct server *s){
	struct ui *u=s->ui;
	if(!u){return true;}
	uint32_t now=milliseconds();float dt=u->anim_time ? (now-u->anim_time)/1000.f : 0;
	if(dt>.1f){dt=.1f;}
	u->anim_time=now;
	if(u->gesture!=G_SHADE_OPEN && u->gesture!=G_SHADE){spring_step(&u->shade,dt,260);}
	if(u->gesture!=G_MENU_OPEN && u->gesture!=G_MENU){spring_step(&u->menu,dt,260);}
	spring_step(&u->pull,dt,u->armed ? 700 : 420);
	spring_step(&u->tasks,dt,260);
	spring_step(&u->hint,dt,700);
	if(u->gesture!=G_TASKS){spring_step(&u->task_pos,dt,400);}
	spring_step(&u->app,dt,300);
	spring_step(&u->toast,dt,300);
	if(u->gesture!=G_MENU_SCROLL){scroll_step(&u->apps_scroll,dt);}
	if(u->gesture!=G_SHADE_SCROLL){scroll_step(&u->notes_scroll,dt);}
	shade_step(u,dt);
	tasks_step(s,dt);
	if(!u->home && !top_window(s)){u->home=true;spring_to(&u->app,0);u->app.x=0;u->app.v=0;input_focus(s,NULL);}
	if(u->dirty){build(u);}
	s->zoom=u->app.x>0 ? u->app.x : 0;s->zoom_x=u->zoom_x;s->zoom_y=u->zoom_y;
	/* a settled app covers the screen: no wallpaper or home behind it */
	if(!u->home && !spring_moving(&u->app)){s->zoom=1;return true;}
	glBindTexture(GL_TEXTURE_2D,u->wall);
	glUniform1f(s->alpha,1.f);glUniform1f(s->native,1.f);
	renderer_quad(s,-1,1,1,-1,true);
	begin(s);
	draw_layer(u,L_HOME,0,0);
	end(s);
	return u->app.x>.01f;
}

/* android-style back bubble: slides in a little, follows the finger only a bit up and down */
static void draw_pull(struct ui *u,struct layer *dyn){
	float b=u->pull.x;
	if(b<=.01f){return;}
	bool home=u->pull_home;
	float size=(36+20*clampf(b,0,1))*(home ? 1+.3f*u->hint.x : 1),a=clampf(b*2,0,1),x,y;
	if(home){x=screen_w/2.f-size/2;y=screen_h-b*90;}
	else{
		float cy=(float)u->y0+clampf((float)(u->y-u->y0)*.25f,-30,30);
		y=cy-size/2;
		x=u->x0<screen_w/2.f ? -size+b*(size+20) : screen_w-b*(size+20);
	}
	rect(dyn,x,y,size,size,size/2,(struct col){.1f*a,.1f*a,.1f*a,.82f*a},(struct col){.1f*a,.1f*a,.1f*a,.82f*a});
	float ia=clampf((b-.3f)/.4f,0,1);
	if(ia>0){icon(u,dyn,x+size/2,y+size/2,home ? (u->hint.x>.5f ? I_TASKS : I_HOME) : I_BACK,(int)(size*.6f),white(ia));}
}

void ui_draw_over(struct server *s){
	struct ui *u=s->ui;
	if(!u){return;}
	begin(s);
	if(u->menu.x>0){float y=(1-u->menu.x)*screen_h;draw_layer(u,L_MENU,0,y);draw_layer(u,L_APPS,0,y-u->apps_scroll.x);}
	if(u->tasks.x>0){draw_tasks(s);}
	if(u->toast.x>0){draw_layer(u,L_TOAST,0,-(1-u->toast.x)*(STATUS+12+TOAST_H));}
	/* the notes go between the shade's background and its covers above and below the list */
	if(u->shade.x>0){
		float y=-(1-u->shade.x)*screen_h;
		draw_layer(u,L_SHADE,0,y);draw_layer(u,L_NOTES,0,y-u->notes_scroll.x);draw_layer(u,L_SHADE_TOP,0,y);
	}
	draw_layer(u,L_STATUS,0,0);
	struct layer *dyn=&u->dynamic;dyn->n=0;
	int l=active_layer(u);
	if(u->keys && !is_open(&u->tasks) && u->focus>=0 && owner(u->items[u->focus].layer)==l){
		struct item *it=&u->items[u->focus];float ox,oy;layer_offset(u,it->layer,&ox,&oy);
		rect(dyn,it->x+ox,it->y+oy,it->w,it->h,10,(struct col){.08f,.3f,.5f,.35f},(struct col){.03f,.2f,.36f,.35f});
	}
	draw_pull(u,dyn);
	shade_draw_swipe(u,dyn);
	if(u->dirty_y1>u->dirty_y0){u->dirty=true;}
	draw_dynamic(u,dyn);
	end(s);
}

/* setup */
static GLuint compile(GLenum type,const char *src){
	GLuint id=glCreateShader(type);GLint ok;
	glShaderSource(id,1,&src,NULL);glCompileShader(id);glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
	if(!ok){fprintf(stderr,"yamashell: ui shader failed\n");glDeleteShader(id);return 0;}
	return id;
}

static FT_Face font(struct ui *u,const char *env,const char *path){
	FT_Face f=NULL;
	if(getenv(env)){path=getenv(env);}
	if(FT_New_Face(u->ft,path,0,&f)){fprintf(stderr,"yamashell: no font %s\n",path);return NULL;}
	return f;
}

static void load_apps(struct ui *u){
	u->napps=apps_load(&u->apps);u->terminal=-1;
	for(int i=0;i<u->napps;i++){
		struct app *a=&u->apps[i];
		if(a->terminal && u->terminal<0){u->terminal=i;}
		if(!a->icon || i>=APP_SLOTS){continue;}
		int ax=i%(ATLAS_W/ICON)*ICON,ay=GLYPHS_H+i/(ATLAS_W/ICON)*ICON;
		for(int y=0;y<ICON;y++){memcpy(u->pixels+((size_t)(ay+y)*ATLAS_W+ax)*4,a->icon+(size_t)y*ICON*4,ICON*4);}
		dirty_rows(u,ay,ay+ICON);
	}
	fprintf(stderr,"yamashell: %d apps\n",u->napps);
}

void ui_init(struct server *s){
	struct ui *u=calloc(1,sizeof(*u));
	if(!u){return;}
	char vs[640];
	snprintf(vs,sizeof(vs),"attribute vec2 pos; attribute vec2 uv; attribute vec4 col; attribute vec4 prm; uniform vec2 off;"
		"varying vec2 t; varying vec4 c; varying vec4 p;"
		"void main(){vec2 q=pos+off; gl_Position=vec4(q.x*%f-1.,1.-q.y*%f,0.,1.); t=uv; c=col; p=prm;}",2.0/screen_w,2.0/screen_h);
	/* mode p.w: 0 rect (rounded box distance in pixels), 1 text (atlas coverage), 2 icon (atlas colour) */
	const char *fs="precision mediump float; uniform sampler2D tex; varying vec2 t; varying vec4 c; varying vec4 p;"
		"void main(){vec4 g=texture2D(tex,t); vec2 q=abs(t)-p.xy+p.z; float d=length(max(q,0.))+min(max(q.x,q.y),0.)-p.z;"
		"vec4 a=c*mix(clamp(.5-d,0.,1.),g.a,min(p.w,1.)); gl_FragColor=mix(a,g*c.a,step(1.5,p.w));}";
	GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);GLint ok=0;
	if(v && f){
		u->program=glCreateProgram();glAttachShader(u->program,v);glAttachShader(u->program,f);
		glBindAttribLocation(u->program,0,"pos");glBindAttribLocation(u->program,1,"uv");
		glBindAttribLocation(u->program,2,"col");glBindAttribLocation(u->program,3,"prm");
		glLinkProgram(u->program);glGetProgramiv(u->program,GL_LINK_STATUS,&ok);
	}

	if(v){glDeleteShader(v);}if(f){glDeleteShader(f);}
	if(!ok){fprintf(stderr,"yamashell: ui disabled (shader)\n");if(u->program){glDeleteProgram(u->program);}free(u);return;}
	glUseProgram(u->program);
	u->off=glGetUniformLocation(u->program,"off");
	glUniform1i(glGetUniformLocation(u->program,"tex"),0);
	glUseProgram(s->program);
	if(FT_Init_FreeType(&u->ft)){u->ft=NULL;}
	else{
		u->face[F_TEXT]=font(u,"YAMASHELL_FONT",ASSETS "Ubuntu-Regular.ttf");
		u->face[F_LIGHT]=font(u,"YAMASHELL_FONT_LIGHT",ASSETS "Ubuntu-Light.ttf");
		u->face[F_ICON]=font(u,"YAMASHELL_ICONS",ASSETS "MaterialIcons-Regular.ttf");
	}

	u->pixels=calloc((size_t)ATLAS_W*ATLAS_H,4);
	if(!u->pixels){abort();}
	u->dirty_y0=ATLAS_H;
	glGenTextures(1,&u->atlas);glBindTexture(GL_TEXTURE_2D,u->atlas);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,ATLAS_W,ATLAS_H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
	wallpaper(u);
	load_apps(u);
	u->home=true;u->focus=-1;u->fly_i=-1;
	s->ui=u;s->inset=STATUS;
	status_init(s);
	/* items exist before the first frame, a tap may come first */
	build(u);
	u->clock=wl_event_loop_add_timer(s->loop,clock_tick,s);clock_tick(s);
	u->hold=wl_event_loop_add_timer(s->loop,hold_fire,s);
	u->press=wl_event_loop_add_timer(s->loop,press_fire,s);
	u->child=wl_event_loop_add_signal(s->loop,SIGCHLD,reap,s);
	shade_init(s);
	if(glGetError()!=GL_NO_ERROR){fprintf(stderr,"yamashell: ui setup GL error\n");}
}

void ui_finish(struct server *s){
	struct ui *u=s->ui;
	if(!u){return;}
	wl_event_source_remove(u->clock);wl_event_source_remove(u->hold);wl_event_source_remove(u->press);
	wl_event_source_remove(u->child);
	status_finish(u);
	shade_finish(u);
	for(int i=0;i<L_COUNT;i++){if(u->layers[i].vbo){glDeleteBuffers(1,&u->layers[i].vbo);}free(u->layers[i].v);}
	if(u->dynamic.vbo){glDeleteBuffers(1,&u->dynamic.vbo);}free(u->dynamic.v);
	if(u->taskl.vbo){glDeleteBuffers(1,&u->taskl.vbo);}free(u->taskl.v);
	glDeleteTextures(1,&u->atlas);glDeleteTextures(1,&u->wall);glDeleteProgram(u->program);
	for(int i=0;i<F_COUNT;i++){if(u->face[i]){FT_Done_Face(u->face[i]);}}
	if(u->ft){FT_Done_FreeType(u->ft);}
	apps_free(u->apps,u->napps);
	free(u->pixels);free(u);s->ui=NULL;s->inset=0;
}