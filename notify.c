// SPDX-License-Identifier: GPL-2.0-only
/* org.freedesktop.Notifications on the session bus, so notify-send, libnotify
 * apps and foot's notifications all land in the shell */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <systemd/sd-bus.h>
#include "yamashell.h"

#define PATH "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"
enum{CLOSED_EXPIRED=1,CLOSED_DISMISSED=2,CLOSED_CALL=3};

static struct server *server;
static sd_bus *bus;
static struct wl_event_source *source,*expire;
static struct note notes[MAX_NOTES];
static int nnotes;
static uint32_t last_id;

int notify_list(const struct note **out){*out=notes;return nnotes;}

static void flush(void){
	while(sd_bus_process(bus,NULL)>0){}
	sd_bus_flush(bus);
}

static void rearm(void){
	uint32_t now=milliseconds(),next=0;
	for(int i=0;i<nnotes;i++){
		if(notes[i].expire && (!next || (int32_t)(notes[i].expire-next)<0)){next=notes[i].expire;}
	}
	wl_event_source_timer_update(expire,next ? ((int32_t)(next-now)>1 ? (int)(next-now) : 1) : 0);
}

static void drop(int i,uint32_t reason){
	uint32_t id=notes[i].id;
	memmove(&notes[i],&notes[i+1],sizeof(*notes)*(size_t)(nnotes-i-1));nnotes--;
	if(bus){sd_bus_emit_signal(bus,PATH,IFACE,"NotificationClosed","uu",id,reason);}
}

static int find(uint32_t id){
	for(int i=0;i<nnotes;i++){if(notes[i].id==id){return i;}}
	return -1;
}

void notify_close(uint32_t id){
	int i=find(id);
	if(i<0){return;}
	drop(i,CLOSED_DISMISSED);rearm();flush();
	ui_notes_changed(server,0);
}

static int expired(void *data){
	uint32_t now=milliseconds();bool hit=false;
	(void)data;
	for(int i=nnotes-1;i>=0;i--){if(notes[i].expire && (int32_t)(notes[i].expire-now)<=0){drop(i,CLOSED_EXPIRED);hit=true;}}
	rearm();flush();
	if(hit){ui_notes_changed(server,0);}
	return 0;
}

/* one line, control characters as spaces */
static void copy(char *out,size_t n,const char *s){
	size_t i=0;
	for(;s && *s && *s!='\n' && i+1<n;s++){out[i++]=(unsigned char)*s<0x20 ? ' ' : *s;}
	out[i]=0;
}

static int method_notify(sd_bus_message *m,void *data,sd_bus_error *err){
	const char *app,*icon,*summary,*body,*image=NULL,*desktop=NULL;uint32_t replaces;int32_t timeout;uint8_t urgency=1;int r;
	(void)data;(void)err;
	if((r=sd_bus_message_read(m,"susss",&app,&replaces,&icon,&summary,&body))<0){return r;}
	if((r=sd_bus_message_skip(m,"as"))<0){return r;}
	/* hints used: urgency (critical stays until dismissed), image-path and desktop-entry for the icon.
	 * shortcut: raw image-data pixels are skipped, a sender with only those gets the app's or the default icon */
	if((r=sd_bus_message_enter_container(m,'a',"{sv}"))<0){return r;}
	while((r=sd_bus_message_enter_container(m,'e',"sv"))>0){
		const char *key,*type;
		if((r=sd_bus_message_read(m,"s",&key))<0){return r;}
		if(sd_bus_message_peek_type(m,NULL,&type)<=0){return -EINVAL;}
		if(!strcmp(key,"urgency") && !strcmp(type,"y")){r=sd_bus_message_read(m,"v","y",&urgency);}
		else if((!strcmp(key,"image-path") || !strcmp(key,"image_path")) && !strcmp(type,"s")){r=sd_bus_message_read(m,"v","s",&image);}
		else if(!strcmp(key,"desktop-entry") && !strcmp(type,"s")){r=sd_bus_message_read(m,"v","s",&desktop);}
		else{r=sd_bus_message_skip(m,"v");}
		if(r<0 || (r=sd_bus_message_exit_container(m))<0){return r;}
	}
	if(r<0 || (r=sd_bus_message_exit_container(m))<0 || (r=sd_bus_message_read(m,"i",&timeout))<0){return r;}

	int i=replaces ? find(replaces) : -1;
	struct note n={0};
	if(i>=0){n.id=replaces;}
	else{if(!++last_id){last_id=1;}n.id=last_id;}
	copy(n.app,sizeof(n.app),app);
	copy(n.icon,sizeof(n.icon),image && *image ? image : *icon ? icon : desktop ? desktop : "");
	copy(n.title,sizeof(n.title),summary);copy(n.body,sizeof(n.body),body);
	if(!n.title[0]){copy(n.title,sizeof(n.title),app[0] ? app : "Notification");}
	n.critical=urgency>=2;

	if(timeout>0 && !n.critical){n.expire=milliseconds()+(uint32_t)timeout;if(!n.expire){n.expire=1;}}
	/* newest first, a replaced note moves to the top too; a full list drops the oldest */
	if(i>=0){memmove(&notes[i],&notes[i+1],sizeof(*notes)*(size_t)(nnotes-i-1));nnotes--;}
	else if(nnotes==MAX_NOTES){drop(nnotes-1,CLOSED_EXPIRED);}
	memmove(&notes[1],&notes[0],sizeof(*notes)*(size_t)nnotes);notes[0]=n;nnotes++;
	rearm();
	ui_notes_changed(server,n.id);
	return sd_bus_reply_method_return(m,"u",n.id);
}

static int method_close(sd_bus_message *m,void *data,sd_bus_error *err){
	uint32_t id;int r,i;
	(void)data;(void)err;
	if((r=sd_bus_message_read(m,"u",&id))<0){return r;}
	if((i=find(id))>=0){drop(i,CLOSED_CALL);rearm();ui_notes_changed(server,0);}
	return sd_bus_reply_method_return(m,"");
}

static int method_caps(sd_bus_message *m,void *data,sd_bus_error *err){
	(void)data;(void)err;
	return sd_bus_reply_method_return(m,"as",1,"body");
}

static int method_info(sd_bus_message *m,void *data,sd_bus_error *err){
	(void)data;(void)err;
	return sd_bus_reply_method_return(m,"ssss","yamashell","rapuyama-linux","0.1","1.2");
}

static const sd_bus_vtable vtable[]={
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify","susssasa{sv}i","u",method_notify,SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification","u","",method_close,SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities","","as",method_caps,SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation","","ssss",method_info,SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed","uu",0),
	SD_BUS_VTABLE_END
};

static int readable(int fd,uint32_t mask,void *data){
	(void)fd;(void)data;
	if(mask&(WL_EVENT_HANGUP|WL_EVENT_ERROR)){
		fprintf(stderr,"yamashell: session bus gone, no more notifications\n");
		wl_event_source_remove(source);source=NULL;
		return 0;
	}
	flush();
	return 0;
}

void notify_init(struct server *s){
	int r;
	server=s;
	expire=wl_event_loop_add_timer(s->loop,expired,NULL);
	if((r=sd_bus_open_user(&bus))<0){fprintf(stderr,"yamashell: no session bus (%s), no notifications\n",strerror(-r));bus=NULL;return;}
	if((r=sd_bus_add_object_vtable(bus,NULL,PATH,IFACE,vtable,NULL))<0 || (r=sd_bus_request_name(bus,IFACE,0))<0){
		/* a pc desktop already has its own notification daemon */
		fprintf(stderr,"yamashell: can't serve %s (%s)\n",IFACE,strerror(-r));
		sd_bus_flush_close_unref(bus);bus=NULL;return;
	}
	source=wl_event_loop_add_fd(s->loop,sd_bus_get_fd(bus),WL_EVENT_READABLE,readable,NULL);
	flush();
}

void notify_finish(void){
	if(source){wl_event_source_remove(source);source=NULL;}
	if(expire){wl_event_source_remove(expire);expire=NULL;}
	if(bus){sd_bus_flush_close_unref(bus);bus=NULL;}
	nnotes=0;server=NULL;
}