// SPDX-License-Identifier: GPL-2.0-only
/* brightness: the first /sys/class/backlight device (rapuyama-vc on the E6, DCS 0x51 to the panel).
 * volume: only a number in the config for now. both are kept in config.c across reboots */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include "level.h"
#include "config.h"

#define BACKLIGHT "/sys/class/backlight"
#define MIN_BRIGHTNESS 5 // guess why

static int read_int(const char *path){
	int v=-1;FILE *f=fopen(path,"r");
	if(f){if(fscanf(f,"%d",&v)!=1){v=-1;}fclose(f);}
	return v;
}

static void write_int(const char *path,int v){
	FILE *f=fopen(path,"w");
	if(f){fprintf(f,"%d\n",v);fclose(f);}
}

static int backlight(const char *name,char *path,size_t n){
	DIR *d=opendir(BACKLIGHT);struct dirent *e;int ok=0;
	while(d && !ok && (e=readdir(d))){
		if(e->d_name[0]=='.'){continue;}
		snprintf(path,n,BACKLIGHT "/%s/%s",e->d_name,name);ok=1;
	}
	if(d){closedir(d);}
	return ok;
}

static const char *const keys[LEVEL_COUNT]={"brightness","volume"};

int level_get(int which){
	char path[300];int v,max;
	/* shortcut */
	if(which==LEVEL_VOLUME){v=config_get(keys[which],path,sizeof(path)) ? atoi(path) : 50;return v<0 ? 0 : v>100 ? 100 : v;}
	if(!backlight("max_brightness",path,sizeof(path)) || (max=read_int(path))<=0){return -1;}
	/* the saved value, the panel may be dimmed for idle right now */
	if(config_get(keys[which],path,sizeof(path))){v=atoi(path);return v<0 ? 0 : v>100 ? 100 : v;}
	backlight("brightness",path,sizeof(path));
	v=read_int(path);
	return v<0 ? -1 : (v*100+max/2)/max;
}

void level_set(int which,int pct){
	char path[300];int max;
	pct=pct<0 ? 0 : pct>100 ? 100 : pct;
	if(which==LEVEL_BRIGHTNESS && pct<MIN_BRIGHTNESS){pct=MIN_BRIGHTNESS;}
	snprintf(path,sizeof(path),"%d",pct);
	config_set(keys[which],path);
	if(which==LEVEL_VOLUME){return;}
	if(!backlight("max_brightness",path,sizeof(path)) || (max=read_int(path))<=0){return;}
	backlight("brightness",path,sizeof(path));
	write_int(path,(pct*max+50)/100);
}

void level_restore(void){
	char v[16];
	if(config_get(keys[LEVEL_BRIGHTNESS],v,sizeof(v))){level_set(LEVEL_BRIGHTNESS,atoi(v));}
}
