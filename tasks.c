// SPDX-License-Identifier: GPL-2.0-only
/* running apps: a card per window with its live picture, swiped through and thrown away. emmgee can still suck a fart*/
#define _GNU_SOURCE
#include "ui.h"

/* state */
struct surface *top_window(struct server *s){
	struct surface *v;
	wl_list_for_each_reverse(v,&s->surfaces,link){if(!v->parent && !v->cursor && v->xdg && surface_visible(v)){return v;}}
	return NULL;
}

int task_list(struct server *s,struct surface **out){
	struct surface *v;int n=0;
	wl_list_for_each_reverse(v,&s->surfaces,link){if(n<MAX_TASKS && !v->parent && !v->cursor && v->xdg && surface_visible(v)){out[n++]=v;}}
	return n;
}

void card(struct ui *u,int i,float *x,float *y,float *w,float *h){
	*w=screen_w*(screen_w>screen_h ? .5f : .64f);*h=*w*(screen_h-STATUS)/screen_w;
	*x=screen_w/2.f-*w/2+(i-u->task_pos.x)*(*w+28);
	*y=STATUS+(screen_h-STATUS-*h)/2+14+(1-u->tasks.x)*screen_h;
}

void open_tasks(struct server *s,bool on){
	struct ui *u=s->ui;struct surface *list[MAX_TASKS];
	int n=task_list(s,list);
	if(on){
		/* like android: from an app the previous one is preselected */
		u->task_focus=n>1 && !u->home ? 1 : 0;
		u->task_pos.x=u->task_pos.to=(float)u->task_focus;u->task_pos.v=0;
		spring_to(&u->menu,0);spring_to(&u->shade,0);
	}
	spring_to(&u->tasks,on ? 1 : 0);
	schedule_repaint(s,true);
}

pid_t window_pid(struct surface *v){
	pid_t pid=0;
	wl_client_get_credentials(wl_resource_get_client(v->resource),&pid,NULL,NULL);
	return pid;
}

/* ask every top-level window of the app to close, like its own close button */
void close_windows(struct server *s,pid_t pid){
	struct surface *v;
	wl_list_for_each(v,&s->surfaces,link){
		if(!v->parent && v->xdg && v->role && surface_visible(v) && (pid<0 || window_pid(v)==pid)){xdg_toplevel_send_close(v->role);}
	}
}

static const char *task_name(struct ui *u,struct surface *v){
	pid_t pid=window_pid(v);
	for(int i=0;i<u->napps;i++){if(u->apps[i].pid==pid){return u->apps[i].name;}}
	return "Window";
}

/* the card flies up first, the app is asked to close once it is off screen */
void task_dismiss(struct server *s,int i,float from){
	struct ui *u=s->ui;struct surface *list[MAX_TASKS];
	if(u->fly_i>=0 || i<0 || i>=task_list(s,list)){return;}
	u->fly_i=i;u->fly_pid=window_pid(list[i]);
	u->fly.x=from;u->fly.v=-400;
	schedule_repaint(s,true);
}

void zoom_from_card(struct ui *u,int i){
	float x,y,w,h;card(u,i,&x,&y,&w,&h);
	u->zoom_x=x+w/2;u->zoom_y=y+h/2;u->zoom_from=true;
}

void tasks_step(struct server *s,float dt){
	struct ui *u=s->ui;
	if(u->fly_i>=0){
		/* thrown up: speeds up until it is off screen, about 0.3 s */
		u->fly.v-=5000*dt;u->fly.x+=u->fly.v*dt;
		if(u->fly.x<-screen_h*.9f){close_windows(s,u->fly_pid);u->fly_i=-1;u->fly=(struct spring){0,0,0};}
	}
}

/* running apps: dark sheet, one card per window with its live texture */
void draw_tasks(struct server *s){
	struct ui *u=s->ui;struct layer *l=&u->taskl;struct surface *list[MAX_TASKS];
	int n=task_list(s,list);
	char name[96];
	if(u->task_focus>=n){u->task_focus=n>0 ? n-1 : 0;spring_to(&u->task_pos,(float)u->task_focus);}
	l->n=0;
	float off=(1-u->tasks.x)*screen_h;
	rect(l,0,off,screen_w,screen_h*1.25f,0,(struct col){0,0,0,.92f},(struct col){0,0,0,.96f});
	if(!n){text(u,l,F_TEXT,screen_w/2.f,off+screen_h/2.f,"No running apps",18,white(.7f),1);}
	for(int i=0;i<n;i++){
		float x,y,w,h;card(u,i,&x,&y,&w,&h);
		if(i==u->task_lift){y+=u->lift;}
		if(i==u->fly_i){y+=u->fly.x;}
		if(u->keys && i==u->task_focus){rect(l,x-5,y-5,w+10,h+10,14,rgb(0x3cb2ff,.9f),rgb(0x0b72c9,.9f));}
		rect(l,x-1,y-1,w+2,h+2,10,white(.25f),white(.25f));
		fit_text(u,F_TEXT,task_name(u,list[i]),16,w,name,sizeof(name));
		text(u,l,F_TEXT,x+w/2,y-12,name,16,white(.9f),1);
	}
	draw_dynamic(u,l);
	end(s);
	for(int i=0;i<n;i++){
		float x,y,w,h;card(u,i,&x,&y,&w,&h);
		if(i==u->task_lift){y+=u->lift;}
		if(i==u->fly_i){y+=u->fly.x;}
		if(x+w>0 && x<screen_w){renderer_thumb(list[i],x,y,w,h);}
	}
	begin(s);
}