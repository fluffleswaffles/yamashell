// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include "app.h"
#include "modem.h"
#include "contacts.h"
#include "field.h"
#include "calls.h"

enum{P_RECENT,P_CONTACTS,P_DIAL};
static struct modem_job job={.fd=-1};
static struct contact contacts[512],recent[128];
static struct call_record records[128];
static uint32_t checked;
static int ncontacts,nrecent,page,selected;
static char number[24];
static struct field dial;
static struct field fields[2];
static char contact_name[128],contact_number[24];
static int editing=-1,edit_focus;
static struct spring editor;
static double down_x,down_y,last_y;
static bool dragged,keys,tabs_focus=true;
static struct spring tab,focus,press;
static struct scroll lists[2];
static float scroll_from;
static double velocity;
static uint32_t move_time;

/* material icons */
#define I_HISTORY 0xe889
#define I_CONTACTS 0xe0ba
#define I_DIAL 0xe0bc
#define I_CALL 0xe0b0
#define I_END 0xe0b1
#define I_DELETE 0xe14a

static void open_page(int next){
	if(next==page){return;}
	page=next;selected=0;tabs_focus=true;
	spring_to(&tab,page);focus=(struct spring){0};
}

static bool keypad(void){return app_h>app_w || !app_hardware_keyboard();}

static void history_path(char *path,size_t size){
	const char *data=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
	if(data && *data){snprintf(path,size,"%s/yamashell/calls",data);}
	else{snprintf(path,size,"%s/.local/share/yamashell/calls",home ? home : "/root");}
}

static void load_recent(void){
	int n=calls_load(records,128);if(!n){return;}nrecent=n;
	for(int i=0;i<n;i++){
		snprintf(recent[i].number,sizeof(recent[i].number),"%s",records[i].number);
		snprintf(recent[i].name,sizeof(recent[i].name),"%s",contacts_name(records[i].number));
	}
}

static void edit_contact(int index){
	editing=index;edit_focus=0;
	snprintf(contact_name,sizeof(contact_name),"%s",index<0 ? "" : contacts[index].name);
	snprintf(contact_number,sizeof(contact_number),"%s",index<0 ? "" : contacts[index].number);
	field_end(&fields[0]);field_end(&fields[1]);spring_to(&editor,1);
}

static void save_contact(void){
	const char *buttons[]={"OK"};
	if(!modem_number(contact_number)){edit_focus=1;modal("Phone number","Use an international number starting with +.",buttons,1,NULL);return;}
	int index=editing<0 ? ncontacts : editing;
	if(index>=512){modal("Contacts","The contact list is full.",buttons,1,NULL);return;}
	struct contact old=contacts[index];
	snprintf(contacts[index].name,sizeof(contacts[index].name),"%s",*contact_name ? contact_name : contact_number);
	snprintf(contacts[index].number,sizeof(contacts[index].number),"%s",contact_number);
	if(!contacts_save(contacts,ncontacts+(editing<0))){contacts[index]=old;modal("Contacts","Could not save the contact.",buttons,1,NULL);return;}
	ncontacts=contacts_load(contacts,512);spring_to(&editor,0);
}

static void delete_contact(int answer){
	if(answer!=1 || editing<0 || editing>=ncontacts){return;}
	memmove(contacts+editing,contacts+editing+1,(ncontacts-editing-1)*sizeof(*contacts));
	bool saved=contacts_save(contacts,ncontacts-1);ncontacts=contacts_load(contacts,512);
	if(saved){spring_to(&editor,0);}
	else{const char *buttons[]={"OK"};modal("Contacts","Could not delete the contact.",buttons,1,NULL);}
}

static void edit_action(void){
	if(edit_focus<2){edit_focus++;}
	else if(edit_focus==2){save_contact();}
	else{const char *buttons[]={"Cancel","Delete"};modal("Delete contact?","This removes the contact from the phone, not from the SIM.",buttons,2,delete_contact);}
}

static void contact_action(int answer){
	if(selected<1 || selected-1>=ncontacts){return;}
	if(answer==1){edit_contact(selected-1);}
	else if(answer==2){snprintf(number,sizeof(number),"%s",contacts[selected-1].number);field_end(&dial);open_page(P_DIAL);tabs_focus=false;modem_job_start(&job,MODEM_CALL,number,NULL);}
}

static void activate(void){
	if(job.running){modem_job_stop(&job);return;}
	if(page!=P_DIAL){
		if(page==P_CONTACTS){
			if(selected==0){edit_contact(-1);return;}
			if(selected-1<ncontacts){const char *buttons[]={"Cancel","Edit","Call"};modal(contacts[selected-1].name,contacts[selected-1].number,buttons,3,contact_action);}return;
		}
		struct contact *list=recent;int n=nrecent;
		if(!n){return;}snprintf(number,sizeof(number),"%s",list[selected].number);field_end(&dial);open_page(P_DIAL);tabs_focus=false;return;
	}
	if(!modem_number(number)){
		const char *buttons[]={"OK"};modal("Phone number","Use an international number starting with +.",buttons,1,NULL);return;
	}
	modem_job_start(&job,MODEM_CALL,number,NULL);
}

static void draw_page(int pane){
	if(pane!=P_DIAL){
		struct scroll *sc=&lists[pane];
		struct contact *list=pane==P_CONTACTS ? contacts : recent;int n=pane==P_CONTACTS ? ncontacts+1 : nrecent;
		sc->max=fmaxf(0,108+n*64+12-app_h);
		if(!n){
			icon(app_w/2,app_h*.48f,pane==P_CONTACTS ? I_CONTACTS : I_HISTORY,48,C_WHITE,.25f);
			text(F_TEXT,app_w/2,app_h*.48f+48,pane==P_CONTACTS ? "No contacts yet" : "No recent calls",18,C_WHITE,.6f,1);
		}
		clip(0,104,app_w,app_h);
		if(n && keys && !tabs_focus && pane==page){fill(8,108+focus.x*64-sc->x,app_w-16,60,10,0x144c80,0x08335c,.35f);}
		for(int i=0;i<n;i++){
			float y=108+i*64-sc->x;if(y+64<104){continue;}if(y>app_h){break;}
			if(pane==P_CONTACTS && !i){
				icon(32,y+28,0xe7fe,24,C_WHITE,.8f);
				text(F_TEXT,58,y+36,"New contact",19,C_WHITE,1,0);continue;
			}
			int index=pane==P_CONTACTS ? i-1 : i;
			icon(32,y+28,pane==P_CONTACTS ? 0xe7fd : I_CALL,24,C_WHITE,.8f);
			text(F_TEXT,58,y+25,list[index].name,19,C_WHITE,1,0);
			if(pane==P_RECENT && records[index].magic==0x59434c31){
				char stamp[40],detail[128];time_t when=records[index].started;struct tm tm;localtime_r(&when,&tm);strftime(stamp,sizeof(stamp),"%d %b %H:%M",&tm);
				snprintf(detail,sizeof(detail),"%s · %s",call_result(records[index].result),stamp);
				if(records[index].answered && records[index].ended>=records[index].answered){
					unsigned long long duration=records[index].ended-records[index].answered;
					snprintf(detail,sizeof(detail),"%s · %s · %llu:%02llu",call_result(records[index].result),stamp,duration/60,duration%60);
				}
				text(F_TEXT,58,y+48,detail,14,C_WHITE,records[index].result==CALL_MISSED ? 1 : .6f,0);
			}else{text(F_TEXT,58,y+48,list[index].number,15,C_WHITE,.6f,0);}
		}
		return;
	}
	field_draw(&dial,20,112,app_w-40,52,26,"+ Phone number",!tabs_focus && !job.running,false);
	if(job.running){text(F_TEXT,app_w/2,180,"Calling…",17,C_WHITE,.7f,1);}
	else if(job.result){text(F_TEXT,app_w/2,180,"Call unavailable",17,C_WHITE,.7f,1);}
	if(keypad() && !job.running){
		const char *digits="123456789+0";float h=fminf(52,(app_h-260)/4.f),w=(app_w-48)/3.f;
		for(int i=0;i<12;i++){
			float x=18+(i%3)*w,y=194+(i/3)*h;char label[4]={i<11 ? digits[i] : 0,0};
			fill(x+3,y+2,w-6,h-4,10,C_TILE,C_TILE2,1);
			if(i==11){icon(x+w/2,y+h/2,I_DELETE,23,C_WHITE,1);}
			else{text(F_TEXT,x+w/2,y+h*.68f,label,23,C_WHITE,1,1);}
		}
	}
	if(keypad()){
		fill(24,app_h-62,app_w-48,46,23,job.running ? 0xb84141 : C_ACCENT,job.running ? 0x782525 : C_ACCENT2,1);
		fill(24,app_h-62,app_w-48,46,23,0,0,.22f*press.x);
		icon(app_w/2,app_h-39,job.running ? I_END : I_CALL,26,C_WHITE,1);
	}
}

void app_draw(void){
	background();text(F_LIGHT,20,44,"Phone",34,C_WHITE,1,0);
	const char *tabs[]={"Recent","Contacts","Dial"};float tw=app_w/3.f;
	static const uint32_t icons[]={I_HISTORY,I_CONTACTS,I_DIAL};
	for(int i=0;i<3;i++){
		float width=text(F_TEXT,0,-100,tabs[i],17,C_WHITE,0,0),cx=(i+.5f)*tw;
		if(keys && tabs_focus && page==i){fill(i*tw+4,62,tw-8,30,8,0x144c80,0x08335c,.35f);}
		icon(cx-width/2-3,79,icons[i],21,page==i ? C_ACCENT : C_WHITE,page==i ? 1 : .5f);
		text(F_TEXT,cx+14,85,tabs[i],17,C_WHITE,page==i ? 1 : .5f,1);
	}
	fill(tab.x*tw+14,96,tw-28,3,1,C_ACCENT,C_ACCENT2,1);
	for(int i=0;i<3;i++){
		float ox=(i-tab.x)*app_w;if(ox>=app_w || ox+app_w<=0){continue;}
		app_view(fmaxf(0,ox),104,fminf(app_w,ox+app_w),app_h);
		app_offset(ox,0);clip(0,104,app_w,app_h);draw_page(i);app_offset(0,0);
	}
	app_view(0,0,app_w,app_h);clip(0,0,app_w,app_h);
	if(editor.x>0){
		float x=app_w*(1-editor.x);
		fill(0,0,app_w,app_h,0,0,0,.5f*clampf(editor.x,0,1));
		app_view(fmaxf(0,x),0,app_w,app_h);clip(0,0,app_w,app_h);background();app_offset(x,0);
		icon(28,32,0xe5c4,24,C_WHITE,1);text(F_LIGHT,54,44,editing<0 ? "New contact" : "Edit contact",28,C_WHITE,1,0);
		text(F_TEXT,20,80,"Name",15,C_WHITE,.6f,0);field_draw(&fields[0],16,88,app_w-32,44,19,"Contact name",edit_focus==0,false);
		text(F_TEXT,20,154,"Phone number",15,C_WHITE,.6f,0);field_draw(&fields[1],16,162,app_w-32,44,19,"+ Phone number",edit_focus==1,false);
		float w=editing<0 ? app_w-32 : (app_w-48)/2.f;
		if(keys && edit_focus==2){fill(13,app_h-65,w+6,50,25,C_WHITE,C_WHITE,.85f);}
		fill(16,app_h-62,w,44,22,C_ACCENT,C_ACCENT2,1);text(F_TEXT,16+w/2,app_h-33,"Save",19,C_WHITE,1,1);
		if(editing>=0){float x=32+w;if(keys && edit_focus==3){fill(x-3,app_h-65,w+6,50,25,C_WHITE,C_WHITE,.85f);}fill(x,app_h-62,w,44,22,C_TILE,C_TILE2,1);text(F_TEXT,x+w/2,app_h-33,"Delete",19,C_WHITE,1,1);}
		app_offset(0,0);app_view(0,0,app_w,app_h);clip(0,0,app_w,app_h);
	}
}

bool app_step(float dt){
	bool was=job.running;modem_job_poll(&job);
	spring_to(&focus,selected);
	bool moving=spring_step(&tab,dt,260);moving|=spring_step(&editor,dt,260);moving|=spring_step(&focus,dt,500);moving|=spring_step(&press,dt,900);
	if(!dragged){for(int i=0;i<2;i++){scroll_step(&lists[i],dt);moving|=scroll_moving(&lists[i]);}}
	if(app_ms()-checked>2000 && !is_open(&editor)){checked=app_ms();ncontacts=contacts_load(contacts,512);load_recent();int n=page==P_CONTACTS ? ncontacts+1 : nrecent;if(selected>=n){selected=n ? n-1 : 0;}moving=true;}
	return moving || was!=job.running;
}

void app_touch(int state,double x,double y){
	if(state==T_CANCEL){dragged=false;spring_to(&press,0);return;}
	if(is_open(&editor)){
		if(state==T_DOWN){keys=false;down_x=x;down_y=y;return;}
		if(state!=T_UP || fabs(x-down_x)>10 || fabs(y-down_y)>10){return;}
		if(x<48 && y<58){spring_to(&editor,0);}
		else if(y>=88 && y<132){edit_focus=0;field_end(&fields[0]);}
		else if(y>=162 && y<206){edit_focus=1;field_end(&fields[1]);}
		else if(y>=app_h-62 && y<app_h-18){edit_focus=editing>=0 && x>app_w/2 ? 3 : 2;edit_action();}
		return;
	}
	if(state==T_DOWN){
		keys=false;down_x=x;down_y=last_y=y;dragged=false;velocity=0;move_time=app_ms();
		if(page!=P_DIAL){scroll_from=lists[page].x;lists[page].v=0;}
		spring_to(&press,page==P_DIAL && keypad() && y>=app_h-62 ? 1 : 0);return;
	}
	if(state==T_MOVE){
		if(page!=P_DIAL && down_y>=104 && fabs(y-down_y)>10 && fabs(y-down_y)>fabs(x-down_x)){dragged=true;}
		if(dragged){
			uint32_t now=app_ms();float dt=(now-move_time)/1000.f;
			if(dt>.0005f){velocity=velocity*.4+((last_y-y)/dt)*.6;}move_time=now;last_y=y;
			lists[page].x=scroll_rubber(&lists[page],scroll_from-(float)(y-down_y));
		}
		return;
	}
	if(state==T_UP){spring_to(&press,0);if(dragged){lists[page].v=velocity;dragged=false;return;}}
	if(state!=T_UP || dragged || fabs(x-down_x)>20){return;}
	if(y>=60 && y<104 && x>=0 && x<app_w){open_page((int)(x*3/app_w));return;}
	if(page!=P_DIAL){int i=(y-108+lists[page].x)/64,n=page==P_CONTACTS ? ncontacts+1 : nrecent;if(y>=108 && i>=0 && i<n){selected=i;activate();}return;}
	if(y>=112 && y<164){tabs_focus=false;field_end(&dial);return;}
	if(keypad() && y>=app_h-62){activate();return;}
	if(keypad() && !job.running){
		float h=fminf(52,(app_h-260)/4.f),w=(app_w-48)/3.f;
		if(x<18 || x>=18+3*w || y<194 || y>=194+4*h){return;}
		int i=(int)((y-194)/h)*3+(int)((x-18)/w);
		if(i==11){field_key(&dial,KEY_BACKSPACE);}else{char s[2]={"123456789+0"[i],0};app_text(s);}
	}
}

void app_key(uint32_t key){
	keys=true;
	if(is_open(&editor)){
		if(key==KEY_ESC){spring_to(&editor,0);}
		else if(key==KEY_TAB || key==KEY_DOWN){edit_focus=(edit_focus+1)%(editing<0 ? 3 : 4);}
		else if(key==KEY_UP){int n=editing<0 ? 3 : 4;edit_focus=(edit_focus+n-1)%n;}
		else if(key==KEY_ENTER || key==KEY_KPENTER || key==KEY_OK){edit_action();}
		else if(edit_focus<2){field_key(&fields[edit_focus],key);}
		return;
	}
	if(key==KEY_PICKUP_PHONE || key==KEY_PHONE){
		if(page==P_CONTACTS && selected>0){contact_action(2);}
		else{if(page==P_RECENT && nrecent){activate();}activate();}
		return;
	}
	if(key==KEY_ESC){if(job.running){modem_job_stop(&job);}else{app_quit();}return;}
	if(key==KEY_TAB){tabs_focus=!tabs_focus;return;}
	if((key==KEY_LEFT || key==KEY_RIGHT) && (tabs_focus || page!=P_DIAL)){open_page((page+(key==KEY_LEFT ? 2 : 1))%3);return;}
	if(tabs_focus && key==KEY_DOWN){tabs_focus=false;return;}
	if(page==P_DIAL && key==KEY_UP){tabs_focus=true;return;}
	if(key==KEY_ENTER || key==KEY_KPENTER || key==KEY_OK || key==KEY_PHONE){if(tabs_focus && key!=KEY_PHONE){tabs_focus=false;}else{activate();}return;}
	if(page==P_DIAL){if(!job.running){field_key(&dial,key);}return;}
	int n=page==P_CONTACTS ? ncontacts+1 : nrecent;
	if(key==KEY_DOWN && selected+1<n){selected++;}else if(key==KEY_UP){if(selected>0){selected--;}else{tabs_focus=true;}}
	struct scroll *sc=&lists[page];float y=108+selected*64;
	if(y<sc->x+104){sc->x=y-104;}else if(y+64>sc->x+app_h){sc->x=y+64-app_h;}
	sc->x=clampf(sc->x,0,sc->max);sc->v=0;
}

void app_text(const char *utf8){if(is_open(&editor)){if(edit_focus<2){field_text(&fields[edit_focus],utf8);}return;}if(!job.running && field_text(&dial,utf8)){open_page(P_DIAL);tabs_focus=false;}}

int main(void){
	umask(077);
	field_init(&dial,number,sizeof(number),true);
	field_init(&fields[0],contact_name,sizeof(contact_name),false);field_init(&fields[1],contact_number,sizeof(contact_number),true);
	ncontacts=contacts_load(contacts,512);
	char path[768],line[64];history_path(path,sizeof(path));FILE *f=fopen(path,"r");
	if(f){while(fgets(line,sizeof(line),f)){line[strcspn(line,"\r\n")]=0;if(!modem_number(line)){continue;}if(nrecent==128){nrecent--;}memmove(recent+1,recent,nrecent*sizeof(*recent));nrecent++;snprintf(recent[0].number,sizeof(recent[0].number),"%.23s",line);snprintf(recent[0].name,sizeof(recent[0].name),"%s",line);}fclose(f);}
	load_recent();
	int result=app_run("Phone","yamashell-phone");modem_job_close(&job);return result;
}