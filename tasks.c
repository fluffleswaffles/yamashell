// SPDX-License-Identifier: GPL-2.0-only
/* running apps: a card per window with its live picture, swiped through and thrown away. emmgee can still suck a fart*/
#define _GNU_SOURCE
#include "ui.h"

/* state */
static bool closing(struct ui *u,pid_t pid){
	for(int i=0;i<MAX_TASKS;i++){if(u->closing[i] && u->closing[i]==pid){return true;}}
	return false;
}

static bool listed(struct server *s,struct surface *v){
	return !v->parent && !v->cursor && v->xdg && surface_visible(v) && !closing(s->ui,window_pid(v));
}

struct surface *top_window(struct server *s){
	struct surface *v;
	wl_list_for_each_reverse(v,&s->surfaces,link){if(listed(s,v)){return v;}}
	return NULL;
}

int task_list(struct server *s,struct surface **out){
	struct surface *v;int n=0;
	wl_list_for_each_reverse(v,&s->surfaces,link){if(n<MAX_TASKS && listed(s,v)){out[n++]=v;}}
	return n;
}

void card(struct ui *u,int i,float *x,float *y,float *w,float *h){
	*w=screen_w*(screen_w>screen_h ? .5f : .64f);*h=*w*(screen_h-STATUS)/screen_w;
	float slot=*w+28;
	*x=screen_w/2.f-*w/2+(i-u->task_pos.x)*slot;
	*y=STATUS+(screen_h-STATUS-*h)/2+14+(1-u->tasks.x)*screen_h;
	/* the cards after a closed one slide over its slot */
	if(i>=u->gap_i){*x+=u->gap.x*slot;}
	if(i==u->fly_i){*y+=u->fly.x;}
	if(i==u->task_lift && u->lift.x<0){*y+=u->lift.x;}
	/* pulled down it grows towards the full window, around its centre that follows half the finger */
	if(i==u->task_lift && u->lift.x>0){
		float g=1+fminf(u->lift.x/screen_h,.5f)*.8f,cx=*x+*w/2,cy=*y+*h/2+u->lift.x*.5f;
		*w*=g;*h*=g;*x=cx-*w/2;*y=cy-*h/2;
	}
}

/* fades as it goes up */
float card_alpha(struct ui *u,int i){
	float up=i==u->fly_i ? u->fly.x : i==u->task_lift ? fminf(u->lift.x,0) : 0;
	return 1-clampf(-up/(screen_h*.6f),0,1);
}

void open_tasks(struct server *s,bool on){
	struct ui *u=s->ui;struct surface *list[MAX_TASKS];
	int n=task_list(s,list);
	if(on){
		/* like android: from an app the previous one is preselected */
		u->task_focus=n>1 && !u->home ? 1 : 0;
		u->task_pos.x=u->task_pos.to=(float)u->task_focus;u->task_pos.v=0;
		u->task_lift=-1;u->lift=(struct spring){0,0,0};
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
void task_dismiss(struct server *s,int i){
	struct ui *u=s->ui;struct surface *list[MAX_TASKS];
	if(u->fly_i>=0 || i<0 || i>=task_list(s,list)){return;}
	u->fly_i=i;u->fly_pid=window_pid(list[i]);
	if(i==u->task_lift){u->fly.x=fminf(u->lift.x,0);u->fly.v=fminf(u->lift.v,-1600);u->task_lift=-1;}
	u->lift=(struct spring){0,0,0};
	schedule_repaint(s,true);
}

void zoom_from_card(struct ui *u,int i){
	float x,y,w,h;card(u,i,&x,&y,&w,&h);
	float z=clampf(w/screen_w,.05f,.95f);
	u->zoom_x=x/(1-z);u->zoom_y=(y-STATUS*z)/(1-z);u->zoom_from=true;
	u->app.x=z;u->app.v=0;
	u->dim.x=u->tasks.x*.92f;u->dim.v=0;spring_to(&u->dim,0);
	u->tasks.x=u->tasks.to=0;u->tasks.v=0;
}

void tasks_step(struct server *s,float dt){
	struct ui *u=s->ui;
	if(u->fly_i>=0){
		/* thrown up: speeds up until it is off screen, about 0.3 s */
		u->fly.v-=6000*dt;u->fly.x+=u->fly.v*dt;
		if(u->fly.x<-screen_h*.9f){
			close_windows(s,u->fly_pid);
			for(int i=0;i<MAX_TASKS;i++){if(!u->closing[i]){u->closing[i]=u->fly_pid;u->closing_at[i]=milliseconds();break;}}
			u->gap_i=u->fly_i;u->gap.x=1;spring_to(&u->gap,0);
			u->fly_i=-1;u->fly=(struct spring){0,0,0};
		}
	}
	for(int i=0;i<MAX_TASKS;i++){
		if(!u->closing[i]){continue;}
		struct surface *v;bool alive=false;
		wl_list_for_each(v,&s->surfaces,link){if(!v->parent && v->xdg && surface_visible(v) && window_pid(v)==u->closing[i]){alive=true;break;}}
		if(!alive || milliseconds()-u->closing_at[i]>2000){u->closing[i]=0;schedule_repaint(s,true);}
	}
	if(u->gesture!=G_TASKS || u->task_drag!=2){spring_step(&u->lift,dt,400);}
	spring_step(&u->gap,dt,400);
	spring_step(&u->dim,dt,120);
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
	int top=u->fly_i>=0 ? u->fly_i : u->task_lift;
	for(int pass=0;pass<2;pass++){
		for(int i=0;i<n;i++){
			if((i==top)!=pass){continue;}
			float x,y,w,h,a=card_alpha(u,i);card(u,i,&x,&y,&w,&h);
			if(u->keys && i==u->task_focus){rect(l,x-5,y-5,w+10,h+10,14,rgb(0x3cb2ff,.9f*a),rgb(0x0b72c9,.9f*a));}
			rect(l,x-1,y-1,w+2,h+2,10,white(.25f*a),white(.25f*a));
			fit_text(u,F_TEXT,task_name(u,list[i]),16,w,name,sizeof(name));
			text(u,l,F_TEXT,x+w/2,y-12,name,16,white(.9f*a),1);
		}
		draw_dynamic(u,l);l->n=0;
		end(s);
		for(int i=0;i<n;i++){
			if((i==top)!=pass){continue;}
			float x,y,w,h;card(u,i,&x,&y,&w,&h);
			if(x+w>0 && x<screen_w){renderer_thumb(list[i],x,y,w,h,card_alpha(u,i));}
		}
		begin(s);
	}
}