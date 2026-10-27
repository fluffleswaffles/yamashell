// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "yamashell.h"

struct device{
	struct server *server;
	struct wl_list link;
	struct wl_event_source *source;
	int fd,slot;
	struct input_absinfo abs_x,abs_y;
	struct{int id,x,y;bool down,changed,was_down;} slots[16];
};

static void destroy(struct wl_client *c,struct wl_resource *r){(void)c;wl_resource_destroy(r);}
static void resource_destroy(struct wl_resource *r){struct input_resource *i=wl_resource_get_user_data(r);wl_list_remove(&i->link);free(i);}
static struct wl_resource *new_resource(struct wl_client *c,const struct wl_interface *interface,int version,uint32_t id,struct wl_list *list,const void *impl,struct server *s){
	struct input_resource *i=calloc(1,sizeof(*i));
	if(!i){wl_client_post_no_memory(c);return NULL;}
	i->server=s;
	i->resource=wl_resource_create(c,interface,version,id);
	if(!i->resource){free(i);wl_client_post_no_memory(c);return NULL;}
	wl_list_insert(list,&i->link);wl_resource_set_implementation(i->resource,impl,i,resource_destroy);return i->resource;
}

static bool same_client(struct wl_resource *r,struct surface *v){return v && wl_resource_get_client(r)==wl_resource_get_client(v->resource);}
static void modifiers(struct server *s,struct wl_resource *r,uint32_t serial){
	wl_keyboard_send_modifiers(r,serial,xkb_state_serialize_mods(s->keys,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(s->keys,XKB_STATE_MODS_LATCHED),xkb_state_serialize_mods(s->keys,XKB_STATE_MODS_LOCKED),xkb_state_serialize_layout(s->keys,XKB_STATE_LAYOUT_EFFECTIVE));
}

static void keyboard_enter(struct server *s,struct wl_resource *r,uint32_t serial){
	struct wl_array keys={s->npressed*sizeof(uint32_t),sizeof(s->pressed),s->pressed};
	wl_keyboard_send_enter(r,serial,s->focus->resource,&keys);modifiers(s,r,serial);
}

void input_focus(struct server *s,struct surface *v){
	struct input_resource *i;
	if(v==s->focus){return;}
	uint32_t serial=wl_display_next_serial(s->display);

	wl_list_for_each(i,&s->keyboards,link){if(same_client(i->resource,s->focus)){wl_keyboard_send_leave(i->resource,serial,s->focus->resource);}}
	s->focus=v;

	wl_list_for_each(i,&s->keyboards,link){if(same_client(i->resource,v)){keyboard_enter(s,i->resource,serial);}}
	data_focus(s);
}

void input_key(struct server *s,uint32_t time,uint32_t key,uint32_t state){
	if(key>KEY_MAX || state>1){return;}
	if(power_key(s,key,state)){return;}
	uint32_t n=0;
	while(n<s->npressed && s->pressed[n]!=key){n++;}
	if(state && n==s->npressed && n<256){s->pressed[s->npressed++]=key;}
	if(!state && n<s->npressed){s->pressed[n]=s->pressed[--s->npressed];}

	xkb_state_update_key(s->keys,key+8,state ? XKB_KEY_DOWN : XKB_KEY_UP);
	if(ui_key(s,key,state)){return;}
	if(key==KEY_TAB && !state && s->tab_grabbed){s->tab_grabbed=false;return;}
	if(state && key==KEY_TAB && xkb_state_mod_name_is_active(s->keys,XKB_MOD_NAME_ALT,XKB_STATE_MODS_EFFECTIVE)){
		s->tab_grabbed=true;
		struct surface *v;
		wl_list_for_each(v,&s->surfaces,link){if(v->mapped && !v->parent && v!=s->focus){surface_focus(v);break;}}
		return;
	}

	uint32_t serial=wl_display_next_serial(s->display);
	struct input_resource *i;
	wl_list_for_each(i,&s->keyboards,link){if(same_client(i->resource,s->focus)){
		wl_keyboard_send_key(i->resource,serial,time,key,state);modifiers(s,i->resource,serial);
	}}
}

void input_key_tap(struct server *s,uint32_t key){
	uint32_t time=milliseconds();struct input_resource *i;
	for(int state=1;state>=0;state--){
		uint32_t serial=wl_display_next_serial(s->display);
		wl_list_for_each(i,&s->keyboards,link){if(same_client(i->resource,s->focus)){wl_keyboard_send_key(i->resource,serial,time,key,state);}}
	}
}

static struct surface *hit(struct server *s,double x,double y){
	struct surface *v;
	wl_list_for_each_reverse(v,&s->surfaces,link){
		int64_t vx,vy;surface_position(v,&vx,&vy);
		if(!v->cursor && surface_visible(v) && x>=vx+v->content_x && y>=vy+v->content_y && x<vx+v->content_x+v->width/v->scale && y<vy+v->content_y+v->height/v->scale){return v;}
	}
	return NULL;
}

void input_pointer(struct server *s,uint32_t time,double x,double y){
	if(power_pointer(s)){return;}
	if(x<0){x=0;}if(y<0){y=0;}if(x>=screen_w){x=screen_w-1;}if(y>=screen_h){y=screen_h-1;}
	s->pointer_x=x;s->pointer_y=y;
	if(s->cursor){s->cursor->x=(int)x-s->hotspot_x;s->cursor->y=(int)y-s->hotspot_y;schedule_repaint(s,true);}

	struct surface *v=hit(s,x,y);
	struct input_resource *i;int64_t vx=0,vy=0;
	if(v){surface_position(v,&vx,&vy);}
	uint32_t serial=wl_display_next_serial(s->display);
	
	wl_list_for_each(i,&s->pointers,link){
		struct wl_resource *r=i->resource;
		if(s->pointer_focus!=v){
			if(same_client(r,s->pointer_focus)){wl_pointer_send_leave(r,serial,s->pointer_focus->resource);}
			if(same_client(r,v)){wl_pointer_send_enter(r,serial,v->resource,wl_fixed_from_double(x-vx),wl_fixed_from_double(y-vy));}
		}
		if(same_client(r,v)){wl_pointer_send_motion(r,time,wl_fixed_from_double(x-vx),wl_fixed_from_double(y-vy));}
		if(wl_resource_get_version(r)>=5){wl_pointer_send_frame(r);}
	}
	s->pointer_focus=v;
}

void input_button(struct server *s,uint32_t time,uint32_t button,uint32_t state){
	if(power_key(s,button,state)){return;}
	struct surface *v=s->pointer_focus;
	if(state && v){struct surface *root=v;while(root->parent){root=root->parent;}surface_focus(root);}

	struct input_resource *i;
	uint32_t serial=wl_display_next_serial(s->display);
	wl_list_for_each(i,&s->pointers,link){if(same_client(i->resource,v)){
		wl_pointer_send_button(i->resource,serial,time,button,state);
		if(wl_resource_get_version(i->resource)>=5){wl_pointer_send_frame(i->resource);}
	}}
}
void input_touch(struct server *s,uint32_t time,int id,double x,double y,int state){
	if(power_touch(s,id,state)){return;}
	if(ui_touch(s,id,x,y,state)){return;}
	if(state==1 && s->ntouches==0){
		struct surface *v=hit(s,x,y);if(!v){return;}
		s->touch_focus=v;struct surface *root=v;while(root->parent){root=root->parent;}surface_focus(root);
	}

	struct surface *v=s->touch_focus;
	if(!v){return;}
	if(state==1){s->ntouches++;}
	if(state==0 && s->ntouches>0){s->ntouches--;}
	int64_t vx,vy;surface_position(v,&vx,&vy);
	uint32_t serial=wl_display_next_serial(s->display);
	struct input_resource *i;

	wl_list_for_each(i,&s->touches,link){if(same_client(i->resource,v)){
		if(state==1){wl_touch_send_down(i->resource,serial,time,v->resource,id,wl_fixed_from_double(x-vx),wl_fixed_from_double(y-vy));}
		else if(state==0){wl_touch_send_up(i->resource,serial,time,id);}
		else{wl_touch_send_motion(i->resource,time,id,wl_fixed_from_double(x-vx),wl_fixed_from_double(y-vy));}
		wl_touch_send_frame(i->resource);
	}}
	if(!s->ntouches){s->touch_focus=NULL;}
}

static void cursor(struct wl_client *c,struct wl_resource *r,uint32_t serial,struct wl_resource *surface,int32_t x,int32_t y){
	(void)c;(void)serial;
	struct server *s=((struct input_resource *)wl_resource_get_user_data(r))->server;
	if(!same_client(r,s->pointer_focus)){return;}

	struct surface *v=surface ? wl_resource_get_user_data(surface) : NULL;
	if(v && (v->xdg || v->subsurface)){wl_resource_post_error(r,WL_POINTER_ERROR_ROLE,"surface already has a role");return;}
	if(s->cursor && s->cursor!=v){s->cursor->mapped=false;}
	s->cursor=v;s->hotspot_x=x;s->hotspot_y=y;

	if(v){v->cursor=true;v->x=(int)s->pointer_x-x;v->y=(int)s->pointer_y-y;}
	schedule_repaint(s,true);
}

static const struct wl_pointer_interface pointer_impl={.set_cursor=cursor,.release=destroy};
static const struct wl_keyboard_interface keyboard_impl={.release=destroy};
static const struct wl_touch_interface touch_impl={.release=destroy};
static void get_pointer(struct wl_client *c,struct wl_resource *r,uint32_t id){
	struct server *s=wl_resource_get_user_data(r);new_resource(c,&wl_pointer_interface,wl_resource_get_version(r),id,&s->pointers,&pointer_impl,s);
}

static void get_keyboard(struct wl_client *c,struct wl_resource *r,uint32_t id){
	struct server *s=wl_resource_get_user_data(r);
	struct wl_resource *k=new_resource(c,&wl_keyboard_interface,wl_resource_get_version(r),id,&s->keyboards,&keyboard_impl,s);
	if(!k){return;}
	wl_keyboard_send_keymap(k,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,s->keymap_fd,s->keymap_size);
	if(wl_resource_get_version(k)>=4){wl_keyboard_send_repeat_info(k,20,500);}
	if(same_client(k,s->focus)){keyboard_enter(s,k,wl_display_next_serial(s->display));}
}

static void get_touch(struct wl_client *c,struct wl_resource *r,uint32_t id){
	struct server *s=wl_resource_get_user_data(r);new_resource(c,&wl_touch_interface,wl_resource_get_version(r),id,&s->touches,&touch_impl,s);
}

static const struct wl_seat_interface seat_impl={.get_pointer=get_pointer,.get_keyboard=get_keyboard,.get_touch=get_touch,.release=destroy};
static void bind_seat(struct wl_client *c,void *data,uint32_t ver,uint32_t id){
	struct wl_resource *r=wl_resource_create(c,&wl_seat_interface,ver,id);wl_resource_set_implementation(r,&seat_impl,data,NULL);
	wl_seat_send_capabilities(r,WL_SEAT_CAPABILITY_KEYBOARD|WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_TOUCH);
	if(ver>=2){wl_seat_send_name(r,"e6");}
}

static double absolute(int value,struct input_absinfo *info,int size){
	return info->maximum>info->minimum ? (double)(value-info->minimum)*(size-1)/(info->maximum-info->minimum) : 0;
}

static int device_read(int fd,uint32_t mask,void *data){
	struct device *d=data;struct server *s=d->server;struct input_event events[32];ssize_t n;
	(void)mask;
	while((n=read(fd,events,sizeof(events)))>0){for(size_t j=0;j<(size_t)n/sizeof(*events);j++){
		struct input_event *e=&events[j];uint32_t time=(uint32_t)(e->input_event_sec*1000+e->input_event_usec/1000);
		if(e->type==EV_KEY && (e->code<BTN_MISC || (e->code>=KEY_OK && e->code<BTN_DPAD_UP))){if(e->value!=2){input_key(s,time,e->code,e->value);}}
		else if(e->type==EV_KEY && e->code>=BTN_MOUSE && e->code<=BTN_TASK){input_button(s,time,e->code,e->value);}
		else if(e->type==EV_REL){
			if(e->code==REL_X){input_pointer(s,time,s->pointer_x+e->value,s->pointer_y);}
			if(e->code==REL_Y){input_pointer(s,time,s->pointer_x,s->pointer_y+e->value);}
		}
		else if(e->type==EV_ABS){
			if(e->code==ABS_MT_SLOT){d->slot=e->value>=0 && e->value<16 ? e->value : 0;}
			else if(e->code==ABS_MT_TRACKING_ID){d->slots[d->slot].id=e->value;d->slots[d->slot].down=e->value>=0;d->slots[d->slot].changed=true;}
			else if(e->code==ABS_MT_POSITION_X){d->slots[d->slot].x=e->value;d->slots[d->slot].changed=true;}
			else if(e->code==ABS_MT_POSITION_Y){d->slots[d->slot].y=e->value;d->slots[d->slot].changed=true;}
		}
		else if(e->type==EV_SYN && e->code==SYN_REPORT){
			for(int k=0;k<16;k++){if(d->slots[k].changed){
				int state=d->slots[k].down ? (d->slots[k].was_down ? 2 : 1) : 0;
				input_touch(s,time,k,absolute(d->slots[k].x,&d->abs_x,screen_w),absolute(d->slots[k].y,&d->abs_y,screen_h),state);
				d->slots[k].was_down=d->slots[k].down;d->slots[k].changed=false;
			}}
		}
	}}
	if(n==0 || (n<0 && errno!=EAGAIN && errno!=EINTR)){
		wl_event_source_remove(d->source);wl_list_remove(&d->link);close(d->fd);free(d);
	}
	return 0;
}

void input_init(struct server *s){
	struct xkb_rule_names names={.layout="e6,e6",.variant=",ru",.options="grp:menu_toggle,lv3:ralt_switch"};
#ifdef YAMASHELL_HOST
	if(!getenv("XKB_CONFIG_EXTRA_PATH")){names.layout="us";names.variant=NULL;names.options=NULL;}
#endif
	s->xkb=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	s->keymap=xkb_keymap_new_from_names(s->xkb,&names,XKB_KEYMAP_COMPILE_NO_FLAGS);

	if(!s->keymap){names.layout="us";names.variant=NULL;names.options=NULL;s->keymap=xkb_keymap_new_from_names(s->xkb,&names,XKB_KEYMAP_COMPILE_NO_FLAGS);}
	if(!s->keymap){fprintf(stderr,"yamashell: cannot compile keymap\n");exit(1);}
	s->keys=xkb_state_new(s->keymap);

	char *keymap=xkb_keymap_get_as_string(s->keymap,XKB_KEYMAP_FORMAT_TEXT_V1);
	s->keymap_size=strlen(keymap)+1;s->keymap_fd=memfd_create("yamashell-keymap",MFD_CLOEXEC|MFD_ALLOW_SEALING);
	if(s->keymap_fd<0 || ftruncate(s->keymap_fd,s->keymap_size) || write(s->keymap_fd,keymap,s->keymap_size)!=(ssize_t)s->keymap_size){perror("keymap");exit(1);}
	fcntl(s->keymap_fd,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_SEAL);free(keymap);

	wl_global_create(s->display,&wl_seat_interface,5,s,bind_seat);
	if(s->input_fd>=0){
		struct device *d=calloc(1,sizeof(*d));if(!d){abort();}d->fd=s->input_fd;d->server=s;
		d->abs_x.maximum=screen_w-1;d->abs_y.maximum=screen_h-1;
		fcntl(d->fd,F_SETFL,fcntl(d->fd,F_GETFL)|O_NONBLOCK);
		d->source=wl_event_loop_add_fd(s->loop,d->fd,WL_EVENT_READABLE,device_read,d);wl_list_insert(&s->devices,&d->link);
	}

	if(s->headless){return;}
	glob_t paths={0};glob("/dev/input/event*",0,NULL,&paths);
	for(size_t i=0;i<paths.gl_pathc;i++){
		int fd=open(paths.gl_pathv[i],O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){continue;}
		struct device *d=calloc(1,sizeof(*d));if(!d){close(fd);continue;}
		d->fd=fd;d->server=s;int clock=CLOCK_MONOTONIC;ioctl(fd,EVIOCSCLOCKID,&clock);
		ioctl(fd,EVIOCGABS(ABS_MT_POSITION_X),&d->abs_x);ioctl(fd,EVIOCGABS(ABS_MT_POSITION_Y),&d->abs_y);
		d->source=wl_event_loop_add_fd(s->loop,fd,WL_EVENT_READABLE,device_read,d);
		wl_list_insert(&s->devices,&d->link);fprintf(stderr,"yamashell: input %s\n",paths.gl_pathv[i]);
	}
	globfree(&paths);
}

void input_finish(struct server *s){
	struct device *d,*tmp;
	wl_list_for_each_safe(d,tmp,&s->devices,link){wl_event_source_remove(d->source);close(d->fd);wl_list_remove(&d->link);free(d);}
	close(s->keymap_fd);xkb_state_unref(s->keys);xkb_keymap_unref(s->keymap);xkb_context_unref(s->xkb);
}