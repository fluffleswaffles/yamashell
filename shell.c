// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include "yamashell.h"

static void destroy(struct wl_client *c, struct wl_resource *r){(void)c; wl_resource_destroy(r);}
static void noop(struct wl_client *c, struct wl_resource *r){(void)c;(void)r;}

static void role_destroy(struct wl_resource *r){
	struct surface *v=wl_resource_get_user_data(r);
	if(!v){return;}
	v->role=NULL; v->mapped=false; v->configured=false; v->acked=false;
	if(v->server->focus==v){input_focus(v->server,NULL);surface_focus_next(v->server);}
	schedule_repaint(v->server,true);
}

static void top_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *parent){(void)c;(void)r;(void)parent;}
static void text(struct wl_client *c, struct wl_resource *r, const char *str){(void)c;(void)r;(void)str;}
static void menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y){(void)c;(void)r;(void)seat;(void)serial;(void)x;(void)y;}
static void move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial){(void)c;(void)r;(void)seat;(void)serial;}
static void resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges){(void)c;(void)r;(void)seat;(void)serial;(void)edges;}
static void limits(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h){(void)c;(void)r;(void)w;(void)h;}
static void fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *output){(void)c;(void)r;(void)output;}
static const struct xdg_toplevel_interface top_impl={
	.destroy=destroy,.set_parent=top_parent,.set_title=text,.set_app_id=text,.show_window_menu=menu,.move=move,.resize=resize,
	.set_max_size=limits,.set_min_size=limits,.set_maximized=noop,.unset_maximized=noop,.set_fullscreen=fullscreen,.unset_fullscreen=noop,.set_minimized=noop
};

static void get_top(struct wl_client *c, struct wl_resource *r, uint32_t id){
	struct surface *v=wl_resource_get_user_data(r);
	if(!v){return;}
	if(v->role){wl_resource_post_error(r,XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,"surface already has a role"); return;}
	v->role=wl_resource_create(c,&xdg_toplevel_interface,1,id);
	wl_resource_set_implementation(v->role,&top_impl,v,role_destroy);
}

struct positioner{int w,h;int64_t x,y,aw,ah,dx,dy; uint32_t anchor,gravity;};
static void pos_destroy(struct wl_resource *r){free(wl_resource_get_user_data(r));}
static void pos_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h){
	(void)c; struct positioner *p=wl_resource_get_user_data(r);
	if(w<=0 || h<=0){wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid size");return;}
	p->w=w;p->h=h;
}

static void pos_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h){
	(void)c; struct positioner *p=wl_resource_get_user_data(r);
	if(w<=0 || h<=0){wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid anchor rect");return;}
	p->x=x;p->y=y;p->aw=w;p->ah=h;
}

static void pos_anchor(struct wl_client *c, struct wl_resource *r, uint32_t n){(void)c;((struct positioner *)wl_resource_get_user_data(r))->anchor=n;}
static void pos_gravity(struct wl_client *c, struct wl_resource *r, uint32_t n){(void)c;((struct positioner *)wl_resource_get_user_data(r))->gravity=n;}
static void pos_constraint(struct wl_client *c, struct wl_resource *r, uint32_t n){(void)c;(void)r;(void)n;}
static void pos_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y){(void)c;struct positioner *p=wl_resource_get_user_data(r);p->dx=x;p->dy=y;}
static const struct xdg_positioner_interface pos_impl={.destroy=destroy,.set_size=pos_size,.set_anchor_rect=pos_rect,.set_anchor=pos_anchor,.set_gravity=pos_gravity,.set_constraint_adjustment=pos_constraint,.set_offset=pos_offset};
static void create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id){
	(void)r; struct positioner *p=calloc(1,sizeof(*p));
	if(!p){wl_client_post_no_memory(c);return;}
	struct wl_resource *n=wl_resource_create(c,&xdg_positioner_interface,1,id);
	wl_resource_set_implementation(n,&pos_impl,p,pos_destroy);
}

static void grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial){(void)c;(void)r;(void)seat;(void)serial;}
static const struct xdg_popup_interface popup_impl={.destroy=destroy,.grab=grab};
static void get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *pos){
	struct surface *v=wl_resource_get_user_data(r), *p=parent ? wl_resource_get_user_data(parent) : NULL;
	struct positioner *a=wl_resource_get_user_data(pos);
	if(!v){return;}
	if(v->role || !p || !a->w || !a->h){wl_resource_post_error(r,XDG_WM_BASE_ERROR_INVALID_POSITIONER,"invalid popup parent or positioner");return;}
	v->parent=p;v->width=a->w;v->height=a->h;
	v->x=a->x+a->aw/2+a->dx;v->y=a->y+a->ah/2+a->dy;
	if(a->anchor==3 || a->anchor==5 || a->anchor==6){v->x=a->x+a->dx;}
	if(a->anchor==4 || a->anchor==7 || a->anchor==8){v->x=a->x+a->aw+a->dx;}
	if(a->anchor==1 || a->anchor==5 || a->anchor==7){v->y=a->y+a->dy;}
	if(a->anchor==2 || a->anchor==6 || a->anchor==8){v->y=a->y+a->ah+a->dy;}
	if(a->gravity==3 || a->gravity==5 || a->gravity==6){v->x-=a->w;}
	else if(a->gravity!=4 && a->gravity!=7 && a->gravity!=8){v->x-=a->w/2;}
	if(a->gravity==1 || a->gravity==5 || a->gravity==7){v->y-=a->h;}
	else if(a->gravity!=2 && a->gravity!=6 && a->gravity!=8){v->y-=a->h/2;}
	v->role=wl_resource_create(c,&xdg_popup_interface,1,id);
	wl_resource_set_implementation(v->role,&popup_impl,v,role_destroy);
}

static void geometry(struct wl_client *c, struct wl_resource *r, int32_t x,int32_t y,int32_t w,int32_t h){
	(void)c;struct surface *v=wl_resource_get_user_data(r); if(!v){return;}
	if(w<=0 || h<=0){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SIZE,"invalid window geometry");return;}
	v->geometry_x=x;v->geometry_y=y;
}

static void ack(struct wl_client *c, struct wl_resource *r, uint32_t serial){
	(void)c;struct surface *v=wl_resource_get_user_data(r); if(!v){return;}
	if(serial!=v->configure_serial){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SERIAL,"unknown configure serial");return;}
	v->acked=true;
}

static void xdg_destroy(struct wl_resource *r){struct surface *v=wl_resource_get_user_data(r);if(v){v->xdg=NULL;}}
static const struct xdg_surface_interface xdg_impl={.destroy=destroy,.get_toplevel=get_top,.get_popup=get_popup,.set_window_geometry=geometry,.ack_configure=ack};
static void get_xdg(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface){
	struct surface *v=wl_resource_get_user_data(surface);
	if(v->xdg || v->subsurface){wl_resource_post_error(r,XDG_WM_BASE_ERROR_ROLE,"surface already has a role");return;}
	v->xdg=wl_resource_create(c,&xdg_surface_interface,1,id);
	wl_resource_set_implementation(v->xdg,&xdg_impl,v,xdg_destroy);
}

static void pong(struct wl_client *c, struct wl_resource *r, uint32_t n){(void)c;(void)r;(void)n;}
static const struct xdg_wm_base_interface shell_impl={.destroy=destroy,.create_positioner=create_positioner,.get_xdg_surface=get_xdg,.pong=pong};
static void bind_shell(struct wl_client *c, void *data, uint32_t ver, uint32_t id){
	struct wl_resource *r=wl_resource_create(c,&xdg_wm_base_interface,ver,id);wl_resource_set_implementation(r,&shell_impl,data,NULL);
}

static void sub_position(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y){
	(void)c;struct surface *v=wl_resource_get_user_data(r);if(!v){return;}v->pending_x=x;v->pending_y=y;v->position_pending=true;
}

static void sub_place(struct wl_client *c,struct wl_resource *r,struct wl_resource *sibling){
	(void)c;struct surface *v=wl_resource_get_user_data(r),*other=wl_resource_get_user_data(sibling);
	if(!v){return;}
	if(other==v || (other!=v->parent && other->parent!=v->parent)){wl_resource_post_error(r,WL_SUBSURFACE_ERROR_BAD_SURFACE,"invalid sibling");return;}
	v->sibling=other;v->above=true;v->place_pending=true;
}

static void sub_below(struct wl_client *c,struct wl_resource *r,struct wl_resource *sibling){
	(void)c;struct surface *v=wl_resource_get_user_data(r),*other=wl_resource_get_user_data(sibling);
	if(!v){return;}
	if(other==v || (other!=v->parent && other->parent!=v->parent)){wl_resource_post_error(r,WL_SUBSURFACE_ERROR_BAD_SURFACE,"invalid sibling");return;}
	v->sibling=other;v->above=false;v->place_pending=true;
}

static void sync_set(struct wl_client *c,struct wl_resource *r){(void)c;struct surface *v=wl_resource_get_user_data(r);if(v){v->sync=true;}}
static void desync(struct wl_client *c,struct wl_resource *r){(void)c;struct surface *v=wl_resource_get_user_data(r);if(v){v->sync=false;if(v->cached && !surface_synced(v)){surface_apply(v);}}}
static const struct wl_subsurface_interface sub_impl={.destroy=destroy,.set_position=sub_position,.place_above=sub_place,.place_below=sub_below,.set_sync=sync_set,.set_desync=desync};
static void get_sub(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *surface,struct wl_resource *parent){
	struct surface *v=wl_resource_get_user_data(surface),*p=wl_resource_get_user_data(parent);
	if(v==p || v->role || v->xdg || v->subsurface){wl_resource_post_error(r,WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,"invalid subsurface");return;}
	for(struct surface *a=p;a;a=a->parent){if(a==v){wl_resource_post_error(r,WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,"surface cycle");return;}}
	surface_restack(v,p,true);
	v->parent=p;v->subsurface=true;v->sync=true;
	v->role=wl_resource_create(c,&wl_subsurface_interface,1,id);wl_resource_set_implementation(v->role,&sub_impl,v,role_destroy);
}

static const struct wl_subcompositor_interface subcompositor_impl={.destroy=destroy,.get_subsurface=get_sub};
static void bind_sub(struct wl_client *c,void *data,uint32_t ver,uint32_t id){
	struct wl_resource *r=wl_resource_create(c,&wl_subcompositor_interface,ver,id);wl_resource_set_implementation(r,&subcompositor_impl,data,NULL);
}

void shell_init(struct server *s){
	wl_global_create(s->display,&xdg_wm_base_interface,1,s,bind_shell);
	wl_global_create(s->display,&wl_subcompositor_interface,1,s,bind_sub);
}

