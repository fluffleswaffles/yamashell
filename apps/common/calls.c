// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "calls.h"

static bool directory(char path[640]){
	const char *data=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
	if(data && *data){snprintf(path,640,"%s/yamashell/calls.d",data);}
	else{snprintf(path,640,"%s/.local/share/yamashell/calls.d",home ? home : "/root");}
	for(char *p=path+1;*p;p++){if(*p=='/'){*p=0;mkdir(path,0700);*p='/';}}
	mkdir(path,0700);struct stat st;
	return !lstat(path,&st) && S_ISDIR(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&077);
}

bool call_save(struct call_record *call){
	char dir[640],path[768],tmp[800];if(!directory(dir)){return false;}
	if(!*call->id){struct timespec now;clock_gettime(CLOCK_REALTIME,&now);snprintf(call->id,sizeof(call->id),"%016llx-%08x-%08x",(unsigned long long)now.tv_sec,(unsigned)now.tv_nsec,(unsigned)getpid());}
	for(const char *p=call->id;*p;p++){if((*p<'0' || *p>'9') && (*p<'a' || *p>'f') && *p!='-'){return false;}}
	call->magic=0x59434c31;
	snprintf(path,sizeof(path),"%s/%s.call",dir,call->id);snprintf(tmp,sizeof(tmp),"%s.%u",path,(unsigned)getpid());
	int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){return false;}
	bool ok=write(fd,call,sizeof(*call))==(ssize_t)sizeof(*call);if(ok){ok=!fsync(fd);}close(fd);
	if(ok){ok=!rename(tmp,path);}if(!ok){unlink(tmp);}return ok;
}

static int compare(const void *a,const void *b){const struct call_record *x=a,*y=b;return x->started==y->started ? strcmp(y->id,x->id) : x->started>y->started ? -1 : 1;}

int calls_load(struct call_record *calls,int capacity){
	char path[640];if(!directory(path)){return 0;}DIR *dir=opendir(path);if(!dir){return 0;}
	struct dirent *entry;int n=0;
	while((entry=readdir(dir))){
		if(entry->d_name[0]=='.' || !strstr(entry->d_name,".call")){continue;}
		int fd=openat(dirfd(dir),entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){continue;}
		struct call_record call;ssize_t got=read(fd,&call,sizeof(call));close(fd);
		if(got!=(ssize_t)sizeof(call) || call.magic!=0x59434c31 || call.incoming>1 || call.result>CALL_INTERRUPTED || !memchr(call.id,0,sizeof(call.id)) || !memchr(call.number,0,sizeof(call.number))){continue;}
		if(n<capacity){calls[n++]=call;}
		else if(capacity && compare(&call,&calls[n-1])<0){calls[n-1]=call;}
		qsort(calls,n,sizeof(*calls),compare);
	}
	closedir(dir);return n;
}

const char *call_result(unsigned result){
	static const char *names[]={"Incoming","Outgoing","In progress","Completed","Missed","Declined","Not connected","Interrupted"};
	return result<sizeof(names)/sizeof(*names) ? names[result] : "Unknown";
}

bool calls_decode(FILE *f,struct call_state *call){
	char line[4096];*call=(struct call_state){0};
	while(fgets(line,sizeof(line),f)){
		char *p=strstr(line,"res c9:");if(!p){continue;}p+=7;
		unsigned char bytes[256];unsigned n=0;
		while(n<sizeof(bytes)){
			while(*p==' '){p++;}if(!*p || *p=='\n'){break;}
			char *end;unsigned long value=strtoul(p,&end,16);if(end-p!=2 || value>255){break;}bytes[n++]=value;p=end;
		}
		if(n<4 || bytes[1]!=0x0e){continue;}
		unsigned off=4;bool valid=false;
		for(unsigned i=0;i<bytes[3];i++){
			if(off+2>n){return false;}unsigned len=bytes[off+1];if(len<2 || len>n-off){return false;}
			const unsigned char *s=bytes+off;
			if(s[0]==8 && len==4 && s[2]==2 && s[3]==1){valid=true;}
			if((s[0]==0x0b && len>=8) || (s[0]==0x1d && len>=12)){
				unsigned id=s[2]&7;if(!id){return false;}
				struct call_state candidate={.id=id,.state=s[5],.incoming=(s[4]&1)!=0 || s[5]==2 || s[5]==5};valid=true;
				if(s[0]==0x1d){
					unsigned digits=s[11],at=0;if(digits>20 || 12+digits*2>len){return false;}
					if((s[8]&0x70)==0x10){candidate.number[at++]='+';}
					for(unsigned j=0;j<digits;j++){unsigned c=(unsigned)s[12+j*2]<<8|s[13+j*2];if((c<'0' || c>'9') && !(c=='+' && !at)){candidate.number[0]=0;break;}candidate.number[at++]=c;candidate.number[at]=0;}
				}
				if(!call->id || call->id==id || (candidate.incoming && candidate.state==5)){*call=candidate;}
			}
			off+=len;
		}
		return valid && off==n;
	}
	return false;
}
