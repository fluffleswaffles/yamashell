// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "apps/common/calls.h"

struct session{struct call_record call;unsigned id;};

int main(int argc,char **argv){
	umask(077);struct call_state state={0};bool offline=argc==2 && !strcmp(argv[1],"--offline"),decline=argc==2 && !strcmp(argv[1],"--declined");
	if(!offline && !decline && !calls_decode(stdin,&state)){return 1;}
	if(argc==2 && !strcmp(argv[1],"--decode")){printf("%u %u %u %s\n",state.id,state.state,state.incoming,*state.number ? state.number : "-");return 0;}
	struct session session={0};
	int fd=open("/run/rapuyama-modem/session",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
	if(fd>=0){if(read(fd,&session,sizeof(session))!=(ssize_t)sizeof(session) || session.call.magic!=0x59434c31 || !memchr(session.call.id,0,sizeof(session.call.id))){memset(&session,0,sizeof(session));}close(fd);}
	struct session original=session;
	if(session.id==state.id && state.state>=6){state.incoming=session.call.incoming;}
	if(decline){if(session.id){session.call.result=CALL_DECLINED;}}else{
		if(session.id && (session.id!=state.id || session.call.incoming!=state.incoming || offline)){
			session.call.ended=time(NULL);
			session.call.result=offline ? CALL_INTERRUPTED : session.call.answered ? CALL_COMPLETED : session.call.result==CALL_DECLINED ? CALL_DECLINED : session.call.incoming ? CALL_MISSED : CALL_NOT_CONNECTED;
			if(!call_save(&session.call)){return 1;}memset(&session,0,sizeof(session));
		}
		if(state.id){
			if(!session.id){session.id=state.id;session.call.started=time(NULL);session.call.incoming=state.incoming;session.call.result=state.incoming ? CALL_RINGING : CALL_DIALING;}
			if(*state.number){snprintf(session.call.number,sizeof(session.call.number),"%s",state.number);}
			if(state.state==8 && !session.call.answered){session.call.answered=time(NULL);session.call.result=CALL_CONNECTED;}
		}
	}
	if(memcmp(&original,&session,sizeof(session))){
		if(session.id && !call_save(&session.call)){return 1;}
		fd=open("/run/rapuyama-modem/session.new",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){return 1;}
		bool ok=write(fd,&session,sizeof(session))==(ssize_t)sizeof(session);close(fd);
		if(!ok || rename("/run/rapuyama-modem/session.new","/run/rapuyama-modem/session")){return 1;}
	}
	if(!offline && !decline){struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);printf("%llu %02x %u %u %s\n",(unsigned long long)now.tv_sec,state.id,state.state,state.incoming,*state.number ? state.number : session.id && *session.call.number ? session.call.number : "-");}
	return 0;
}