// SPDX-License-Identifier: GPL-2.0-only
/* Private per-message files; conversations are grouped by number by the UI. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "modem.h"
#include "messages.h"

static bool directory(char path[512]){
	const char *base=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
	if(base && *base){snprintf(path,512,"%s/yamashell/messages",base);}
	else{snprintf(path,512,"%s/.local/share/yamashell/messages",home ? home : "/root");}
	for(char *p=path+1;*p;p++){
		if(*p=='/'){*p=0;mkdir(path,0700);*p='/';}
	}
	mkdir(path,0700);
	struct stat st;
	return !lstat(path,&st) && S_ISDIR(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&077);
}

bool message_save(struct message *m){
	char dir[512],path[640],tmp[660];
	if(!modem_number(m->number) || !directory(dir)){return false;}
	if(!*m->id){
		static unsigned sequence;struct timespec now;clock_gettime(CLOCK_REALTIME,&now);
		snprintf(m->id,sizeof(m->id),"%016llx-%08x-%08x",(unsigned long long)now.tv_sec,(unsigned)getpid(),(unsigned)now.tv_nsec+sequence++);
	}
	for(const char *p=m->id;*p;p++){if((*p<'0' || *p>'9') && (*p<'a' || *p>'f') && *p!='-'){return false;}}
	m->magic=0x594d5331;if(!m->time){m->time=time(NULL);}
	snprintf(path,sizeof(path),"%s/%s.msg",dir,m->id);
	snprintf(tmp,sizeof(tmp),"%s.tmp-%u",path,(unsigned)getpid());
	int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
	if(fd<0){return false;}
	bool ok=write(fd,m,sizeof(*m))==(ssize_t)sizeof(*m);
	if(ok){ok=!fsync(fd);}close(fd);
	if(ok){ok=!rename(tmp,path);}if(!ok){unlink(tmp);}return ok;
}

static int by_time(const void *a,const void *b){
	const struct message *x=a,*y=b;
	if(x->time!=y->time){return x->time<y->time ? -1 : 1;}return strcmp(x->id,y->id);
}

static unsigned put_utf8(char *out,unsigned at,unsigned value){
	if(at+4>=MESSAGE_TEXT){return at;}
	if(value<128){out[at++]=value;}
	else if(value<2048){out[at++]=0xc0|(value>>6);out[at++]=0x80|(value&63);}
	else if(value<65536){out[at++]=0xe0|(value>>12);out[at++]=0x80|((value>>6)&63);out[at++]=0x80|(value&63);}
	else{out[at++]=0xf0|(value>>18);out[at++]=0x80|((value>>12)&63);out[at++]=0x80|((value>>6)&63);out[at++]=0x80|(value&63);}
	out[at]=0;return at;
}

static void import_inbox(const char *source){
	DIR *dir=opendir(source);if(!dir){return;}struct dirent *entry;
	while((entry=readdir(dir))){
		if(entry->d_name[0]=='.'){continue;}
		char path[768];snprintf(path,sizeof(path),"%s/%s",source,entry->d_name);
		int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){continue;}
		unsigned char raw[2048];ssize_t got=read(fd,raw,sizeof(raw));close(fd);
		if(got<4 || raw[1]!=0x43){continue;}
		unsigned size=got,off=4,n=0;const unsigned char *p=NULL;
		for(unsigned i=0;i<raw[3] && off+4<=size;i++){
			unsigned length=(unsigned)raw[off+2]<<8|raw[off+3];
			if(length<4 || length>size-off){break;}
			if(raw[off]==0 && raw[off+1]==0x1c && length>=6 && raw[off+4]<=length-6){p=raw+off+6;n=raw[off+4];}
			off+=length;
		}
		if(!p || n<3 || (p[0]&3)!=0 || p[1]>15 || !p[1] || p[2]!=0x91){continue;}
		struct message m={0};m.number[0]='+';bool valid=true;
		unsigned address=(p[1]+1)/2;
		if(3+address+10>n){continue;}
		for(unsigned i=0;i<p[1];i++){
			unsigned digit=(p[3+i/2]>>(4*(i%2)))&15;
			if(digit>9){valid=false;break;}m.number[i+1]='0'+digit;
		}
		if(!valid){continue;}
		unsigned pos=3+address,dcs=p[pos+1];pos+=9;unsigned length=p[pos++],header=0,out=0;
		if((p[0]&0x40) && pos<n){header=p[pos]+1;}
		if(header>n-pos){continue;}
		if((dcs&12)==8){
			if(length>n-pos || length<header || (length-header)%2){continue;}
			for(unsigned j=header;j<length;j+=2){
				unsigned cp=(unsigned)p[pos+j]<<8|p[pos+j+1];
				if(cp>=0xd800 && cp<=0xdbff && j+3<length){unsigned low=(unsigned)p[pos+j+2]<<8|p[pos+j+3];if(low>=0xdc00 && low<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;j+=2;}}
				if(cp>=0xd800 && cp<=0xdfff){cp=0xfffd;}out=put_utf8(m.body,out,cp);
			}
		}
		else if((dcs&12)==0){
			static const unsigned short first[32]={64,163,36,165,232,233,249,236,242,199,10,216,248,13,197,229,916,95,934,915,923,937,928,936,931,920,926,32,198,230,223,201};
			unsigned start=(header*8+6)/7;
			if((length*7+7)/8>n-pos || length<start){continue;}
			for(unsigned j=start;j<length;j++){
				unsigned bit=j*7,index=bit/8,value=p[pos+index]>>(bit%8);
				if(bit%8>1 && pos+index+1<n){value|=(unsigned)p[pos+index+1]<<(8-bit%8);}value&=127;
				unsigned cp=value<32 ? first[value] : value;
				if(value==0x5b){cp=196;}else if(value==0x5c){cp=214;}else if(value==0x5d){cp=209;}else if(value==0x5e){cp=220;}
				else if(value==0x7b){cp=228;}else if(value==0x7c){cp=246;}else if(value==0x7d){cp=241;}else if(value==0x7e){cp=252;}else if(value==0x7f){cp=224;}
				out=put_utf8(m.body,out,cp);
			}
		}
		else{continue;}
		unsigned long long hash=1469598103934665603ULL;
		for(unsigned j=0;j<size;j++){hash=(hash^raw[j])*1099511628211ULL;}
		snprintf(m.id,sizeof(m.id),"%016llx",hash);m.status=MESSAGE_SENT;
		char stored[512],file[640];if(!directory(stored)){continue;}
		snprintf(file,sizeof(file),"%s/%s.msg",stored,m.id);
		if(!access(file,F_OK) || message_save(&m)){unlink(path);}
	}
	closedir(dir);
}

int messages_load(struct message *list,int capacity){
	char path[512];if(!directory(path)){return 0;}
	char incoming[544];snprintf(incoming,sizeof(incoming),"%s-incoming",path);import_inbox(incoming);import_inbox("/tmp/modem-sms-inbox");
	DIR *dir=opendir(path);if(!dir){return 0;}struct dirent *entry;int count=0;
	while(count<capacity && (entry=readdir(dir))){
		if(!strstr(entry->d_name,".msg") || entry->d_name[0]=='.'){continue;}
		int fd=openat(dirfd(dir),entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){continue;}
		struct message m;ssize_t n=read(fd,&m,sizeof(m));close(fd);
		if(n!=(ssize_t)sizeof(m) || m.magic!=0x594d5331 || !memchr(m.number,0,sizeof(m.number)) || !memchr(m.body,0,sizeof(m.body)) || !memchr(m.id,0,sizeof(m.id)) || !modem_number(m.number)){continue;}
		list[count++]=m;
	}
	closedir(dir);qsort(list,count,sizeof(*list),by_time);return count;
}
