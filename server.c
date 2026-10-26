// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include "yamashell.h"
#include "e6-vc-image-server.h"

struct region{struct rect opaque;};
struct callback{struct wl_resource *resource; struct wl_list link;};
int screen_w=640, screen_h=480;
uint32_t milliseconds(void){struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
static void destroy(struct wl_client *c, struct wl_resource *r){(void)c; wl_resource_destroy(r);}

static void callback_destroy(struct wl_resource *r){
	struct callback *cb=wl_resource_get_user_data(r);
	wl_list_remove(&cb->link); free(cb);
}

static void callbacks_finish(struct wl_list *list, bool done){
	struct callback *cb, *tmp;
	wl_list_for_each_safe(cb, tmp, list, link){
		if(done){wl_callback_send_done(cb->resource, milliseconds());}
		wl_resource_destroy(cb->resource);
	}
}

static void buffer_forget(struct surface_state *state);

static int repaint(void *data){
	struct server *s=data;
	struct surface *v;
	s->repaint_pending=false;
	s->repaint_started=milliseconds();
	/* textures already hold the commits */
	wl_list_for_each(v, &s->surfaces, link){if(surface_visible(v)){callbacks_finish(&v->callbacks, true);}}
	wl_display_flush_clients(s->display);
	if(s->dirty && !renderer_frame(s)){
		fprintf(stderr, "yamashell: frame failed (EGL %#x)\n", eglGetError());
		wl_display_terminate(s->display); return 0;
	}
	s->dirty=false;
	/* done*/
	if(s->retire){wl_list_for_each(v, &s->surfaces, link){buffer_forget(&v->retired);} s->retire=false;}
	if(ui_animating(s)){schedule_repaint(s,true);}
	return 0;
}

void schedule_repaint(struct server *s, bool dirty){
	s->dirty|=dirty;
	if(!s->repaint_pending){
		uint32_t elapsed=milliseconds()-s->repaint_started;
		uint32_t interval=s->repaint_interval ? s->repaint_interval : 16;
		int delay=s->frames && elapsed>=interval ? 1 : s->frames ? interval-elapsed : interval;
		s->repaint_pending=true;wl_event_source_timer_update(s->repaint,delay);
	}
}

bool surface_visible(struct surface *v){
	if(!v->role && !v->cursor){return false;}
	for(struct surface *p=v;p;p=p->parent){if(!p->mapped){return false;}}
	return true;
}

void surface_position(struct surface *v, int64_t *x, int64_t *y){
	*x=v->x; *y=v->y;
	for(struct surface *p=v->parent; p; p=p->parent){*x+=p->x; *y+=p->y;}
}

void surface_focus(struct surface *v){
	struct server *s=v->server;
	while(v->parent){v=v->parent;}
	if(!v->mapped || v->cursor){return;}
	struct wl_list group;wl_list_init(&group);
	struct surface *child,*tmp;
	wl_list_for_each_safe(child,tmp,&s->surfaces,link){
		struct surface *root=child;while(root->parent){root=root->parent;}
		if(root==v){wl_list_remove(&child->link);wl_list_insert(group.prev,&child->link);}
	}
	wl_list_insert_list(s->surfaces.prev,&group);
	input_focus(s,v);schedule_repaint(s,true);
}

void surface_focus_next(struct server *s){
	struct surface *v;
	wl_list_for_each_reverse(v,&s->surfaces,link){
		if(!v->parent && !v->cursor && surface_visible(v)){surface_focus(v);return;}
	}
}

static bool in_tree(struct surface *v,struct surface *root){
	for(struct surface *p=v;p;p=p->parent){if(p==root){return true;}}
	return false;
}

void surface_restack(struct surface *v,struct surface *sibling,bool above){
	struct server *s=v->server;
	struct wl_list group;wl_list_init(&group);
	struct surface *child,*tmp;
	wl_list_for_each_safe(child,tmp,&s->surfaces,link){
		if(in_tree(child,v)){wl_list_remove(&child->link);wl_list_insert(group.prev,&child->link);}
	}
	struct wl_list *anchor=&sibling->link;
	if(sibling!=v->parent){
		wl_list_for_each(child,&s->surfaces,link){
			if(!in_tree(child,sibling)){continue;}
			anchor=&child->link;if(!above){break;}
		}
	}
	wl_list_insert_list(above ? anchor : anchor->prev,&group);
}

struct buffer_use{struct wl_resource *resource;struct wl_list link;unsigned count;};
static struct buffer_use *buffer_use_get(struct server *s,struct wl_resource *resource){
	struct buffer_use *use;
	wl_list_for_each(use,&s->buffers,link){if(use->resource==resource){use->count++;return use;}}
	use=calloc(1,sizeof(*use));if(!use){return NULL;}
	use->resource=resource;use->count=1;wl_list_insert(&s->buffers,&use->link);return use;
}

static void buffer_use_drop(struct surface_state *state,bool release){
	struct buffer_use *use=state->use;state->use=NULL;
	if(use && !--use->count){
		if(release){wl_buffer_send_release(use->resource);}
		wl_list_remove(&use->link);free(use);
	}
}

static void buffer_gone(struct wl_listener *listener, void *data){
	struct surface_state *state=wl_container_of(listener, state, buffer_destroy);
	(void)data;buffer_use_drop(state,false);state->buffer=NULL; wl_list_remove(&state->buffer_destroy.link); wl_list_init(&state->buffer_destroy.link);
}

static void buffer_forget(struct surface_state *state){
	if(state->buffer){buffer_use_drop(state,true);wl_list_remove(&state->buffer_destroy.link); wl_list_init(&state->buffer_destroy.link); state->buffer=NULL;}
}

static void buffer_move(struct surface_state *to, struct surface_state *from){
	to->buffer=from->buffer;to->use=from->use;
	wl_list_remove(&from->buffer_destroy.link);wl_list_init(&from->buffer_destroy.link);
	if(to->buffer){to->buffer_destroy.notify=buffer_gone;wl_resource_add_destroy_listener(to->buffer,&to->buffer_destroy);}
	from->buffer=NULL;from->use=NULL;
}

static void vc_buffer_destroy(struct wl_client *c, struct wl_resource *r){(void)c;wl_resource_destroy(r);}
static const struct wl_buffer_interface vc_buffer_impl={vc_buffer_destroy};
static struct vc_buffer *vc_buffer_get(struct wl_resource *r){
	return wl_resource_instance_of(r,&wl_buffer_interface,&vc_buffer_impl) ? wl_resource_get_user_data(r) : NULL;
}
static void vc_buffer_free(struct wl_resource *r){
	struct vc_buffer *b=wl_resource_get_user_data(r);
	struct surface *v;

	wl_list_for_each(v,&b->server->surfaces,link){if(v->vc && v->texture==b->texture){v->texture=0;v->vc=false;v->mapped=false;schedule_repaint(b->server,true);}}
	renderer_vc_free(b);free(b);
}
static void vc_image_create(struct wl_client *c, struct wl_resource *r, uint32_t id, uint32_t id0, uint32_t id1, int32_t w, int32_t h, uint32_t format){
	if(w<1 || h<1 || w>2048 || h>2048){wl_resource_post_error(r,E6_VC_IMAGE_ERROR_INVALID_SIZE,"image size out of range");return;}
	struct vc_buffer *b=calloc(1,sizeof(*b));
	struct wl_resource *br=b ? wl_resource_create(c,&wl_buffer_interface,1,id) : NULL;
	if(!br){free(b);wl_client_post_no_memory(c);return;}
	*b=(struct vc_buffer){wl_resource_get_user_data(r),id0,id1,w,h,format,0};
	if(!renderer_vc_buffer(b)){free(b);wl_resource_destroy(br);wl_resource_post_error(r,E6_VC_IMAGE_ERROR_INVALID_SIZE,"vc image import failed");return;}
	wl_resource_set_implementation(br,&vc_buffer_impl,b,vc_buffer_free);
}
static void vc_image_destroy(struct wl_client *c, struct wl_resource *r){(void)c;wl_resource_destroy(r);}
static const struct e6_vc_image_interface vc_image_impl={vc_image_create,vc_image_destroy};
static void bind_vc_image(struct wl_client *c, void *data, uint32_t ver, uint32_t id){
	struct wl_resource *r=wl_resource_create(c,&e6_vc_image_interface,ver,id);
	if(!r){wl_client_post_no_memory(c);return;}
	wl_resource_set_implementation(r,&vc_image_impl,data,NULL);
}

static void state_init(struct surface_state *state, struct surface *v){
	*state=(struct surface_state){.surface=v};
	wl_list_init(&state->callbacks);wl_list_init(&state->damages);wl_list_init(&state->buffer_destroy.link);
}

static void damages_finish(struct surface_state *state){
	struct damage_rect *d,*tmp;
	wl_list_for_each_safe(d,tmp,&state->damages,link){wl_list_remove(&d->link);free(d);}
}

static bool state_commit(struct surface *v){
	struct surface_state *p=&v->pending, *cache=&v->cache;
	if(p->attached){
		struct buffer_use *use=p->buffer ? buffer_use_get(v->server,p->buffer) : NULL;
		if(p->buffer && !use){wl_client_post_no_memory(wl_resource_get_client(v->resource));return false;}
		buffer_forget(cache);cache->buffer=p->buffer;cache->use=use;cache->attached=true;
		buffer_forget(p);
		if(cache->buffer){cache->buffer_destroy.notify=buffer_gone; wl_resource_add_destroy_listener(cache->buffer, &cache->buffer_destroy);}
		cache->x+=p->x; cache->y+=p->y;
	}
	if(p->scale_set){cache->scale=p->scale; cache->scale_set=true;}
	if(p->opaque_set){cache->opaque_region=p->opaque_region;cache->opaque_set=true;}
	wl_list_insert_list(cache->damages.prev,&p->damages);wl_list_init(&p->damages);
	wl_list_insert_list(cache->callbacks.prev, &p->callbacks); wl_list_init(&p->callbacks);
	state_init(p,v);v->cached=true;return true;
}

bool surface_synced(struct surface *v){
	for(struct surface *p=v;p && p->subsurface;p=p->parent){if(p->sync){return true;}}
	return false;
}

static void surface_output(struct surface *v, bool enter){
	struct input_resource *o;
	wl_list_for_each(o,&v->server->outputs,link){
		if(wl_resource_get_client(o->resource)!=wl_resource_get_client(v->resource)){continue;}
		if(enter){wl_surface_send_enter(v->resource,o->resource);}else{wl_surface_send_leave(v->resource,o->resource);}
	}
}

static void surface_destroy(struct wl_resource *r){
	struct surface *v=wl_resource_get_user_data(r), *child;
	struct server *s=v->server;
	bool focused=s->focus==v;
	if(v->xdg){wl_resource_set_user_data(v->xdg, NULL);}
	if(v->role){wl_resource_set_user_data(v->role, NULL);}
	wl_list_for_each(child, &s->surfaces, link){if(child->parent==v){child->parent=NULL; child->mapped=false;} if(child->sibling==v){child->sibling=NULL; child->place_pending=false;}}
	if(s->focus==v){input_focus(s, NULL);}
	if(s->pointer_focus==v){s->pointer_focus=NULL;}
	if(s->touch_focus==v){s->touch_focus=NULL;}
	if(s->cursor==v){s->cursor=NULL;}
	buffer_forget(&v->pending); buffer_forget(&v->cache); buffer_forget(&v->shown); buffer_forget(&v->retired);
	damages_finish(&v->pending);damages_finish(&v->cache);
	callbacks_finish(&v->callbacks, false); callbacks_finish(&v->pending.callbacks, false); callbacks_finish(&v->cache.callbacks, false);
	renderer_drop(v); wl_list_remove(&v->link); free(v);
	if(focused){surface_focus_next(s);}
	schedule_repaint(s, true);
}

static void attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer, int32_t x, int32_t y){
	struct surface *v=wl_resource_get_user_data(r);
	(void)c;
	struct surface_state *p=&v->pending;
	buffer_forget(p); p->buffer=buffer; p->attached=true; p->x=x; p->y=y;
	if(buffer){p->buffer_destroy.notify=buffer_gone; wl_resource_add_destroy_listener(buffer, &p->buffer_destroy);}
}

static void add_damage(struct wl_client *c,struct surface *v,int32_t x,int32_t y,int32_t w,int32_t h,bool buffer){
	if(w<=0 || h<=0){return;}
	struct damage_rect *d=calloc(1,sizeof(*d));if(!d){wl_client_post_no_memory(c);return;}
	d->rect=(struct rect){x,y,w,h};d->buffer=buffer;wl_list_insert(v->pending.damages.prev,&d->link);
}

static void damage(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){add_damage(c,wl_resource_get_user_data(r),x,y,w,h,false);}
static void damage_buffer(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){add_damage(c,wl_resource_get_user_data(r),x,y,w,h,true);}
static void frame(struct wl_client *c, struct wl_resource *r, uint32_t id){
	struct surface *v=wl_resource_get_user_data(r);
	struct callback *cb=calloc(1, sizeof(*cb));
	if(!cb){wl_client_post_no_memory(c); return;}
	cb->resource=wl_resource_create(c, &wl_callback_interface, 1, id);
	if(!cb->resource){free(cb); wl_client_post_no_memory(c); return;}
	wl_list_insert(v->pending.callbacks.prev, &cb->link);
	wl_resource_set_implementation(cb->resource, NULL, cb, callback_destroy);
}

static void region_set(struct wl_client *c, struct wl_resource *r, struct wl_resource *region){(void)c; (void)r; (void)region;}
static void opaque_set(struct wl_client *c,struct wl_resource *r,struct wl_resource *region){
	(void)c;struct surface *v=wl_resource_get_user_data(r);
	struct region *hint=region ? wl_resource_get_user_data(region) : NULL;
	v->pending.opaque_region=hint ? hint->opaque : (struct rect){0};v->pending.opaque_set=true;
}

static struct rect clip(struct rect r, int scale, int width, int height){
	int64_t l=(int64_t)r.x*scale, t=(int64_t)r.y*scale;
	int64_t right=((int64_t)r.x+r.w)*scale, bottom=((int64_t)r.y+r.h)*scale;
	if(l<0){l=0;} if(t<0){t=0;} if(right>width){right=width;} if(bottom>height){bottom=height;}
	if(l>=right || t>=bottom){return (struct rect){0};}
	return (struct rect){l,t,right-l,bottom-t};
}

static void damage_merge(struct surface_state *state,struct surface *v,int w,int h){
	struct damage_rect *a,*b;
	wl_list_for_each(a,&state->damages,link){
		struct rect r=a->rect;
		if(!a->buffer){r.x-=v->content_x;r.y-=v->content_y;}
		a->rect=clip(r,a->buffer ? 1 : v->scale,w,h);a->buffer=true;
	}
	bool again;
	do{
		again=false;
		wl_list_for_each(a,&state->damages,link){
			if(!a->rect.w || !a->rect.h){continue;}
			wl_list_for_each(b,&state->damages,link){
				if(a==b || !b->rect.w || !b->rect.h){continue;}
				struct rect x=a->rect,y=b->rect;
				int64_t l=x.x<y.x ? x.x : y.x,t=x.y<y.y ? x.y : y.y;
				int64_t r=x.x+x.w>y.x+y.w ? x.x+x.w : y.x+y.w;
				int64_t bottom=x.y+x.h>y.y+y.h ? x.y+x.h : y.y+y.h;
				int64_t il=x.x>y.x ? x.x : y.x,it=x.y>y.y ? x.y : y.y;
				int64_t ir=x.x+x.w<y.x+y.w ? x.x+x.w : y.x+y.w;
				int64_t ib=x.y+x.h<y.y+y.h ? x.y+x.h : y.y+y.h;
				int64_t intersection=ir>il && ib>it ? (ir-il)*(ib-it) : 0;
				if((r-l)*(bottom-t)!=x.w*x.h+y.w*y.h-intersection){continue;}
				a->rect=(struct rect){l,t,r-l,bottom-t};b->rect=(struct rect){0};again=true;
			}
		}
	}
	while(again);
}

void surface_apply(struct surface *v){
	struct server *s=v->server;
	struct surface_state *state=&v->cache;
	bool changed=false, fresh;
	if(v->cached){
		if(state->opaque_set){v->opaque_region=state->opaque_region;}
		if(state->scale_set){changed=v->scale!=state->scale; v->scale=state->scale;}
		if(state->attached){
			v->content_x+=state->x; v->content_y+=state->y;
			if(state->buffer){
				struct vc_buffer *vb=vc_buffer_get(state->buffer);
				struct wl_shm_buffer *b=vb ? NULL : wl_shm_buffer_get(state->buffer);
				if(!vb && !b){wl_resource_post_error(v->resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "only wl_shm and e6_vc_image buffers are supported"); return;}
				uint32_t fmt=b ? wl_shm_buffer_get_format(b) : WL_SHM_FORMAT_ARGB8888;
				if(fmt!=WL_SHM_FORMAT_ARGB8888 && fmt!=WL_SHM_FORMAT_XRGB8888){wl_resource_post_error(v->resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "unsupported shm format"); return;}
				int w=vb ? vb->width : wl_shm_buffer_get_width(b), h=vb ? vb->height : wl_shm_buffer_get_height(b);
				if(w%v->scale || h%v->scale){wl_resource_post_error(v->resource,WL_SURFACE_ERROR_INVALID_SIZE,"buffer size is not divisible by scale");return;}
				struct rect hint=v->opaque_region;
				bool opaque=vb ? (vb->format&7)>=2 : fmt==WL_SHM_FORMAT_XRGB8888 || (hint.w>0 && hint.h>0 &&
					hint.x<=v->content_x && hint.y<=v->content_y &&
					hint.x+hint.w>=v->content_x+w/v->scale && hint.y+hint.h>=v->content_y+h/v->scale);

				fresh=!v->texture || w!=v->width || h!=v->height || v->opaque!=opaque || v->vc;
				bool uploaded=false,ok=true;
				if(vb){renderer_import(v,vb);uploaded=true;}
				else if(fresh){ok=renderer_upload(v,b,(struct rect){0},true,opaque);uploaded=true;}
				else{
					struct damage_rect *d;
					damage_merge(state,v,w,h);
					wl_list_for_each(d,&state->damages,link){
						struct rect rect=d->rect;
						
						if(!rect.w || !rect.h){continue;}
						if(!renderer_upload(v,b,rect,false,opaque)){ok=false;break;}
						uploaded=true;
					}
				}
				if(!ok){wl_resource_post_error(v->resource,WL_DISPLAY_ERROR_IMPLEMENTATION,vb ? "vc image import failed" : "texture upload failed");return;}
				v->width=w;v->height=h;v->opaque=opaque;
				changed|=uploaded || !v->mapped;
				bool was_mapped=v->mapped;v->mapped=true;
				if(!was_mapped){surface_output(v,true);if(!v->subsurface && !v->cursor){surface_focus(v);if(v->xdg && !v->parent){ui_window_mapped(s,v);}}}
				if(vb){buffer_forget(&v->retired);buffer_move(&v->retired,&v->shown);buffer_move(&v->shown,state);s->retire=true;}
				else{buffer_forget(&v->shown);buffer_forget(&v->retired);buffer_forget(state);}
			}else{
				buffer_forget(&v->shown);buffer_forget(&v->retired);
				if(v->mapped){surface_output(v,false);}
				changed|=v->mapped;v->mapped=false;renderer_drop(v);
				if(v->xdg){v->configured=false;v->acked=false;}
				if(s->focus==v){input_focus(s,NULL);surface_focus_next(s);}
			}
		}
		wl_list_insert_list(v->callbacks.prev,&state->callbacks);wl_list_init(&state->callbacks);
		damages_finish(state);state_init(state,v);v->cached=false;
	}
	struct surface *child, *tmp;
	wl_list_for_each_safe(child,tmp,&s->surfaces,link){
		if(child->parent!=v || !child->subsurface){continue;}
		if(child->position_pending){child->x=child->pending_x;child->y=child->pending_y;child->position_pending=false;changed=true;}
		if(child->place_pending && child->sibling){
			surface_restack(child,child->sibling,child->above);
			child->place_pending=false;changed=true;
		}
		if(child->cached && surface_synced(child)){surface_apply(child);}
	}
	if(changed || !wl_list_empty(&v->callbacks)){schedule_repaint(s,changed);}
}

static void commit(struct wl_client *c, struct wl_resource *r){
	struct surface *v=wl_resource_get_user_data(r); (void)c;
	if(v->xdg && !v->configured){
		if(v->pending.attached && v->pending.buffer){wl_resource_post_error(v->xdg, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "buffer before initial configure"); return;}
		if(!v->role){return;}
		v->configured=true; v->configure_serial=wl_display_next_serial(v->server->display);
		if(!v->parent){
			uint32_t states[]={XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED};
			struct wl_array a={sizeof(states),sizeof(states),states};
			v->y=v->server->inset;
			xdg_toplevel_send_configure(v->role, screen_w,screen_h-v->server->inset,&a);
		}else{xdg_popup_send_configure(v->role,v->x,v->y,v->width,v->height);}
		xdg_surface_send_configure(v->xdg,v->configure_serial);
	}
	if(v->xdg && v->pending.attached && v->pending.buffer && !v->acked){wl_resource_post_error(v->xdg, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "configure not acknowledged"); return;}
	if(!state_commit(v)){return;}
	if(surface_synced(v)){return;}
	surface_apply(v);
}

static void transform(struct wl_client *c, struct wl_resource *r, int32_t t){(void)c; if(t!=WL_OUTPUT_TRANSFORM_NORMAL){wl_resource_post_error(r, WL_SURFACE_ERROR_INVALID_TRANSFORM, "only normal transform is supported");}}
static void scale(struct wl_client *c, struct wl_resource *r, int32_t n){
	(void)c; if(n<=0){wl_resource_post_error(r, WL_SURFACE_ERROR_INVALID_SCALE, "invalid scale"); return;}
	struct surface *v=wl_resource_get_user_data(r);v->pending.scale=n;v->pending.scale_set=true;
}

static const struct wl_surface_interface surface_impl={.destroy=destroy,.attach=attach,.damage=damage,.frame=frame,.set_opaque_region=opaque_set,.set_input_region=region_set,.commit=commit,.set_buffer_transform=transform,.set_buffer_scale=scale,.damage_buffer=damage_buffer};
static void create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id){
	struct server *s=wl_resource_get_user_data(r);
	struct surface *v=calloc(1,sizeof(*v));
	if(!v){wl_client_post_no_memory(c); return;}
	v->server=s; v->scale=1;
	v->resource=wl_resource_create(c,&wl_surface_interface,wl_resource_get_version(r),id);
	if(!v->resource){free(v); wl_client_post_no_memory(c); return;}
	wl_list_init(&v->callbacks); state_init(&v->pending,v);state_init(&v->cache,v);state_init(&v->shown,v);state_init(&v->retired,v);
	wl_list_insert(s->surfaces.prev,&v->link);
	wl_resource_set_implementation(v->resource,&surface_impl,v,surface_destroy);
}

static void region_add(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){
	(void)c;struct region *region=wl_resource_get_user_data(r);
	if(w>0 && h>0 && (int64_t)w*h>region->opaque.w*region->opaque.h){region->opaque=(struct rect){x,y,w,h};}
}

static void region_subtract(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){
	(void)c;struct region *region=wl_resource_get_user_data(r);struct rect a=region->opaque;
	if(w>0 && h>0 && x<a.x+a.w && (int64_t)x+w>a.x && y<a.y+a.h && (int64_t)y+h>a.y){region->opaque=(struct rect){0};}
}

static void region_destroy(struct wl_resource *r){free(wl_resource_get_user_data(r));}
static const struct wl_region_interface region_impl={.destroy=destroy,.add=region_add,.subtract=region_subtract};
static void create_region(struct wl_client *c, struct wl_resource *r, uint32_t id){
	(void)r;struct region *hint=calloc(1,sizeof(*hint));
	if(!hint){wl_client_post_no_memory(c);return;}
	struct wl_resource *region=wl_resource_create(c,&wl_region_interface,1,id);
	if(!region){free(hint);wl_client_post_no_memory(c);return;}
	wl_resource_set_implementation(region,&region_impl,hint,region_destroy);
}

static const struct wl_compositor_interface compositor_impl={.create_surface=create_surface,.create_region=create_region};
static void bind_compositor(struct wl_client *c, void *data, uint32_t ver, uint32_t id){
	struct wl_resource *r=wl_resource_create(c,&wl_compositor_interface,ver,id); wl_resource_set_implementation(r,&compositor_impl,data,NULL);
}

static void output_destroy(struct wl_resource *r){struct input_resource *o=wl_resource_get_user_data(r);wl_list_remove(&o->link);free(o);}
static const struct wl_output_interface output_impl={.release=destroy};
static void bind_output(struct wl_client *c, void *data, uint32_t ver, uint32_t id){
	struct server *s=data;struct input_resource *o=calloc(1,sizeof(*o));
	if(!o){wl_client_post_no_memory(c);return;}
	struct wl_resource *r=wl_resource_create(c,&wl_output_interface,ver,id);
	if(!r){free(o);wl_client_post_no_memory(c);return;}
	o->server=s;o->resource=r;wl_list_insert(&s->outputs,&o->link);
	wl_resource_set_implementation(r,&output_impl,o,output_destroy);
	wl_output_send_geometry(r,0,0,screen_w>screen_h ? 53 : 40,screen_w>screen_h ? 40 : 53,WL_OUTPUT_SUBPIXEL_UNKNOWN,"Nokia","E6",WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r,WL_OUTPUT_MODE_CURRENT|WL_OUTPUT_MODE_PREFERRED,screen_w,screen_h,60000);
	if(ver>=2){wl_output_send_scale(r,1); wl_output_send_done(r);}
	struct surface *v;wl_list_for_each(v,&s->surfaces,link){if(surface_visible(v) && wl_resource_get_client(v->resource)==c){wl_surface_send_enter(v->resource,r);}}
}

static int stats_write(void *data){
	struct server *s=data;
	char path[PATH_MAX];
	if(snprintf(path,sizeof(path),"%s.tmp",s->stats_path)<(int)sizeof(path)){
		FILE *f=fopen(path,"w");
		if(f){
			fprintf(f,"%ld %u %llu %llu %llu %llu %llu\n",(long)getpid(),milliseconds(),
				(unsigned long long)s->frames,(unsigned long long)s->uploaded,
				(unsigned long long)s->uploads,(unsigned long long)s->upload_ms,(unsigned long long)s->present_ms);
			if(fclose(f)==0){rename(path,s->stats_path);}
		}
	}
	wl_event_source_timer_update(s->stats,1000);
	return 0;
}

static int stop(int signal, void *data){(void)signal; wl_display_terminate(((struct server *)data)->display); return 0;}
int main(int argc, char **argv){
	struct server s={.keymap_fd=-1,.input_fd=-1,.console_fd=-1,.zoom=1};
	const char *socket="wayland-0";
	bool ui=false,start_terminal=false;
	if(argc==2 && !strcmp(argv[1],"--restore-console")){console_restore();return 0;}
	for(int i=1;i<argc;i++){
#if defined(YAMASHELL_HOST) || defined(YAMASHELL_TEST)
		if(!strcmp(argv[i],"--input-fd") && i+1<argc){s.input_fd=atoi(argv[++i]);continue;}
#endif
		if(!strcmp(argv[i],"--headless")){s.headless=true;}
		else if(!strcmp(argv[i],"--ui")){ui=true;}
		else if(!strcmp(argv[i],"--start-terminal")){start_terminal=true;}
		else if(!strcmp(argv[i],"--capture") && i+1<argc){s.capture=argv[++i];}
		else if(!strcmp(argv[i],"--socket") && i+1<argc){socket=argv[++i];}
		else{fprintf(stderr,"usage: yamashell [--socket name] [--headless] [--ui] [--capture frame.ppm] [--start-terminal]\n"); return 1;}
	}
#if !defined(YAMASHELL_HOST) && !defined(YAMASHELL_TEST)
	if(s.headless || s.capture){fprintf(stderr,"yamashell: headless capture is available in the host build\n"); return 1;}
#endif
	const char *size=getenv("YAMASHELL_SIZE");
	if(size){
		int w=0,h=0;
		if(sscanf(size,"%dx%d",&w,&h)!=2 || w<160 || h<160 || w>2048 || h>2048){fprintf(stderr,"yamashell: bad YAMASHELL_SIZE %s\n",size); return 1;}
		screen_w=w;screen_h=h;
	}
	wl_list_init(&s.surfaces); wl_list_init(&s.keyboards); wl_list_init(&s.pointers); wl_list_init(&s.touches); wl_list_init(&s.devices);wl_list_init(&s.outputs);wl_list_init(&s.buffers);
	if(!renderer_init(&s)){fprintf(stderr,"yamashell: renderer init failed (EGL %#x)\n",eglGetError()); return 1;}
	s.display=wl_display_create(); s.loop=wl_display_get_event_loop(s.display);
	const char *rate=getenv("YAMASHELL_FPS");
	if(rate){
		char *end;long fps=strtol(rate,&end,10);
		if(end!=rate && !*end && fps>=30 && fps<=240){s.repaint_interval=1000/fps;}
	}
	s.repaint=wl_event_loop_add_timer(s.loop,repaint,&s);
	s.stats_path=getenv("YAMASHELL_STATS");
	if(s.stats_path){s.stats=wl_event_loop_add_timer(s.loop,stats_write,&s);stats_write(&s);}
	wl_global_create(s.display,&wl_compositor_interface,4,&s,bind_compositor);
	wl_global_create(s.display,&wl_output_interface,3,&s,bind_output);
	wl_global_create(s.display,&e6_vc_image_interface,1,&s,bind_vc_image);
	if(wl_display_init_shm(s.display)<0){return 1;}
	shell_init(&s); data_init(&s); input_init(&s);
	/* the shell UI is on by default; headless window tests opt in with --ui */
	if(!s.headless || ui){ui_init(&s);}
	if(wl_display_add_socket(s.display,socket)<0){fprintf(stderr,"yamashell: cannot create %s\n",socket); return 1;}
	/* apps launched from the shell connect here */
	setenv("WAYLAND_DISPLAY",socket,1);
	if(start_terminal){ui_start_terminal(&s);}
	struct wl_event_source *sigint=wl_event_loop_add_signal(s.loop,SIGINT,stop,&s);
	struct wl_event_source *sigterm=wl_event_loop_add_signal(s.loop,SIGTERM,stop,&s);
	console_init(&s);
	fprintf(stderr,"yamashell: listening on %s\n",socket);
	schedule_repaint(&s,true); wl_display_run(s.display);
	wl_display_destroy_clients(s.display); input_finish(&s); data_finish(&s); ui_finish(&s);
	wl_event_source_remove(sigint); wl_event_source_remove(sigterm); wl_event_source_remove(s.repaint);
	if(s.stats){wl_event_source_remove(s.stats);unlink(s.stats_path);}
	wl_display_destroy(s.display); renderer_finish(&s); console_finish(&s);
	fprintf(stderr,"yamashell: frames=%llu uploaded=%llu bytes\n",(unsigned long long)s.frames,(unsigned long long)s.uploaded);
	return 0;
}