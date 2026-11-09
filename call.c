// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include "ui.h"
#include "config.h"
#include "apps/common/messages.h"
#include "apps/common/contacts.h"

static bool ringing(struct ui *u){return u->call_incoming && u->call_state==5;}

static void notice(const char *app,const char *title,const char *body,bool critical){
	if(fork()==0){
		sigset_t mask;sigemptyset(&mask);sigprocmask(SIG_SETMASK,&mask,NULL);
		if(critical){execlp("notify-send","notify-send","-u","critical","-t","0","-a",app,title,body,(char *)NULL);}
		else{execlp("notify-send","notify-send","-a",app,title,body,(char *)NULL);}_exit(127);
	}
}

static void action(struct server *s,bool answer){
	struct ui *u=s->ui;
	sound_stop(u->ring_pid);u->ring_pid=0;
	if(u->call_pid>1){if(!answer){kill(-u->call_pid,SIGTERM);}return;}
	pid_t pid=fork();
	if(pid==0){
		setsid();sigset_t mask;sigemptyset(&mask);sigprocmask(SIG_SETMASK,&mask,NULL);
		const char *directory="/usr/bin",*name=answer ? "modem-answer" : ringing(u) ? "modem-reject" : "modem-hangup";
#ifdef YAMASHELL_HOST
		const char *mock=getenv("YAMASHELL_MODEM_HELPERS");if(mock && *mock){directory=mock;}
#endif
		char path[512];snprintf(path,sizeof(path),"%s/%s",directory,name);
		execl(path,name,(char *)NULL);_exit(127);
	}
	if(pid>0){u->call_pid=pid;}
	schedule_repaint(s,true);
}

static int poll_call(void *data){
	struct server *s=data;struct ui *u=s->ui;unsigned long long stamp;unsigned id=0,state=0,incoming=0;struct timespec now;
	clock_gettime(CLOCK_MONOTONIC,&now);
	const char *directory="/run/rapuyama-modem";
#ifdef YAMASHELL_HOST
	const char *mock=getenv("YAMASHELL_MODEM_STATE");if(mock && *mock){directory=mock;}
#endif
	char path[640];snprintf(path,sizeof(path),"%s/calls",directory);
	FILE *f=fopen(path,"r");
	if(f){
		char number[24];
		if(fscanf(f,"%llu %x %u %u %23s",&stamp,&id,&state,&incoming,number)!=5 || stamp>(unsigned long long)now.tv_sec || (unsigned long long)now.tv_sec-stamp>8 || !(id&7) || state==0 || state>12 || incoming>1){id=0;}
		else{snprintf(u->caller,sizeof(u->caller),"%s",strcmp(number,"-") ? number : "");}
		fclose(f);
	}
	if(u->call_pid>1 && kill(u->call_pid,0)){u->call_pid=0;}
	if(!id && u->call_pid>1){kill(-u->call_pid,SIGTERM);}
	if(id!=u->call_id){u->call_started=0;u->call_focus=0;}
	if(id && state==8 && !u->call_started){u->call_started=now.tv_sec;}
	bool changed=id!=u->call_id || state!=u->call_state || incoming!=u->call_incoming;
	u->call_id=id;u->call_state=id ? state : 0;u->call_incoming=id && incoming;u->call_answered=id && (state==7 || state==8);
	bool shown=id!=0;
	if(shown){power_pointer(s);}
	if(shown!=u->call_shown){
		u->call_shown=shown;u->dirty=true;schedule_repaint(s,true);
		spring_to(&u->call_show,shown ? 1 : 0);
	}
	if(changed || (shown && u->call_started)){u->dirty=true;schedule_repaint(s,true);}
	if(u->ring_pid>1 && kill(u->ring_pid,0)){u->ring_pid=0;}
	if(ringing(u) && u->call_pid<=1){if(u->ring_pid<=1){u->ring_pid=sound_play(true);}}
	else{sound_stop(u->ring_pid);u->ring_pid=0;}
	float ring=ringing(u) ? (u->call_ring.to>.5f ? 0 : 1) : 0;
	if(ring!=u->call_ring.to){spring_to(&u->call_ring,ring);schedule_repaint(s,true);}
	snprintf(path,sizeof(path),"%s/failed",directory);
	char reason[24]="";unsigned failed=0;FILE *failure=fopen(path,"r");
	if(failure){
		fscanf(failure,"%23s",reason);fclose(failure);
		failed=!strcmp(reason,"observer") ? 1 : !strcmp(reason,"runtime") ? 3 : 2;
	}
	if(failed && failed!=u->modem_failed){
		notice("Phone","Modem unavailable",failed==1 ? "Call status is temporarily unavailable. The modem has not been restarted." : failed==3 ? "The modem runtime stopped. A fresh Linux boot is required." : "The modem could not be activated.",failed==3);
	}
	u->modem_failed=failed;
	static unsigned checked;static bool loaded;static char latest[48];
	if(milliseconds()-checked>2000){
		checked=milliseconds();struct message *messages=calloc(MESSAGE_LIMIT,sizeof(*messages));
		if(messages){
			int n=messages_load(messages,MESSAGE_LIMIT),i=n-1;
			while(i>=0 && messages[i].outgoing){i--;}
			if(i>=0){
				if(loaded && strcmp(latest,messages[i].id)){notice("Messages","New message",messages[i].number,false);}
				snprintf(latest,sizeof(latest),"%s",messages[i].id);
			}
			loaded=true;free(messages);
		}
	}
	wl_event_source_timer_update(u->call_poll,500);return 0;
}

bool call_key(struct server *s,uint32_t key,uint32_t state){
	if(!s->ui->call_shown){return false;}
	if(!state){return true;}
	struct ui *u=s->ui;
	if(key==KEY_PICKUP_PHONE || key==KEY_PHONE){if(ringing(u)){action(s,true);}return true;}
	if(key==KEY_ENTER || key==KEY_KPENTER || key==KEY_OK){action(s,ringing(u) && u->call_focus==0);}
	if(key==KEY_LEFT || key==KEY_RIGHT){u->call_focus=!u->call_focus;schedule_repaint(s,true);}
	if(state && (key==KEY_HANGUP_PHONE || key==KEY_ESC)){action(s,false);}
	return true;
}

bool call_touch(struct server *s,double x,double y,int state){
	if(!s->ui->call_shown){return false;}
	if(state==1){s->ui->call_touch_x=x;s->ui->call_touch_y=y;spring_to(&s->ui->call_press,y>screen_h-90 ? 1 : 0);}
	if(!state){spring_to(&s->ui->call_press,0);}
	schedule_repaint(s,true);
	if(!state && y>screen_h-90 && y<screen_h-26 && fabs(x-s->ui->call_touch_x)<20 && fabs(y-s->ui->call_touch_y)<20){action(s,ringing(s->ui) && x<screen_w/2);}
	return true;
}

void call_draw(struct server *s,struct layer *l){
	struct ui *u=s->ui;float a=clampf(u->call_show.x,0,1);if(a<=0){return;}
	float dy=(1-u->call_show.x)*28,cx=screen_w/2.f,cy=80+dy,r=38+6*u->call_ring.x;
	rect(l,0,0,screen_w,screen_h,0,rgb(0x1f1f1f,a),rgb(0x080808,a));
	rect(l,cx-r,cy-r,2*r,2*r,r,rgb(0x3cb2ff,.08f*a),rgb(0x0b72c9,.08f*a));
	icon(u,l,cx,cy,0xe7fd,42,white(a));
	const char *name=contacts_name(u->caller);char shown[128];fit_text(u,F_LIGHT,name,30,screen_w-40,shown,sizeof(shown));
	text(u,l,F_LIGHT,cx,150+dy,shown,30,white(a),1);
	if(*u->caller && strcmp(name,u->caller)){text(u,l,F_TEXT,cx,182+dy,u->caller,18,white(.6f*a),1);}
	const char *status=ringing(u) ? "Incoming call" : u->call_state==4 ? "Ringing" : u->call_state==8 ? "Connected" : u->call_state==9 || u->call_state==10 ? "Ending call" : u->call_incoming ? "Connecting" : "Calling";
	char duration[64];if(u->call_started && u->call_state==8){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);unsigned t=now.tv_sec-u->call_started;snprintf(duration,sizeof(duration),"%u:%02u",t/60,t%60);status=duration;}
	text(u,l,F_TEXT,cx,218+dy,status,19,white(.65f*a),1);
	for(int i=0;i<(ringing(u) ? 2 : 1);i++){
		bool answer=ringing(u) && !i;float w=ringing(u) ? (screen_w-60)/2.f : screen_w-48,x=ringing(u) ? 20+i*(w+20) : 24;
		if(u->call_focus==i){rect(l,x-3,screen_h-93+dy,w+6,70,35,white(.25f*a),white(.25f*a));}
		rect(l,x,screen_h-90+dy,w,64,32,rgb(answer ? 0x34864f : 0xb84141,a),rgb(answer ? 0x205732 : 0x782525,a));
		icon(u,l,x+32,screen_h-58+dy,answer ? 0xe0b0 : 0xe0b1,26,white(a));
		text(u,l,F_TEXT,x+w/2+10,screen_h-50+dy,answer ? "Answer" : ringing(u) ? "Decline" : "End call",20,white(a),1);
	}
}

void call_init(struct server *s){s->ui->call_poll=wl_event_loop_add_timer(s->loop,poll_call,s);wl_event_source_timer_update(s->ui->call_poll,500);}