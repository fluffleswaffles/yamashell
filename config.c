// SPDX-License-Identifier: GPL-2.0-only
/* the file is rewritten whole into a temporary one and renamed over, so a crash or a reader
 * in another process never sees half of it */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "config.h"

#define LINE 512

bool config_dir(char *path,size_t n){
	const char *xdg=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");
	if(xdg && *xdg){snprintf(path,n,"%s",xdg);}
	else if(home && *home){snprintf(path,n,"%s/.config",home);}
	else{return false;}
	mkdir(path,0755);
	strncat(path,"/yamashell",n-strlen(path)-1);
	mkdir(path,0755);
	return true;
}

static bool file(char *path,size_t n){
	if(!config_dir(path,n)){return false;}
	strncat(path,"/settings.conf",n-strlen(path)-1);
	return true;
}

/* the value if line is "key=..." */
static const char *match(const char *line,const char *key){
	size_t k=strlen(key);
	return !strncmp(line,key,k) && line[k]=='=' ? line+k+1 : NULL;
}

bool config_get(const char *key,char *val,size_t n){
	char path[300],line[LINE];const char *v;bool found=false;
	if(!file(path,sizeof(path))){return false;}
	FILE *f=fopen(path,"r");
	while(f && !found && fgets(line,sizeof(line),f)){
		if((v=match(line,key))){snprintf(val,n,"%.*s",(int)strcspn(v,"\n"),v);found=true;}
	}
	if(f){fclose(f);}
	return found;
}

void config_set(const char *key,const char *val){
	char path[300],tmp[320],line[LINE],old[LINE];const char *v;
	if(config_get(key,old,sizeof(old)) && !strcmp(old,val)){return;}
	if(!file(path,sizeof(path))){return;}
	snprintf(tmp,sizeof(tmp),"%s.%d",path,(int)getpid());
	FILE *in=fopen(path,"r"),*out=fopen(tmp,"w");
	if(!out){if(in){fclose(in);}return;}
	while(in && fgets(line,sizeof(line),in)){if(!(v=match(line,key))){fputs(line,out);}}
	fprintf(out,"%s=%s\n",key,val);
	if(in){fclose(in);}
	if(fclose(out) || rename(tmp,path)){unlink(tmp);}
}

const char *config_device(void){
	char c[256]={0};size_t len=0;
	FILE *f=fopen("/sys/firmware/devicetree/base/compatible","r");
	if(f){len=fread(c,1,sizeof(c)-1,f);fclose(f);}
	for(char *p=c;p<c+len;p+=strlen(p)+1){
		if(!strcmp(p,"nokia,n8")){return "n8";}
		if(!strcmp(p,"nokia,e7")){return "e7";}
	}
	return "e6";
}

bool config_aod(void){
	char v[8];
	if(config_get("aod",v,sizeof(v))){return atoi(v)!=0;}
	return strcmp(config_device(),"e6")!=0;
}
