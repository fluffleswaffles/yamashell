// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app.h"
#include "contacts.h"
#include "field.h"
#include "modem.h"

static struct contact contacts[512],imported[512];
static int count,editing=-1,focus,selected,nfiles,nimport;
static char name[128],number[24],path[768],chosen[1024];
static struct field fields[2];
static bool importing,keys,dragged;
static struct spring press;
static struct scroll list;
static struct{char name[256];bool directory;}files[256];
static double down_x,down_y,last_y,velocity;
static float scroll_from;
static uint32_t moved;

static void done(int answer){(void)answer;app_quit();}
static void error(const char *body){const char *buttons[]={"OK"};modal("Contacts",body,buttons,1,NULL);}

static int compare(const void *a,const void *b){
	const typeof(files[0]) *x=a,*y=b;
	return x->directory!=y->directory ? (x->directory ? -1 : 1) : strcasecmp(x->name,y->name);
}

static void load_files(void){
	DIR *dir=opendir(path);if(!dir){error("Could not open this folder.");return;}
	struct dirent *entry;nfiles=0;selected=0;list=(struct scroll){0};
	while(nfiles<256 && (entry=readdir(dir))){
		if(entry->d_name[0]=='.'){continue;}
		struct stat st;if(fstatat(dirfd(dir),entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){continue;}
		const char *ext=strrchr(entry->d_name,'.');
		if(!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) && ext && !strcasecmp(ext,".vcf"))){continue;}
		snprintf(files[nfiles].name,sizeof(files[nfiles].name),"%s",entry->d_name);files[nfiles++].directory=S_ISDIR(st.st_mode);
	}
	closedir(dir);qsort(files,nfiles,sizeof(*files),compare);
}

static void parent(void){
	if(!strcmp(path,"/")){app_quit();return;}
	char *p=strrchr(path,'/');if(p==path){path[1]=0;}else if(p){*p=0;}else{strcpy(path,"/");}
	load_files();
}

static void imported_answer(int answer){
	if(answer!=1){return;}
	int total=count;
	for(int i=0;i<nimport;i++){
		bool duplicate=false;
		for(int j=0;j<total;j++){if(!strcmp(imported[i].number,contacts[j].number)){duplicate=true;break;}}
		if(duplicate){continue;}
		if(total==512){error("The contact list is full. No changes were saved.");return;}
		contacts[total++]=imported[i];
	}
	if(!contacts_save(contacts,total)){error("Could not save contacts. The existing file was not replaced.");return;}
	char body[96];snprintf(body,sizeof(body),"Imported %d contacts.",total-count);count=total;
	const char *buttons[]={"Done"};modal("Contacts imported",body,buttons,1,done);
}

static void open_file(void){
	if(!nfiles){return;}
	if(snprintf(chosen,sizeof(chosen),"%s%s%s",path,strcmp(path,"/") ? "/" : "",files[selected].name)>=(int)sizeof(chosen)){error("The path is too long.");return;}
	if(files[selected].directory){
		if(strlen(chosen)>=sizeof(path)){error("The path is too long.");return;}
		memcpy(path,chosen,strlen(chosen)+1);load_files();return;
	}
	nimport=contacts_read(chosen,imported,512);
	if(!nimport){error("No readable vCard contacts were found.");return;}
	char body[128];snprintf(body,sizeof(body),"Import %d phone numbers? Existing contacts are kept and duplicate numbers are skipped.",nimport);
	const char *buttons[]={"Cancel","Import"};modal("Import vCard",body,buttons,2,imported_answer);
}

static void save(void){
	if(!modem_number(number)){focus=1;error("Enter an international number starting with +.");return;}
	int index=editing;
	if(index<0){if(count==512){error("The contact list is full.");return;}index=count;}
	struct contact old=contacts[index];
	snprintf(contacts[index].name,sizeof(contacts[index].name),"%s",*name ? name : number);
	snprintf(contacts[index].number,sizeof(contacts[index].number),"%s",number);
	if(!contacts_save(contacts,count+(editing<0))){contacts[index]=old;error("Could not save this contact.");return;}
	app_quit();
}

static void delete_answer(int answer){
	if(answer!=1 || editing<0){return;}
	memmove(contacts+editing,contacts+editing+1,(count-editing-1)*sizeof(*contacts));
	if(!contacts_save(contacts,count-1)){count=contacts_load(contacts,512);error("Could not delete this contact.");return;}
	app_quit();
}

static void activate(void){
	if(importing){open_file();return;}
	if(focus<2){focus++;return;}
	if(focus==2){save();return;}
	const char *buttons[]={"Cancel","Delete"};modal("Delete contact?","This removes the contact from the phone, not from the SIM.",buttons,2,delete_answer);
}

void app_draw(void){
	background();icon(30,32,0xe5c4,24,C_WHITE,1);text(F_LIGHT,58,44,importing ? "Import vCard" : editing<0 ? "New contact" : "Edit contact",28,C_WHITE,1,0);
	if(importing){
		clip(18,56,app_w-18,82);text(F_TEXT,18,76,path,15,C_WHITE,.6f,0);clip(0,84,app_w,app_h);
		list.max=fmaxf(0,90+nfiles*62+12-app_h);
		for(int i=0;i<nfiles;i++){
			float y=90+i*62-list.x;if(y+62<84 || y>app_h){continue;}
			if(keys && selected==i){fill(8,y,app_w-16,58,10,0x144c80,0x08335c,.35f);}
			icon(32,y+29,files[i].directory ? 0xe2c7 : 0xe24d,24,C_WHITE,.8f);text(F_TEXT,58,y+35,files[i].name,18,C_WHITE,1,0);
		}
		clip(0,0,app_w,app_h);return;
	}
	text(F_TEXT,20,80,"Name",15,C_WHITE,.6f,0);field_draw(&fields[0],16,88,app_w-32,44,19,"Contact name",focus==0,false);
	text(F_TEXT,20,154,"Phone number",15,C_WHITE,.6f,0);field_draw(&fields[1],16,162,app_w-32,44,19,"+ Phone number",focus==1,false);
	float y=app_h-keyboard_height()-62,w=editing<0 ? app_w-32 : (app_w-48)/2.f;
	if(keys && focus==2){fill(13,y-3,w+6,50,25,C_WHITE,C_WHITE,.85f);}
	fill(16,y,w,44,22,C_ACCENT,C_ACCENT2,1);icon(16+w/2,y+22,0xe161,24,C_WHITE,1);
	if(editing>=0){float x=32+w;if(keys && focus==3){fill(x-3,y-3,w+6,50,25,C_WHITE,C_WHITE,.85f);}fill(x,y,w,44,22,C_TILE,C_TILE2,1);icon(x+w/2,y+22,0xe872,24,C_WHITE,1);}
	keyboard_draw(focus==1);
}

bool app_step(float dt){
	bool moving=spring_step(&press,dt,900);if(!dragged){scroll_step(&list,dt);moving|=scroll_moving(&list);}return moving;
}

void app_touch(int state,double x,double y){
	if(!importing && keyboard_touch(state,x,y,focus==1)){return;}
	if(state==T_DOWN){keys=false;down_x=x;down_y=last_y=y;dragged=false;velocity=0;moved=app_ms();scroll_from=list.x;list.v=0;return;}
	if(state==T_MOVE){
		if(importing && down_y>=84 && fabs(y-down_y)>10 && fabs(y-down_y)>fabs(x-down_x)){dragged=true;}
		if(dragged){uint32_t now=app_ms();float dt=(now-moved)/1000.f;if(dt>.0005f){velocity=velocity*.4+((last_y-y)/dt)*.6;}moved=now;last_y=y;list.x=scroll_rubber(&list,scroll_from-(float)(y-down_y));}return;
	}
	if(state!=T_UP){return;}
	if(dragged){list.v=velocity;dragged=false;return;}
	if(fabs(x-down_x)>10 || fabs(y-down_y)>10){return;}
	if(x<52 && y<58){if(importing){parent();}else{app_quit();}return;}
	if(importing){int i=(y-90+list.x)/62;if(y>=90 && i>=0 && i<nfiles){selected=i;open_file();}return;}
	if(y>=88 && y<132){focus=0;field_end(&fields[0]);}
	else if(y>=162 && y<206){focus=1;field_end(&fields[1]);}
	else if(y>=app_h-keyboard_height()-62){focus=editing>=0 && x>app_w/2 ? 3 : 2;activate();}
}

void app_key(uint32_t key){
	keys=true;
	if(key==KEY_ESC){if(importing){parent();}else{app_quit();}return;}
	if(key==KEY_ENTER || key==KEY_KPENTER || key==KEY_OK){activate();return;}
	if(importing){
		if(key==KEY_UP && selected>0){selected--;}else if(key==KEY_DOWN && selected+1<nfiles){selected++;}
		float y=90+selected*62;if(y<list.x+84){list.x=y-84;}else if(y+62>list.x+app_h){list.x=y+62-app_h;}list.x=clampf(list.x,0,list.max);list.v=0;return;
	}
	int last=editing<0 ? 2 : 3;
	if(key==KEY_DOWN || key==KEY_TAB){focus=focus<last ? focus+1 : 0;}
	else if(key==KEY_UP){focus=focus ? focus-1 : last;}
	else if(focus<2){field_key(&fields[focus],key);}
}

void app_text(const char *s){if(!importing){if(focus>=2){focus=0;}field_text(&fields[focus],s);}}

int main(int argc,char **argv){
	umask(077);count=contacts_load(contacts,512);importing=argc==2 && !strcmp(argv[1],"--import");
	if(importing){snprintf(path,sizeof(path),"%s",getenv("HOME") ? getenv("HOME") : "/");load_files();}
	else if(argc==2){for(int i=0;i<count;i++){if(!strcmp(contacts[i].number,argv[1])){editing=i;strcpy(name,contacts[i].name);strcpy(number,contacts[i].number);break;}}}
	field_init(&fields[0],name,sizeof(name),false);field_init(&fields[1],number,sizeof(number),true);
	return app_run(importing ? "Import vCard" : "Contact","yamashell-contacts");
}
