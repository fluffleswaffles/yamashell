// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "yamashell.h"

struct clipboard;
struct source{
	struct clipboard *clipboard;
	struct wl_resource *resource;
	struct wl_list mime;
};
struct mime{struct wl_list link;char *name;};
struct offer{struct wl_resource *source;struct wl_listener destroy;};
struct clipboard{struct server *server;struct source *selection;struct wl_list devices;};
struct data_device{struct clipboard *clipboard;struct wl_resource *resource;struct wl_list link;};
static void destroy(struct wl_client *c,struct wl_resource *r){(void)c;wl_resource_destroy(r);}

static void offer_destroy(struct wl_resource *r){
	struct offer *o=wl_resource_get_user_data(r);if(o->source){wl_list_remove(&o->destroy.link);}free(o);
}

static void source_gone(struct wl_listener *listener,void *data){
	struct offer *o=wl_container_of(listener,o,destroy);(void)data;o->source=NULL;wl_list_remove(&o->destroy.link);
}

static void accept(struct wl_client *c,struct wl_resource *r,uint32_t serial,const char *mime){(void)c;(void)r;(void)serial;(void)mime;}
static void receive(struct wl_client *c,struct wl_resource *r,const char *mime,int32_t fd){
	(void)c;struct offer *o=wl_resource_get_user_data(r);
	if(o->source){
		struct source *s=wl_resource_get_user_data(o->source);struct mime *m;
		wl_list_for_each(m,&s->mime,link){if(!strcmp(m->name,mime)){wl_data_source_send_send(o->source,mime,fd);break;}}
	}close(fd);
}
static void finish_offer(struct wl_client *c,struct wl_resource *r){(void)c;wl_resource_post_error(r,WL_DATA_OFFER_ERROR_INVALID_FINISH,"selection offers cannot finish a drag");}
static void offer_actions(struct wl_client *c,struct wl_resource *r,uint32_t actions,uint32_t preferred){
	(void)c;(void)actions;(void)preferred;wl_resource_post_error(r,WL_DATA_OFFER_ERROR_INVALID_OFFER,"selection offers have no drag actions");
}
static const struct wl_data_offer_interface offer_impl={.accept=accept,.receive=receive,.destroy=destroy,.finish=finish_offer,.set_actions=offer_actions};
static void send_selection(struct data_device *d){
	struct clipboard *cb=d->clipboard;
	if(!cb->server->focus || wl_resource_get_client(d->resource)!=wl_resource_get_client(cb->server->focus->resource)){return;}
	if(!cb->selection){wl_data_device_send_selection(d->resource,NULL);return;}
	struct offer *o=calloc(1,sizeof(*o));if(!o){wl_client_post_no_memory(wl_resource_get_client(d->resource));return;}
	struct wl_resource *r=wl_resource_create(wl_resource_get_client(d->resource),&wl_data_offer_interface,wl_resource_get_version(d->resource),0);
	if(!r){free(o);wl_client_post_no_memory(wl_resource_get_client(d->resource));return;}
	o->source=cb->selection->resource;o->destroy.notify=source_gone;wl_resource_add_destroy_listener(o->source,&o->destroy);
	wl_resource_set_implementation(r,&offer_impl,o,offer_destroy);wl_data_device_send_data_offer(d->resource,r);
	struct mime *m;wl_list_for_each(m,&cb->selection->mime,link){wl_data_offer_send_offer(r,m->name);}
	wl_data_device_send_selection(d->resource,r);
}
void data_focus(struct server *s){
	struct clipboard *cb=s->clipboard;struct data_device *d;
	if(!cb){return;}wl_list_for_each(d,&cb->devices,link){send_selection(d);}
}
static void source_destroy(struct wl_resource *r){
	struct source *s=wl_resource_get_user_data(r);struct mime *m,*tmp;
	if(s->clipboard->selection==s){s->clipboard->selection=NULL;data_focus(s->clipboard->server);}
	wl_list_for_each_safe(m,tmp,&s->mime,link){wl_list_remove(&m->link);free(m->name);free(m);}free(s);
}
static void source_offer(struct wl_client *c,struct wl_resource *r,const char *name){
	struct source *s=wl_resource_get_user_data(r);struct mime *m=calloc(1,sizeof(*m));
	if(!m){wl_client_post_no_memory(c);return;}m->name=strdup(name);
	if(!m->name){free(m);wl_client_post_no_memory(c);return;}wl_list_insert(s->mime.prev,&m->link);
}
static void source_actions(struct wl_client *c,struct wl_resource *r,uint32_t actions){
	(void)c;if(actions&~7u){wl_resource_post_error(r,WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,"invalid actions");}
}
static const struct wl_data_source_interface source_impl={.offer=source_offer,.destroy=destroy,.set_actions=source_actions};
static void create_source(struct wl_client *c,struct wl_resource *r,uint32_t id){
	struct source *s=calloc(1,sizeof(*s));if(!s){wl_client_post_no_memory(c);return;}
	s->clipboard=wl_resource_get_user_data(r);wl_list_init(&s->mime);
	s->resource=wl_resource_create(c,&wl_data_source_interface,wl_resource_get_version(r),id);wl_resource_set_implementation(s->resource,&source_impl,s,source_destroy);
}
static void drag(struct wl_client *c,struct wl_resource *r,struct wl_resource *source,struct wl_resource *origin,struct wl_resource *icon,uint32_t serial){
	(void)c;(void)r;(void)origin;(void)icon;(void)serial;if(source){wl_data_source_send_cancelled(source);}
}
static void selection(struct wl_client *c,struct wl_resource *r,struct wl_resource *source,uint32_t serial){
	(void)serial;struct data_device *d=wl_resource_get_user_data(r);struct clipboard *cb=d->clipboard;
	if(!cb->server->focus || wl_resource_get_client(cb->server->focus->resource)!=c){return;}
	struct source *s=source ? wl_resource_get_user_data(source) : NULL;
	if(cb->selection==s){return;}
	if(cb->selection){wl_data_source_send_cancelled(cb->selection->resource);}
	cb->selection=s;data_focus(cb->server);
}
static const struct wl_data_device_interface device_impl={.start_drag=drag,.set_selection=selection,.release=destroy};
static void device_destroy(struct wl_resource *r){struct data_device *d=wl_resource_get_user_data(r);wl_list_remove(&d->link);free(d);}
static void get_device(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *seat){
	(void)seat;struct clipboard *cb=wl_resource_get_user_data(r);struct data_device *d=calloc(1,sizeof(*d));
	if(!d){wl_client_post_no_memory(c);return;}d->clipboard=cb;d->resource=wl_resource_create(c,&wl_data_device_interface,wl_resource_get_version(r),id);
	wl_list_insert(&cb->devices,&d->link);wl_resource_set_implementation(d->resource,&device_impl,d,device_destroy);send_selection(d);
}
static const struct wl_data_device_manager_interface manager_impl={.create_data_source=create_source,.get_data_device=get_device};
static void bind_manager(struct wl_client *c,void *data,uint32_t version,uint32_t id){
	struct wl_resource *r=wl_resource_create(c,&wl_data_device_manager_interface,version,id);wl_resource_set_implementation(r,&manager_impl,data,NULL);
}
void data_init(struct server *s){
	struct clipboard *cb=calloc(1,sizeof(*cb));if(!cb){abort();}cb->server=s;wl_list_init(&cb->devices);s->clipboard=cb;
	wl_global_create(s->display,&wl_data_device_manager_interface,3,cb,bind_manager);
}
void data_finish(struct server *s){free(s->clipboard);s->clipboard=NULL;}