// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>
#include "modem.h"

bool modem_number(const char *s){
	unsigned n=0;
	if(!s || *s++!='+'){return false;}
	while(*s){
		if(*s<'0' || *s>'9' || ++n>15){return false;}
		s++;
	}
	return n>=3;
}

void modem_backspace(char *s){
	size_t n=strlen(s);
	if(!n){return;}
	do{n--;}while(n && ((unsigned char)s[n]&0xc0)==0x80);
	s[n]=0;
}

bool modem_append(char *s,unsigned capacity,const char *utf8,bool number){
	size_t n=strlen(s),extra=strlen(utf8);
	if(!extra || n+extra>=capacity){return false;}
	for(size_t i=0;i<extra;i++){
		if(number){
			if((utf8[i]<'0' || utf8[i]>'9') && !(utf8[i]=='+' && n+i==0)){return false;}
		}
		else if((unsigned char)utf8[i]<32 && utf8[i]!='\n'){return false;}
	}
	memcpy(s+n,utf8,extra+1);return true;
}

static void status(struct modem_job *j,const char *s){snprintf(j->status,sizeof(j->status),"%s",s);}

bool modem_job_start(struct modem_job *j,enum modem_action action,const char *number,const char *body){
	if(j->running){return false;}
	if((action==MODEM_CALL || action==MODEM_SMS) && !modem_number(number)){status(j,"Enter an international number starting with +");return false;}
	if(action==MODEM_SMS && (!body || !*body)){status(j,"Enter a message first");return false;}
	const char *name=action==MODEM_CALL ? "modem-call" : action==MODEM_SMS ? "modem-sms" : "modem-status";
	char path[512];const char *directory="/usr/bin";
#ifdef YAMASHELL_HOST
	const char *mock=getenv("YAMASHELL_MODEM_HELPERS");if(mock && *mock){directory=mock;}
#endif
	if(snprintf(path,sizeof(path),"%s/%s",directory,name)>=(int)sizeof(path)){return false;}
	int pipefd[2];
	if(pipe2(pipefd,O_CLOEXEC)){status(j,"Could not create the command pipe");return false;}
	if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)<0){
		close(pipefd[0]);close(pipefd[1]);status(j,"Could not configure the command pipe");return false;
	}
	pid_t parent=getpid(),pid=fork();
	if(pid<0){close(pipefd[0]);close(pipefd[1]);status(j,"Could not start the command");return false;}
	if(!pid){
		if(setsid()<0){_exit(126);}
		signal(SIGPIPE,SIG_IGN);signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);
		if(action==MODEM_CALL && (prctl(PR_SET_PDEATHSIG,SIGTERM)<0 || getppid()!=parent)){_exit(126);}
		close(pipefd[0]);
		if(dup2(pipefd[1],1)<0 || dup2(pipefd[1],2)<0){_exit(126);}
		if(pipefd[1]>2){close(pipefd[1]);}
		fcntl(1,F_SETFD,0);fcntl(2,F_SETFD,0);
		char *const call[]={path,(char *)number,NULL};
		char *const sms[]={path,(char *)number,(char *)body,NULL};
		char *const query[]={path,NULL};
		execv(path,action==MODEM_CALL ? call : action==MODEM_SMS ? sms : query);
		_exit(127);
	}
	close(pipefd[1]);
	*j=(struct modem_job){.pid=pid,.fd=pipefd[0],.action=action,.running=true};
	status(j,action==MODEM_CALL ? "Calling..." : action==MODEM_SMS ? "Submitting message..." : "Checking the modem...");
	return true;
}

static void line(struct modem_job *j){
	j->line[j->used]=0;
	if(strstr(j->line,"DELIVERED: matching recipient and message reference")){j->delivered=true;status(j,"Delivered: confirmed by the network");}
	else if(strstr(j->line,"SMSC accepted")){j->accepted=true;status(j,"Accepted by SMSC; waiting for delivery");}
	else if(strstr(j->line,"speech output policy accepted")){status(j,"Speech audio configured");}
	else if(j->action==MODEM_STATUS && strstr(j->line,"42 0e f0 01 08 04 02 01")){status(j,"Modem ready; no calls listed");}
	j->used=0;
}

bool modem_job_poll(struct modem_job *j){
	if(!j->running){return false;}
	char buffer[1024];ssize_t n;
	/* Bounded work even if a misbehaving helper floods its pipe. */
	for(unsigned batch=0;batch<8;batch++){
		n=read(j->fd,buffer,sizeof(buffer));if(n<=0){break;}
		for(ssize_t i=0;i<n;i++){
			if(buffer[i]=='\n'){line(j);}
			else if(j->used<sizeof(j->line)-1){j->line[j->used++]=buffer[i];}
		}
	}
	int result;pid_t done=waitpid(j->pid,&result,WNOHANG);
	if(done!=j->pid){return true;}
	if(j->used){line(j);}close(j->fd);j->fd=-1;j->running=false;j->pid=0;
	j->result=WIFEXITED(result) ? WEXITSTATUS(result) : 128+WTERMSIG(result);
	if(j->action==MODEM_SMS){
		if(!j->delivered){status(j,j->accepted ? "Delivery unconfirmed; SMSC accepted the message" : "Send unconfirmed; no automatic retry");}
	}
	else if(j->result){status(j,"Modem unavailable or call failed");}
	else if(j->action==MODEM_CALL){status(j,"Call ended");}
	return false;
}

void modem_job_stop(struct modem_job *j){
	if(!j->running || j->pid<=1 || j->action!=MODEM_CALL){return;}
	/* i warned yall about bad code */
	if(kill(-j->pid,SIGINT)<0 && errno==ESRCH){kill(j->pid,SIGTERM);}
	status(j,"Ending the call...");
}

void modem_job_close(struct modem_job *j){
	modem_job_stop(j);
	if(j->running && j->fd>=0){close(j->fd);j->fd=-1;}
}
