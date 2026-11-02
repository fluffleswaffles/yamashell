// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "contacts.h"

static void value(char *out,size_t capacity,const char *s){
	size_t n=0;
	while(*s && n+1<capacity){
		if(*s=='\\' && s[1]){s++;out[n++]=*s=='n' || *s=='N' ? ' ' : *s;s++;}
		else{out[n++]=*s++;}
	}
	out[n]=0;
}

static int card_line(char *s,struct contact *card,struct contact *phones,int *nphones,int capacity){
	char *p=strchr(s,':');if(!p){return 0;}*p++=0;
	char *param=strchr(s,';');if(param){*param=0;}
	char *group=strrchr(s,'.');if(group){s=group+1;}
	if(!strcasecmp(s,"BEGIN") && !strcasecmp(p,"VCARD")){memset(card,0,sizeof(*card));*nphones=0;}
	else if(!strcasecmp(s,"FN")){value(card->name,sizeof(card->name),p);}
	else if(!strcasecmp(s,"N") && !*card->name){for(char *q=p;*q;q++){if(*q==';'){*q=' ';}}value(card->name,sizeof(card->name),p);}
	else if(!strcasecmp(s,"TEL") && *nphones<capacity){
		if(!strncasecmp(p,"tel:",4)){p+=4;}
		char *dest=phones[*nphones].number;size_t n=0;
		for(;*p && n+1<sizeof(phones[0].number);p++){if((*p>='0' && *p<='9') || (*p=='+' && !n)){dest[n++]=*p;}}
		dest[n]=0;if(n){(*nphones)++;}
	}
	else if(!strcasecmp(s,"END") && !strcasecmp(p,"VCARD")){
		for(int i=0;i<*nphones;i++){snprintf(phones[i].name,sizeof(phones[i].name),"%s",*card->name ? card->name : phones[i].number);}
		return *nphones;
	}
	return 0;
}

int contacts_read(const char *path,struct contact *out,int capacity){
	FILE *f=fopen(path,"r");if(!f){return 0;}
	struct contact card={0},phones[32]={0};char line[2048],pending[4096]="";int n=0,np=0;
	while(fgets(line,sizeof(line),f)){
		line[strcspn(line,"\r\n")]=0;
		if((line[0]==' ' || line[0]=='\t') && *pending){size_t used=strlen(pending);snprintf(pending+used,sizeof(pending)-used,"%s",line+1);continue;}
		int ready=card_line(pending,&card,phones,&np,32);
		for(int i=0;i<ready && n<capacity;i++){out[n++]=phones[i];}
		snprintf(pending,sizeof(pending),"%s",line);
	}
	int ready=card_line(pending,&card,phones,&np,32);
	for(int i=0;i<ready && n<capacity;i++){out[n++]=phones[i];}
	fclose(f);return n;
}

static int compare(const void *a,const void *b){return strcasecmp(((const struct contact *)a)->name,((const struct contact *)b)->name);}

static bool contact_path(char path[768]){
	const char *data=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
	if(data && *data){snprintf(path,768,"%s/yamashell/contacts.vcf",data);}
	else if(home && *home){snprintf(path,768,"%s/.local/share/yamashell/contacts.vcf",home);}
	else{return false;}
	return true;
}

int contacts_load(struct contact *out,int capacity){
	char path[768];if(!contact_path(path)){return 0;}
	int n=contacts_read(path,out,capacity);qsort(out,n,sizeof(*out),compare);return n;
}

static void escaped(FILE *f,const char *s){
	for(;*s;s++){if(*s=='\n' || *s=='\r'){fputs("\\n",f);}else{if(*s==',' || *s==';' || *s=='\\'){fputc('\\',f);}fputc(*s,f);}}
}

bool contacts_save(const struct contact *contacts,int count){
	char path[768],tmp[800];if(!contact_path(path)){return false;}
	for(char *p=path+1;*p;p++){if(*p=='/'){*p=0;mkdir(path,0700);*p='/';}}
	snprintf(tmp,sizeof(tmp),"%s.%u",path,(unsigned)getpid());
	int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){return false;}
	FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(tmp);return false;}
	for(int i=0;i<count;i++){
		fputs("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:",f);escaped(f,contacts[i].name);
		fputs("\r\nTEL;TYPE=CELL:",f);escaped(f,contacts[i].number);fputs("\r\nEND:VCARD\r\n",f);
	}
	bool ok=!ferror(f);if(fflush(f) || fsync(fd)){ok=false;}if(fclose(f)){ok=false;}
	if(ok){ok=!rename(tmp,path);}if(!ok){unlink(tmp);}return ok;
}

const char *contacts_name(const char *number){
	static struct contact contacts[512];static int count;static time_t checked;
	struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
	if(!checked || now.tv_sec-checked>=2){checked=now.tv_sec;count=contacts_load(contacts,512);}
	for(int i=0;i<count;i++){if(!strcmp(number,contacts[i].number)){return contacts[i].name;}}
	return *number ? number : "Unknown caller";
}
