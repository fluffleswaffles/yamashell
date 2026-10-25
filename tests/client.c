// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wayland-client.h>
#include "xdg-shell-client.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static int configured,done,releases,key_events,touch_down,touch_up,pointer_events;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *keys;
static struct wl_seat *seat;
static struct wl_surface *keyboard_focus;

// trigger warning: unreadable code

static void keymap_event(void *data,struct wl_keyboard *keyboard,uint32_t format,int32_t fd,uint32_t size){
	(void)data;(void)keyboard;assert(format==WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
	char *text=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);assert(text!=MAP_FAILED);

	xkb=xkb_context_new(XKB_CONTEXT_NO_FLAGS);keymap=xkb_keymap_new_from_string(xkb,text,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS);assert(keymap);keys=xkb_state_new(keymap);
	munmap(text,size);close(fd);
}

static void keyboard_enter(void *data,struct wl_keyboard *k,uint32_t serial,struct wl_surface *v,struct wl_array *pressed){(void)data;(void)k;(void)serial;(void)pressed;keyboard_focus=v;}
static void keyboard_leave(void *data,struct wl_keyboard *k,uint32_t serial,struct wl_surface *v){(void)data;(void)k;(void)serial;(void)v;keyboard_focus=NULL;}
static void key_event(void *data,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t key,uint32_t state){
	(void)data;(void)k;(void)serial;(void)time;assert(keys);key_events++;
	if(key==19 && state==1){assert(xkb_state_key_get_one_sym(keys,key+8)==XKB_KEY_1);}
}

static void mods(void *data,struct wl_keyboard *k,uint32_t serial,uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group){(void)data;(void)k;(void)serial;xkb_state_update_mask(keys,depressed,latched,locked,0,0,group);}
static void repeat(void *data,struct wl_keyboard *k,int32_t rate,int32_t delay){(void)data;(void)k;assert(rate==20 && delay==500);}
static const struct wl_keyboard_listener keyboard_listener={.keymap=keymap_event,.enter=keyboard_enter,.leave=keyboard_leave,.key=key_event,.modifiers=mods,.repeat_info=repeat};
static void pointer_enter(void *data,struct wl_pointer *p,uint32_t serial,struct wl_surface *v,wl_fixed_t x,wl_fixed_t y){(void)data;(void)p;(void)serial;(void)v;(void)x;(void)y;}
static void pointer_leave(void *data,struct wl_pointer *p,uint32_t serial,struct wl_surface *v){(void)data;(void)p;(void)serial;(void)v;}
static void pointer_motion(void *data,struct wl_pointer *p,uint32_t time,wl_fixed_t x,wl_fixed_t y){(void)data;(void)p;(void)time;(void)x;(void)y;pointer_events++;}
static void pointer_frame(void *data,struct wl_pointer *p){(void)data;(void)p;}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.frame=pointer_frame};
static void touch_down_event(void *data,struct wl_touch *t,uint32_t serial,uint32_t time,struct wl_surface *v,int32_t id,wl_fixed_t x,wl_fixed_t y){(void)data;(void)t;(void)serial;(void)time;(void)v;assert(id==0);assert(wl_fixed_to_int(x)==30 && wl_fixed_to_int(y)==20);touch_down++;}
static void touch_up_event(void *data,struct wl_touch *t,uint32_t serial,uint32_t time,int32_t id){(void)data;(void)t;(void)serial;(void)time;assert(id==0);touch_up++;}
static void touch_frame(void *data,struct wl_touch *t){(void)data;(void)t;}
static const struct wl_touch_listener touch_listener={.down=touch_down_event,.up=touch_up_event,.frame=touch_frame};

static void capabilities(void *data,struct wl_seat *r,uint32_t caps){
	(void)data;assert(caps & WL_SEAT_CAPABILITY_KEYBOARD);wl_keyboard_add_listener(wl_seat_get_keyboard(r),&keyboard_listener,NULL);
	wl_pointer_add_listener(wl_seat_get_pointer(r),&pointer_listener,NULL);wl_touch_add_listener(wl_seat_get_touch(r),&touch_listener,NULL);
}
static void seat_name(void *data,struct wl_seat *r,const char *name){(void)data;(void)r;assert(!strcmp(name,"e6"));}
static const struct wl_seat_listener seat_listener={capabilities,seat_name};
static struct wl_data_device_manager *manager;
static struct wl_data_offer *selection_offer;

static void offer_mime(void *data,struct wl_data_offer *offer,const char *mime){(void)data;(void)offer;assert(!strcmp(mime,"text/plain"));}
static const struct wl_data_offer_listener offer_listener={.offer=offer_mime};
static void data_offer(void *data,struct wl_data_device *device,struct wl_data_offer *offer){(void)data;(void)device;wl_data_offer_add_listener(offer,&offer_listener,NULL);}
static void data_selection(void *data,struct wl_data_device *device,struct wl_data_offer *offer){(void)data;(void)device;selection_offer=offer;}

static const struct wl_data_device_listener device_listener={.data_offer=data_offer,.selection=data_selection};
static void source_send(void *data,struct wl_data_source *source,const char *mime,int32_t fd){(void)data;(void)source;assert(!strcmp(mime,"text/plain"));assert(write(fd,"clipboard",9)==9);close(fd);}
static void source_cancel(void *data,struct wl_data_source *source){(void)data;(void)source;}
static const struct wl_data_source_listener source_listener={.send=source_send,.cancelled=source_cancel};

static void global(void *data,struct wl_registry *r,uint32_t id,const char *interface,uint32_t version){
	(void)data;(void)version;
	if(!strcmp(interface,"wl_data_device_manager")){manager=wl_registry_bind(r,id,&wl_data_device_manager_interface,3);}
	if(!strcmp(interface,"wl_seat")){seat=wl_registry_bind(r,id,&wl_seat_interface,5);wl_seat_add_listener(seat,&seat_listener,NULL);}
	if(!strcmp(interface,"wl_compositor")){compositor=wl_registry_bind(r,id,&wl_compositor_interface,4);}
	if(!strcmp(interface,"wl_subcompositor")){subcompositor=wl_registry_bind(r,id,&wl_subcompositor_interface,1);}
	if(!strcmp(interface,"wl_shm")){shm=wl_registry_bind(r,id,&wl_shm_interface,1);}
	if(!strcmp(interface,"xdg_wm_base")){shell=wl_registry_bind(r,id,&xdg_wm_base_interface,1);}
}

static void removed(void *data,struct wl_registry *r,uint32_t id){(void)data;(void)r;(void)id;}
static const struct wl_registry_listener registry_listener={global,removed};
static void configure(void *data,struct xdg_surface *r,uint32_t serial){(void)data;xdg_surface_ack_configure(r,serial);configured=1;}
static const struct xdg_surface_listener xdg_listener={configure};
static void top_configure(void *data,struct xdg_toplevel *r,int32_t w,int32_t h,struct wl_array *states){(void)data;(void)r;(void)w;(void)h;(void)states;}
static void close_top(void *data,struct xdg_toplevel *r){(void)data;(void)r;}
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=close_top};
static void frame_done(void *data,struct wl_callback *cb,uint32_t time){(void)data;(void)time;done=1;wl_callback_destroy(cb);}
static const struct wl_callback_listener callback_listener={frame_done};
static void released(void *data,struct wl_buffer *b){(void)data;(void)b;releases++;}
static const struct wl_buffer_listener buffer_listener={released};

static struct wl_buffer *buffer(int w,int h,int stride,uint32_t format,uint32_t **pixels){
	int fd=memfd_create("yamashell-client",MFD_CLOEXEC);assert(fd>=0);assert(!ftruncate(fd,(size_t)stride*h));
	*pixels=mmap(NULL,(size_t)stride*h,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(*pixels!=MAP_FAILED);

	struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,stride*h);
	struct wl_buffer *b=wl_shm_pool_create_buffer(pool,0,w,h,stride,format);wl_shm_pool_destroy(pool);close(fd);

	wl_buffer_add_listener(b,&buffer_listener,NULL);return b;
}

static void present(struct wl_surface *v,const char *name){
	done=0;wl_callback_add_listener(wl_surface_frame(v),&callback_listener,NULL);wl_surface_commit(v);
	while(!done){assert(wl_display_dispatch(display)>=0);}
	assert(wl_display_roundtrip(display)>=0);
	printf("%s releases=%d\n",name,releases);fflush(stdout);
	assert(getchar()!=EOF);

}

int main(void){
	display=wl_display_connect(NULL);assert(display);
	struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
	assert(wl_display_roundtrip(display)>=0);assert(compositor && subcompositor && shm && shell);
	struct wl_surface *v=wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg=xdg_wm_base_get_xdg_surface(shell,v);xdg_surface_add_listener(xdg,&xdg_listener,NULL);
	struct xdg_toplevel *top=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(top,&top_listener,NULL);

	wl_surface_commit(v);while(!configured){assert(wl_display_dispatch(display)>=0);}
	uint32_t *pixels;int stride=272;
	struct wl_buffer *b=buffer(64,32,stride,WL_SHM_FORMAT_ARGB8888,&pixels);
	struct wl_region *opaque=wl_compositor_create_region(compositor);
	wl_region_add(opaque,0,0,64,32);wl_surface_set_opaque_region(v,opaque);

	/* set_opaque_region copies the hint, later mutation/destruction is independent. */
	wl_region_subtract(opaque,0,0,64,32);wl_region_destroy(opaque);
	for(int y=0;y<32;y++){for(int x=0;x<64;x++){pixels[y*(stride/4)+x]=y<16 ? 0xffff0000 : 0xff00ff00;}}
	wl_surface_attach(v,b,0,0);wl_surface_damage_buffer(v,0,0,INT32_MAX,INT32_MAX);present(v,"initial");assert(releases==1);
	printf("input-ready\n");fflush(stdout);assert(getchar()!=EOF);

	assert(wl_display_roundtrip(display)>=0);assert(key_events==4);assert(touch_down==1 && touch_up==1);assert(pointer_events>=1);
	assert(manager && seat);

	struct wl_data_device *device=wl_data_device_manager_get_data_device(manager,seat);wl_data_device_add_listener(device,&device_listener,NULL);
	struct wl_data_source *source=wl_data_device_manager_create_data_source(manager);wl_data_source_add_listener(source,&source_listener,NULL);wl_data_source_offer(source,"text/plain");

	wl_data_device_set_selection(device,source,0);assert(wl_display_roundtrip(display)>=0);assert(selection_offer);
	int pipefd[2];assert(!pipe(pipefd));wl_data_offer_receive(selection_offer,"text/plain",pipefd[1]);close(pipefd[1]);
	assert(wl_display_roundtrip(display)>=0);char pasted[16]={0};assert(read(pipefd[0],pasted,sizeof(pasted))==9);assert(!strcmp(pasted,"clipboard"));close(pipefd[0]);
	wl_data_source_destroy(source);

	printf("input-ok\n");fflush(stdout);assert(getchar()!=EOF);
	for(int y=0;y<32;y++){for(int x=0;x<64;x++){pixels[y*(stride/4)+x]=0xffffffff;}}
	for(int y=7;y<10;y++){for(int x=5;x<9;x++){pixels[y*(stride/4)+x]=0xff0000ff;}}
	for(int y=7;y<10;y++){for(int x=20;x<23;x++){pixels[y*(stride/4)+x]=0xff0000ff;}}

	wl_surface_attach(v,b,0,0);wl_surface_damage_buffer(v,5,7,4,1);wl_surface_damage_buffer(v,5,8,4,1);wl_surface_damage_buffer(v,5,9,4,1);wl_surface_damage_buffer(v,5,8,4,1);wl_surface_damage_buffer(v,20,7,3,3);present(v,"damage");assert(releases==2);
	done=0;wl_callback_add_listener(wl_surface_frame(v),&callback_listener,NULL);
	assert(wl_display_roundtrip(display)>=0);assert(!done);
	printf("pending-frame\n");fflush(stdout);assert(getchar()!=EOF);assert(!done);
	present(v,"frame-only");

	struct wl_surface *child=wl_compositor_create_surface(compositor);
	struct wl_subsurface *sub=wl_subcompositor_get_subsurface(subcompositor,child,v);wl_subsurface_set_position(sub,10,10);
	opaque=wl_compositor_create_region(compositor);wl_region_add(opaque,0,0,16,8);wl_region_subtract(opaque,0,0,16,8);
	wl_surface_set_opaque_region(child,opaque);wl_region_destroy(opaque);
	uint32_t *alpha;struct wl_buffer *ab=buffer(16,8,64,WL_SHM_FORMAT_ARGB8888,&alpha);
	for(int i=0;i<16*8;i++){alpha[i]=0x80000080;}

	wl_surface_attach(child,ab,0,0);wl_surface_damage_buffer(child,0,0,16,8);wl_surface_commit(child);
	assert(wl_display_roundtrip(display)>=0);assert(releases==2);
	printf("cached releases=%d\n",releases);fflush(stdout);assert(getchar()!=EOF);
	uint32_t *green;struct wl_buffer *gb=buffer(16,8,64,WL_SHM_FORMAT_ARGB8888,&green);
	for(int i=0;i<16*8;i++){green[i]=0x80008000;}

	wl_surface_attach(child,gb,0,0);wl_surface_damage_buffer(child,0,0,16,8);
	present(v,"alpha");assert(releases==3);
	wl_surface_commit(child);present(v,"alpha-green");assert(releases==4);
	struct wl_surface *nested=wl_compositor_create_surface(compositor);
	struct wl_subsurface *nested_sub=wl_subcompositor_get_subsurface(subcompositor,nested,child);

	wl_subsurface_set_position(nested_sub,2,2);
	uint32_t *yellow;struct wl_buffer *yb=buffer(4,4,16,WL_SHM_FORMAT_ARGB8888,&yellow);
	for(int i=0;i<16;i++){yellow[i]=0xffffff00;}

	wl_surface_attach(nested,yb,0,0);wl_surface_damage_buffer(nested,0,0,4,4);wl_surface_commit(nested);
	wl_surface_commit(child);present(v,"nested");
	wl_subsurface_place_below(sub,v);present(v,"nested-below");
	wl_subsurface_place_above(sub,v);present(v,"nested-above");

	configured=0;
	struct wl_surface *foreground=wl_compositor_create_surface(compositor);
	struct xdg_surface *fx=xdg_wm_base_get_xdg_surface(shell,foreground);xdg_surface_add_listener(fx,&xdg_listener,NULL);
	struct xdg_toplevel *ft=xdg_surface_get_toplevel(fx);xdg_toplevel_add_listener(ft,&top_listener,NULL);
	wl_surface_commit(foreground);while(!configured){assert(wl_display_dispatch(display)>=0);}
	uint32_t *cyan;struct wl_buffer *fb=buffer(64,32,256,WL_SHM_FORMAT_XRGB8888,&cyan);
	for(int i=0;i<64*32;i++){cyan[i]=0x0000ffff;}

	wl_surface_attach(foreground,fb,0,0);wl_surface_damage_buffer(foreground,0,0,64,32);present(foreground,"foreground");
	struct wl_surface *background_child=wl_compositor_create_surface(compositor);
	struct wl_subsurface *bc=wl_subcompositor_get_subsurface(subcompositor,background_child,v);
	wl_surface_attach(background_child,yb,0,0);wl_surface_damage_buffer(background_child,0,0,4,4);wl_surface_commit(background_child);
	present(v,"background-child");

	wl_subsurface_destroy(bc);wl_surface_destroy(background_child);
	xdg_toplevel_destroy(ft);xdg_surface_destroy(fx);wl_surface_destroy(foreground);wl_buffer_destroy(fb);munmap(cyan,64*32*4);
	present(v,"foreground-restored");assert(keyboard_focus==v);
	wl_subsurface_destroy(nested_sub);wl_surface_destroy(nested);wl_buffer_destroy(yb);munmap(yellow,64);
	wl_surface_attach(child,NULL,0,0);wl_surface_commit(child);present(v,"unmap");
	wl_subsurface_destroy(sub);wl_surface_destroy(child);wl_buffer_destroy(ab);munmap(alpha,16*8*4);wl_buffer_destroy(gb);munmap(green,16*8*4);
	wl_surface_set_buffer_scale(v,2);wl_surface_commit(v);present(v,"scale");


	int before_shared=releases;
	struct wl_surface *shared_a=wl_compositor_create_surface(compositor),*shared_b=wl_compositor_create_surface(compositor);
	struct wl_subsurface *sa=wl_subcompositor_get_subsurface(subcompositor,shared_a,v),*sb=wl_subcompositor_get_subsurface(subcompositor,shared_b,v);
	uint32_t *shared_pixels;struct wl_buffer *shared=buffer(16,8,64,WL_SHM_FORMAT_ARGB8888,&shared_pixels);
	for(int i=0;i<128;i++){shared_pixels[i]=0xffff00ff;}

	wl_surface_attach(shared_a,shared,0,0);wl_surface_damage_buffer(shared_a,0,0,16,8);wl_surface_commit(shared_a);
	wl_surface_attach(shared_b,shared,0,0);wl_surface_damage_buffer(shared_b,0,0,16,8);wl_surface_commit(shared_b);
	wl_subsurface_destroy(sa);wl_surface_destroy(shared_a);assert(wl_display_roundtrip(display)>=0);assert(releases==before_shared);
	present(v,"shared");assert(releases==before_shared+1);
	wl_subsurface_destroy(sb);wl_surface_destroy(shared_b);wl_buffer_destroy(shared);munmap(shared_pixels,512);
	int rounds=getenv("YAMASHELL_STRESS") ? atoi(getenv("YAMASHELL_STRESS")) : 64;

	for(int i=0;i<rounds;i++){
		struct wl_surface *cv=wl_compositor_create_surface(compositor);
		struct wl_subsurface *cs=wl_subcompositor_get_subsurface(subcompositor,cv,v);
		uint32_t *px;struct wl_buffer *cb=buffer(16,8,64,WL_SHM_FORMAT_ARGB8888,&px);
		for(int j=0;j<128;j++){px[j]=0xff000000u|((uint32_t)i<<8);}
		wl_surface_attach(cv,cb,0,0);wl_surface_damage_buffer(cv,0,0,16,8);wl_surface_commit(cv);
		done=0;wl_callback_add_listener(wl_surface_frame(v),&callback_listener,NULL);wl_surface_commit(v);
		while(!done){assert(wl_display_dispatch(display)>=0);}
		wl_subsurface_destroy(cs);wl_surface_destroy(cv);wl_buffer_destroy(cb);munmap(px,512);
	}
	
	assert(wl_display_roundtrip(display)>=0);
	printf("stress %d cycles\n",rounds);fflush(stdout);assert(getchar()!=EOF);
	configured=0;wl_surface_attach(v,NULL,0,0);wl_surface_commit(v);assert(wl_display_roundtrip(display)>=0);
	wl_surface_commit(v);while(!configured){assert(wl_display_dispatch(display)>=0);}
	wl_surface_attach(v,b,0,0);wl_surface_damage_buffer(v,0,0,64,32);present(v,"remap");
	wl_surface_attach(v,NULL,0,0);wl_surface_commit(v);assert(wl_display_roundtrip(display)>=0);
	wl_buffer_destroy(b);munmap(pixels,(size_t)stride*32);
	xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(v);
	assert(wl_display_roundtrip(display)>=0);
	printf("finished\n");fflush(stdout);assert(getchar()!=EOF);
	wl_display_disconnect(display);return 0;
}