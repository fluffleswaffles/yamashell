// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "yamashell.h"
#include "level.h"

pid_t sound_play(bool ring){
	if(level_get(LEVEL_VOLUME)==0){return 0;}
	const char *player=NULL;
	const char *players[]={"/usr/bin/modem-play","/usr/bin/pw-play","/usr/bin/paplay","/usr/bin/ogg123"};
	for(unsigned i=0;i<sizeof(players)/sizeof(*players);i++){if(!access(players[i],X_OK)){player=players[i];break;}}
	bool tone=!player && !access("/usr/bin/modem-tone",X_OK);
	if(!player && !tone){return 0;}
	pid_t pid=fork();
	if(!pid){
		setsid();sigset_t mask;sigemptyset(&mask);sigprocmask(SIG_SETMASK,&mask,NULL);
		if(tone){execl("/usr/bin/modem-tone","modem-tone",ring ? "ring" : "notification",(char *)NULL);_exit(127);}
		const char *file=ring ? "/usr/share/yamashell/sounds/ringtone-alarm-synth.ogg" : "/usr/share/yamashell/sounds/notif-ponderous.ogg";
		execl(player,player,file,(char *)NULL);_exit(127);
	}
	return pid>0 ? pid : 0;
}

void sound_stop(pid_t pid){if(pid>1){kill(-pid,SIGTERM);}}