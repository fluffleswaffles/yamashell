// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "app.h"
#include "field.h"
#include "modem.h"
#include "messages.h"
#include "contacts.h"

#define TOP 70
#define ROW 74
#define I_CHAT 0xe0b7
#define I_COMPOSE 0xe150
#define I_SEND 0xe163
#define I_PERSON 0xe7fd
#define I_DONE 0xe877
#define I_PENDING 0xe8b5
#define I_ERROR 0xe001
enum{C_TO,C_CONTACTS,C_BODY,C_SEND,C_HISTORY};
static struct modem_job job={.fd=-1};
static struct message history[MESSAGE_LIMIT],pending;
static int count,threads[MESSAGE_LIMIT],nthreads,selected=-1,compose=C_TO;
static char number[24],draft[MESSAGE_TEXT];
static struct field recipient,body;
static bool new_number,saving,keys,follow,dragged,tracking;
static struct spring slide,press;
static struct scroll list,chat;
static double down_x,down_y,last_y,velocity;
static float scroll_from;
static uint32_t checked,move_time;
static struct contact contacts[512];
static int ncontacts,contact_selected;
static struct spring picker;
static struct scroll contact_list;

static void pick_contact(void){
	ncontacts=contacts_load(contacts,512);contact_selected=0;contact_list=(struct scroll){0};
	spring_to(&picker,1);tracking=dragged=false;
}

static void choose_contact(void){
	if(contact_selected<0 || contact_selected>=ncontacts){return;}
	snprintf(number,sizeof(number),"%s",contacts[contact_selected].number);field_end(&recipient);
	spring_to(&picker,0);compose=C_BODY;follow=true;
}

static bool conversation(void){return is_open(&slide);}
static struct scroll *scroller(void){return conversation() ? &chat : &list;}

static void reload(void){
	count=messages_load(history,MESSAGE_LIMIT);nthreads=0;
	for(int i=count-1;i>=0;i--){
		bool found=false;
		for(int j=0;j<nthreads;j++){if(!strcmp(history[i].number,history[threads[j]].number)){found=true;break;}}
		if(!found){threads[nthreads++]=i;}
	}
	if(selected>=nthreads){selected=nthreads-1;}
}

static void open_thread(int index){
	new_number=index<0;compose=new_number ? C_TO : C_BODY;draft[0]=0;
	snprintf(number,sizeof(number),"%s",index<0 ? "" : history[threads[index]].number);
	field_end(&recipient);field_end(&body);chat=(struct scroll){0};follow=true;
	spring_to(&slide,1);
}

static void close_thread(void){spring_to(&slide,0);spring_to(&press,0);tracking=dragged=false;}

static void confirmed(int answer){
	if(answer!=1 || !modem_job_start(&job,MODEM_SMS,number,draft)){return;}
	pending=(struct message){.outgoing=1,.status=MESSAGE_PENDING};
	snprintf(pending.number,sizeof(pending.number),"%s",number);
	snprintf(pending.body,sizeof(pending.body),"%s",draft);
	saving=message_save(&pending);draft[0]=0;field_end(&body);reload();follow=true;compose=C_BODY;
}

static void send_message(void){
	if(job.running){return;}
	if(compose==C_CONTACTS){compose=C_TO;}
	if(!modem_number(number)){compose=C_TO;return;}
	if(!*draft){compose=C_BODY;return;}
	char text[160];const char *buttons[]={"Cancel","Send"};
	snprintf(text,sizeof(text),"Send to %s?\nLong messages use multiple SMS parts. Network charges may apply.",number);
	modal("Send message?",text,buttons,2,confirmed);
}

static float wrapped(const char *s,float x,float y,float width,bool draw){
	float left=x;
	while(*s){
		unsigned n=1;char value[5];
		if((unsigned char)*s>=0xf0){n=4;}else if((unsigned char)*s>=0xe0){n=3;}else if((unsigned char)*s>=0xc0){n=2;}
		if(strlen(s)<n){break;}memcpy(value,s,n);value[n]=0;s+=n;
		float w=text(F_TEXT,0,0,value,17,C_WHITE,0,0);
		if(value[0]=='\n' || x+w>left+width){x=left;y+=22;if(value[0]=='\n'){continue;}}
		if(draw){text(F_TEXT,x,y,value,17,C_WHITE,1,0);}x+=w;
	}
	return y;
}

static float bubble(struct message *m,float *width){
	*width=clampf(text(F_TEXT,0,-100,m->body,17,C_WHITE,0,0)+24,120,app_w-71);
	return wrapped(m->body,0,0,*width-24,false)+57;
}

static void draw_list(void){
	float top=-list.x;
	list.max=fmaxf(0,TOP+nthreads*ROW+12-app_h);
	text(F_LIGHT,20,44+top,"Messages",34,C_WHITE,1,0);
	if(keys && selected<0){fill(app_w-61,11+top,46,46,23,C_WHITE,C_WHITE,.85f);}
	fill(app_w-58,14+top,40,40,20,C_ACCENT,C_ACCENT2,1);icon(app_w-38,34+top,I_COMPOSE,22,C_WHITE,1);
	if(!nthreads){icon(app_w/2,app_h*.45f,I_CHAT,48,C_WHITE,.25f);text(F_TEXT,app_w/2,app_h*.45f+48,"No conversations yet",18,C_WHITE,.5f,1);}
	for(int j=0;j<nthreads;j++){
		float y=TOP+j*ROW-list.x;if(y+ROW<0 || y>app_h){continue;}
		if(keys && selected==j){fill(8,y,app_w-16,66,10,0x144c80,0x08335c,.35f);}
		struct message *m=&history[threads[j]];
		fill(20,y+11,44,44,12,C_TILE,C_TILE2,1);icon(42,y+33,I_PERSON,26,C_WHITE,.85f);
		text(F_TEXT,76,y+25,contacts_name(m->number),20,C_WHITE,1,0);
		char preview[MESSAGE_TEXT];snprintf(preview,sizeof(preview),"%s",m->body);
		for(char *p=preview;*p;p++){if(*p=='\n'){*p=' ';}}
		clip(76,y+32,app_w-24,y+64);text(F_TEXT,76,y+51,preview,15,C_WHITE,.6f,0);clip(0,0,app_w,app_h);
		fill(76,y+ROW-1,app_w-92,1,0,C_WHITE,C_WHITE,.12f);
	}
}

static void draw_chat(void){
	int bottom=app_h;float top=new_number ? 104 : 65,end=bottom-102,total=0,w;
	icon(28,32,0xe5c4,24,C_WHITE,1);
	clip(48,0,app_w-16,56);text(F_LIGHT,52,44,new_number ? "New message" : contacts_name(number),25,C_WHITE,1,0);clip(0,0,app_w,app_h);
	if(new_number){
		field_draw(&recipient,20,58,app_w-100,36,18,"To: + Phone number",compose==C_TO && !is_open(&picker),false);
		if(keys && compose==C_CONTACTS){fill(app_w-73,55,56,42,10,C_WHITE,C_WHITE,.85f);}
		fill(app_w-70,58,50,36,8,C_TILE,C_TILE2,1);icon(app_w-45,76,0xe0ba,24,C_WHITE,1);
	}
	for(int i=0;i<count;i++){if(!strcmp(history[i].number,number)){total+=bubble(&history[i],&w)+8;}}
	chat.max=fmaxf(0,total-(end-top));
	if(follow){chat.x=chat.max;chat.v=0;follow=false;}
	float y=top+fmaxf(0,end-top-total)-chat.x;
	clip(12,top,app_w-12,end);
	if(keys && compose==C_HISTORY){fill(12,top,2,end-top,1,C_ACCENT,C_ACCENT2,1);}
	for(int i=0;i<count;i++){
		struct message *m=&history[i];if(strcmp(m->number,number)){continue;}
		float h=bubble(m,&w),x=m->outgoing ? app_w-16-w : 16;
		if(y+h>top && y<end){
			fill(x,y,w,h,10,m->outgoing ? 0x17558b : C_TILE,m->outgoing ? 0x103b61 : C_TILE2,1);
			wrapped(m->body,x+12,y+24,w-24,true);
			const char *state=!m->outgoing ? "Received" : m->status==MESSAGE_PENDING ? "Sending" : m->status==MESSAGE_SENT ? "Delivered" : "Unconfirmed";
			text(F_TEXT,x+w-(m->outgoing ? 31 : 12),y+h-9,state,12,C_WHITE,.55f,2);
			if(m->outgoing){icon(x+w-16,y+h-13,m->status==MESSAGE_PENDING ? I_PENDING : m->status==MESSAGE_SENT ? I_DONE : I_ERROR,15,C_WHITE,.65f);}
		}
		y+=h+8;
	}
	clip(0,0,app_w,app_h);
	field_draw(&body,14,bottom-92,app_w-108,76,17,"Message",compose==C_BODY && !is_open(&picker),true);
	if(keys && compose==C_SEND){fill(app_w-87,bottom-95,76,82,13,C_WHITE,C_WHITE,.85f);}
	fill(app_w-84,bottom-92,70,76,10,C_ACCENT,C_ACCENT2,job.running ? .35f : 1);
	fill(app_w-84,bottom-92,70,76,10,0,0,.22f*press.x);
	icon(app_w-49,bottom-65,job.running ? I_PENDING : I_SEND,24,C_WHITE,1);
	text(F_TEXT,app_w-49,bottom-32,job.running ? "Sending" : "Send",14,C_WHITE,1,1);
}

static void draw_picker(void){
	float x=app_w*(1-picker.x);
	fill(0,0,app_w,app_h,0,0,0,.5f*clampf(picker.x,0,1));
	app_view(fmaxf(0,x),0,app_w,app_h);clip(0,0,app_w,app_h);background();app_offset(x,0);
	icon(28,32,0xe5c4,24,C_WHITE,1);text(F_LIGHT,54,44,"Choose recipient",28,C_WHITE,1,0);
	contact_list.max=fmaxf(0,66+ncontacts*64+12-app_h);clip(0,60,app_w,app_h);
	if(!ncontacts){text(F_TEXT,app_w/2,app_h/2,"No contacts yet",18,C_WHITE,.6f,1);}
	for(int i=0;i<ncontacts;i++){
		float y=66+i*64-contact_list.x;if(y+64<60 || y>app_h){continue;}
		if(keys && contact_selected==i){fill(8,y,app_w-16,60,10,0x144c80,0x08335c,.35f);}
		icon(32,y+28,I_PERSON,24,C_WHITE,.8f);text(F_TEXT,58,y+25,contacts[i].name,19,C_WHITE,1,0);
		text(F_TEXT,58,y+48,contacts[i].number,15,C_WHITE,.6f,0);
	}
	app_offset(0,0);app_view(0,0,app_w,app_h);clip(0,0,app_w,app_h);
}

void app_draw(void){
	float px=app_w*(1-slide.x);
	if(px>0){
		app_view(0,0,px,app_h);clip(0,0,app_w,app_h);background();
		app_offset(-app_w*.3f*slide.x,0);draw_list();app_offset(0,0);
		fill(0,0,px,app_h,0,0,0,.5f*clampf(slide.x,0,1));
	}
	if(px<app_w){
		app_view(fmaxf(0,px),0,app_w,app_h);clip(0,0,app_w,app_h);background();app_offset(px,0);draw_chat();app_offset(0,0);
	}
	app_view(0,0,app_w,app_h);clip(0,0,app_w,app_h);
	if(picker.x>0){draw_picker();}
}

bool app_step(float dt){
	bool was=job.running;modem_job_poll(&job);
	bool moving=spring_step(&slide,dt,260);moving|=spring_step(&press,dt,900);
	moving|=spring_step(&picker,dt,260);
	if(!dragged){scroll_step(&contact_list,dt);moving|=scroll_moving(&contact_list);}
	if(!dragged){scroll_step(&list,dt);scroll_step(&chat,dt);moving|=scroll_moving(&list) || scroll_moving(&chat);}
	if(was && !job.running && saving){pending.status=job.delivered ? MESSAGE_SENT : MESSAGE_UNCONFIRMED;message_save(&pending);saving=false;reload();return true;}
	if(app_ms()-checked>2000){checked=app_ms();int previous=count;bool at_end=chat.x>=chat.max-4;reload();if(previous!=count){follow=at_end;return true;}}
	return moving;
}

static void track(double y){
	uint32_t now=app_ms();float dt=(now-move_time)/1000.f;
	if(dt>.0005f){velocity=velocity*.4+((last_y-y)/dt)*.6;}
	move_time=now;last_y=y;
}

void app_touch(int state,double x,double y){
	if(state==T_CANCEL){dragged=tracking=false;spring_to(&press,0);return;}
	if(is_open(&picker)){
		if(state==T_DOWN){keys=false;down_x=x;down_y=last_y=y;velocity=0;move_time=app_ms();dragged=false;scroll_from=contact_list.x;contact_list.v=0;return;}
		if(state==T_MOVE){
			if(down_y>=60 && fabs(y-down_y)>10 && fabs(y-down_y)>fabs(x-down_x)){dragged=true;}
			if(dragged){track(y);contact_list.x=scroll_rubber(&contact_list,scroll_from-(float)(y-down_y));}return;
		}
		if(state!=T_UP){return;}
		if(dragged){contact_list.v=velocity;dragged=false;return;}
		if(fabs(x-down_x)>10 || fabs(y-down_y)>10){return;}
		if(x<48 && y<58){spring_to(&picker,0);return;}
		int i=(y-66+contact_list.x)/64;
		if(y>=66 && i>=0 && i<ncontacts){contact_selected=i;choose_contact();}return;
	}
	int bottom=app_h;struct scroll *sc=scroller();
	if(state==T_DOWN){
		keys=false;down_x=x;down_y=last_y=y;velocity=0;move_time=app_ms();dragged=false;
		tracking=!conversation() || (y>=(new_number ? 104 : 65) && y<bottom-102);
		sc->v=0;scroll_from=sc->x;
		spring_to(&press,conversation() && x>app_w-92 && y>=bottom-92 ? 1 : 0);return;
	}
	if(state==T_MOVE){
		if(tracking && fabs(y-down_y)>10 && fabs(y-down_y)>fabs(x-down_x)){dragged=true;spring_to(&press,0);}
		if(dragged){track(y);sc->x=scroll_rubber(sc,scroll_from-(float)(y-down_y));}
		return;
	}
	if(state!=T_UP){return;}spring_to(&press,0);
	if(dragged){sc->v=velocity;dragged=tracking=false;return;}tracking=false;
	if(fabs(x-down_x)>10 || fabs(y-down_y)>10){return;}
	if(!conversation()){
		if(y+list.x<60 && x>app_w-90){selected=-1;open_thread(-1);return;}
		int i=(y+list.x-TOP)/ROW;if(y+list.x>=TOP && i>=0 && i<nthreads){selected=i;open_thread(i);}return;
	}
	if(x<48 && y<58){close_thread();}
	else if(new_number && y>=58 && y<100){if(x>=app_w-76){compose=C_CONTACTS;pick_contact();}else{compose=C_TO;field_end(&recipient);}}
	else if(y>=bottom-92){compose=x>app_w-92 ? C_SEND : C_BODY;if(compose==C_SEND){send_message();}else{field_end(&body);}}
	else{compose=C_HISTORY;}
}

static void select_thread(int next){
	selected=next;
	float y=next<0 ? 0 : TOP+next*ROW,h=next<0 ? TOP : ROW;
	if(y<list.x){list.x=y;}else if(y+h>list.x+app_h){list.x=y+h-app_h;}
	list.x=clampf(list.x,0,list.max);list.v=0;
}

void app_key(uint32_t key){
	keys=true;
	if(is_open(&picker)){
		if(key==KEY_ESC || key==KEY_LEFT){spring_to(&picker,0);return;}
		if(key==KEY_ENTER || key==KEY_OK || key==KEY_KPENTER){choose_contact();return;}
		if(key==KEY_UP && contact_selected>0){contact_selected--;}
		else if(key==KEY_DOWN && contact_selected+1<ncontacts){contact_selected++;}
		float y=66+contact_selected*64;if(y<contact_list.x+60){contact_list.x=y-60;}else if(y+64>contact_list.x+app_h){contact_list.x=y+64-app_h;}
		contact_list.x=clampf(contact_list.x,0,contact_list.max);contact_list.v=0;return;
	}
	if(key==KEY_ESC){if(conversation()){close_thread();}else{app_quit();}return;}
	if(!conversation()){
		if(key==KEY_DOWN && selected+1<nthreads){select_thread(selected+1);}
		else if(key==KEY_UP && selected>=0){select_thread(selected-1);}
		else if(key==KEY_TAB){select_thread(selected+1<nthreads ? selected+1 : -1);}
		else if(key==KEY_ENTER || key==KEY_OK || key==KEY_KPENTER){open_thread(selected);}
		return;
	}
	if(key==KEY_PAGEUP || key==KEY_PAGEDOWN){chat.x=clampf(chat.x+(key==KEY_PAGEUP ? -1 : 1)*fmaxf(40,app_h/2),0,chat.max);chat.v=0;return;}
	if(key==KEY_TAB){compose=compose==C_TO ? C_CONTACTS : compose==C_CONTACTS ? C_HISTORY : compose==C_HISTORY ? C_BODY : compose==C_BODY ? C_SEND : new_number ? C_TO : C_HISTORY;return;}
	if(compose==C_CONTACTS){if(key==KEY_ENTER || key==KEY_KPENTER || key==KEY_OK){pick_contact();}else if(key==KEY_LEFT){compose=C_TO;}else if(key==KEY_DOWN){compose=C_BODY;}else if(key==KEY_UP){compose=C_TO;}return;}
	if(compose==C_HISTORY){
		if(key==KEY_UP || key==KEY_DOWN){chat.x=clampf(chat.x+(key==KEY_UP ? -55 : 55),0,chat.max);chat.v=0;}
		else if(key==KEY_ENTER || key==KEY_OK || key==KEY_RIGHT){compose=C_BODY;}
		return;
	}
	if(key==KEY_UP){compose=compose==C_SEND ? C_BODY : compose==C_BODY && new_number ? C_TO : C_HISTORY;return;}
	if(key==KEY_DOWN){compose=compose==C_TO ? C_BODY : C_SEND;return;}
	if(key==KEY_ENTER || key==KEY_KPENTER || key==KEY_OK){if(compose==C_TO){compose=C_BODY;}else{send_message();}return;}
	if(compose==C_SEND){if(key==KEY_LEFT){compose=C_BODY;}return;}
	if(!job.running){field_key(compose==C_TO ? &recipient : &body,key);}
}

void app_text(const char *utf8){
	if(is_open(&picker)){return;}
	if(!conversation()){if(!strcmp(utf8,"n") || !strcmp(utf8,"N")){open_thread(-1);}return;}
	if(job.running){return;}
	if(compose==C_SEND || compose==C_HISTORY){compose=C_BODY;}
	field_text(compose==C_TO ? &recipient : &body,utf8);
}

int main(void){
	field_init(&recipient,number,sizeof(number),true);field_init(&body,draft,sizeof(draft),false);
	reload();int result=app_run("Messages","yamashell-messages");modem_job_close(&job);return result;
}