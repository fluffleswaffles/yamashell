// SPDX-License-Identifier: GPL-2.0-only
/* system state for the status bar and the shade: battery, charger, usb shell, memory card */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <linux/netlink.h>
#include "ui.h"

/* system state: battery, charger, usb shell, card */
struct status read_status(void){
	struct status st={-1,false,false,false,false};
	char buf[32]={0},path[300];
	FILE *f=fopen("/sys/class/power_supply/battery/capacity","r");
	if(f){if(fscanf(f,"%d",&st.pct)!=1){st.pct=-1;}fclose(f);}
	f=fopen("/sys/class/power_supply/battery/status","r");
	if(f){if(!fgets(buf,sizeof(buf),f)){buf[0]=0;}fclose(f);}
	st.charging=!strncmp(buf,"Charging",8);
	int on=0;f=fopen("/sys/class/power_supply/usb/online","r");
	if(f){if(fscanf(f,"%d",&on)!=1){on=0;}fclose(f);}
	st.charger=on;
	/* the usb shell is up once the host configured the gadget, power alone may be a wall charger.
	 * the udc may not notice the unplug, the charger always does */
	DIR *d=opendir("/sys/class/udc");struct dirent *e;
	while(d && (e=readdir(d))){
		if(e->d_name[0]=='.'){continue;}
		snprintf(path,sizeof(path),"/sys/class/udc/%s/state",e->d_name);
		f=fopen(path,"r");
		if(f){if(fgets(buf,sizeof(buf),f) && !strncmp(buf,"configured",10)){st.usb=st.charger;}fclose(f);}
	}
	if(d){closedir(d);}
	st.card=access("/dev/mmcblk0",F_OK)==0;
	return st;
}

static void refresh_status(struct server *s){
	struct ui *u=s->ui;struct status st=read_status();
	if(memcmp(&st,&u->st,sizeof(st))){u->st=st;u->dirty=true;schedule_repaint(s,true);}
}

/* a fallback in case an event got lost */
static int status_poll(void *data){
	struct server *s=data;
	refresh_status(s);
	wl_event_source_timer_update(s->ui->poll,10000);
	return 0;
}

/* kernel uevents: charger plugged, battery changed, card in/out */
static int uevent(int fd,uint32_t mask,void *data){
	struct server *s=data;char buf[2048];ssize_t n;bool hit=false;
	(void)mask;
	while((n=recv(fd,buf,sizeof(buf)-1,MSG_DONTWAIT))>0){
		buf[n]=0;
		for(char *p=buf;p<buf+n;p+=strlen(p)+1){hit|=!strcmp(p,"SUBSYSTEM=power_supply") || !strcmp(p,"SUBSYSTEM=block");}
	}
	if(hit){refresh_status(s);}
	return 0;
}

static int uevent_open(void){
	struct sockaddr_nl a={.nl_family=AF_NETLINK,.nl_groups=1};
	int fd=socket(AF_NETLINK,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,NETLINK_KOBJECT_UEVENT);
	if(fd<0){return -1;}
	if(bind(fd,(struct sockaddr *)&a,sizeof(a))){close(fd);return -1;}
	return fd;
}

/* usb gadget state (attached, configured etc) has no uevent, only sysfs_notify: EPOLLPRI on the attribute */
static int udc_open(void){
	int ep=epoll_create1(EPOLL_CLOEXEC),n=0;
	DIR *d=opendir("/sys/class/udc");struct dirent *e;char path[300],buf[32];
	if(ep<0){if(d){closedir(d);}return -1;}
	while(d && (e=readdir(d))){
		if(e->d_name[0]=='.'){continue;}
		snprintf(path,sizeof(path),"/sys/class/udc/%s/state",e->d_name);
		int fd=open(path,O_RDONLY|O_CLOEXEC);
		if(fd<0){continue;}
		/* a read arms the notification */
		if(pread(fd,buf,sizeof(buf),0)<0){close(fd);continue;}
		struct epoll_event ev={.events=EPOLLPRI|EPOLLERR,.data.fd=fd};
		if(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&ev)){close(fd);continue;}
		n++;
	}
	if(d){closedir(d);}
	if(!n){close(ep);return -1;}
	return ep;
}

static int udc_event(int ep,uint32_t mask,void *data){
	struct epoll_event ev[4];char buf[32];int n;
	(void)mask;
	while((n=epoll_wait(ep,ev,4,0))>0){for(int i=0;i<n;i++){if(pread(ev[i].data.fd,buf,sizeof(buf),0)<0){epoll_ctl(ep,EPOLL_CTL_DEL,ev[i].data.fd,NULL);}}}
	refresh_status(data);
	return 0;
}

void status_bar(struct ui *u,const char *hm){
	struct layer *bar=&u->layers[L_STATUS];struct status st=u->st;char buf[16];
	rect(bar,0,0,screen_w,STATUS,0,(struct col){0,0,0,.7f},(struct col){0,0,0,.7f});
	if(st.usb){icon(u,bar,20,STATUS/2.f,I_USB,20,white(.9f));}
	const struct note *waiting;int nwaiting=notify_list(&waiting),shown[4],nshown=0;
	for(int i=0;i<nwaiting && nshown<4;i++){
		int k=note_slot(u,&waiting[i]);bool dup=false;
		for(int j=0;j<nshown;j++){dup|=shown[j]==k;}
		if(dup){continue;}
		note_icon(u,bar,(st.usb ? 48 : 20)+nshown*28,STATUS/2.f,k,I_NOTE,20,1);
		shown[nshown++]=k;
	}
	float rx=screen_w-12-text(u,bar,F_TEXT,screen_w-12,21,hm,17,white(1),2)-12;
	if(st.pct>=0){
		snprintf(buf,sizeof(buf),"%d%%",st.pct);
		rx-=text(u,bar,F_TEXT,rx,21,buf,16,white(.9f),2)+14;
		icon(u,bar,rx+4,STATUS/2.f,st.charging ? I_CHARGING : I_BATTERY,21,white(.9f));
	}
}

void status_init(struct server *s){
	struct ui *u=s->ui;
	u->st=read_status();
	u->poll=wl_event_loop_add_timer(s->loop,status_poll,s);wl_event_source_timer_update(u->poll,10000);
	u->uevent_fd=uevent_open();
	if(u->uevent_fd>=0){u->uevent=wl_event_loop_add_fd(s->loop,u->uevent_fd,WL_EVENT_READABLE,uevent,s);}
	u->udc_fd=udc_open();
	if(u->udc_fd>=0){u->udc=wl_event_loop_add_fd(s->loop,u->udc_fd,WL_EVENT_READABLE,udc_event,s);}
}

void status_finish(struct ui *u){
	wl_event_source_remove(u->poll);
	if(u->uevent){wl_event_source_remove(u->uevent);}
	if(u->uevent_fd>=0){close(u->uevent_fd);}
	if(u->udc){wl_event_source_remove(u->udc);}
	if(u->udc_fd>=0){close(u->udc_fd);}
}