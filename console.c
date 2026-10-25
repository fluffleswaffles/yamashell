// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include "yamashell.h"

void console_restore(void){
	int fd=open("/dev/tty1",O_RDWR|O_CLOEXEC);
	if(fd>=0){ioctl(fd,KDSKBMODE,K_UNICODE);ioctl(fd,KDSETMODE,KD_TEXT);close(fd);}
}

void console_init(struct server *s){
	if(s->headless){return;}
	s->console_fd=open("/dev/tty1",O_RDWR|O_CLOEXEC);
	if(s->console_fd<0){perror("yamashell: tty1");return;}
	if(ioctl(s->console_fd,KDGKBMODE,&s->keyboard_mode) || ioctl(s->console_fd,KDGETMODE,&s->console_mode)){close(s->console_fd);s->console_fd=-1;return;}
	if(ioctl(s->console_fd,KDSKBMODE,K_OFF) || ioctl(s->console_fd,KDSETMODE,KD_GRAPHICS)){fprintf(stderr,"yamashell: cannot switch console to graphics mode\n");console_finish(s);}
}

void console_finish(struct server *s){
	if(s->console_fd<0){return;}
	ioctl(s->console_fd,KDSKBMODE,s->keyboard_mode);
	ioctl(s->console_fd,KDSETMODE,s->console_mode);
	close(s->console_fd);s->console_fd=-1;
}