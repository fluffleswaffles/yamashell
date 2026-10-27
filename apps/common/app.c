// SPDX-License-Identifier: GPL-2.0-only
/* the system apps window
 * xdg toplevel drawn with GLES on the VC (libvcgl wayland-egl), only quads cross the bus:
 * the shell's rounded box shader, a glyph atlas, a texture per picture. a frame loop that
 * runs while the app animates. the app works in logical pixels, app_scale bigger on screen */
#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "xdg-shell-client.h"
#include "app.h"

#define ASSETS "/usr/share/yamashell/"
#define ATLAS 512
#define GLYPHS 1024
#define SLOTS 2048 /* glyph hash, power of two */
#define QUADS 4096 /* per frame, the vertex buffer holds that many */
#define TEXTURES 64

const float app_scale=1.25f;
int app_w=512,app_h=360;
/* 20 bytes: positions and box coords in quarter pixels, prm = half size, radius, mode*4 */
struct vtx{int16_t x,y,u,v;uint8_t r,g,b,a;int16_t hw,hh,rad,mode;};
struct glyph{int face,size,x,y,w,h,left,top;uint32_t cp;float adv;};
struct tex{const unsigned char *px;GLuint id;int w,h;};
static struct{
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm;
	struct wl_seat *seat;
	struct wl_surface *surface;
	struct wl_egl_window *win;
	EGLDisplay egl;EGLSurface es;EGLContext ctx;
	GLuint program,flat,atlas,bound,vbo,ebo;GLint ts;
	FT_Library ft;FT_Face face[F_COUNT];
	struct glyph glyphs[GLYPHS];int nglyphs,slot[SLOTS],ax,ay,arow;
	struct tex tex[TEXTURES];int ntex;
	struct vtx v[QUADS*4];int nq,first;
	int pw,ph,touch;uint32_t last;
	bool configured,quit,redraw,moving,resized;
}a={.pw=640,.ph=450,.touch=-1};
/* clip in physical pixels */
static float cx0,cy0,cx1,cy1;

uint32_t app_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000+t.tv_nsec/1000000);}
void app_redraw(void){a.redraw=true;}
void app_quit(void){a.quit=true;}

/* drawing */
/* alacritty's way, proven on the VC: the quads since the last draw go once into the vbo
 * behind the earlier ones of this frame, a static ebo */
static void flush(void){
	int n=a.nq-a.first;
	if(n<=0){return;}
	size_t off=(size_t)a.first*4*sizeof(struct vtx);
	glBufferSubData(GL_ARRAY_BUFFER,(GLintptr)off,(GLsizeiptr)(n*4*sizeof(struct vtx)),a.v+a.first*4);
	glVertexAttribPointer(0,2,GL_SHORT,GL_FALSE,sizeof(struct vtx),(const void *)(off+offsetof(struct vtx,x)));
	glVertexAttribPointer(1,2,GL_SHORT,GL_FALSE,sizeof(struct vtx),(const void *)(off+offsetof(struct vtx,u)));
	glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(struct vtx),(const void *)(off+offsetof(struct vtx,r)));
	glVertexAttribPointer(3,4,GL_SHORT,GL_FALSE,sizeof(struct vtx),(const void *)(off+offsetof(struct vtx,hw)));
	glDrawElements(GL_TRIANGLES,n*6,GL_UNSIGNED_SHORT,NULL);
	a.first=a.nq;
}

static void bind(GLuint id,int w,int h){
	if(a.bound==id){return;}
	flush();
	glBindTexture(GL_TEXTURE_2D,id);glUniform2f(a.ts,.25f/w,.25f/h);
	a.bound=id;
}

void clip(float x0,float y0,float x1,float y1){
	flush();
	cx0=fmaxf(floorf(x0*app_scale),0);cy0=fmaxf(floorf(y0*app_scale),0);
	cx1=fminf(ceilf(x1*app_scale),a.pw);cy1=fminf(ceilf(y1*app_scale),a.ph);
	/* gl y is up */
	glScissor((int)cx0,(int)(a.ph-cy1),(int)fmaxf(cx1-cx0,0),(int)fmaxf(cy1-cy0,0));
}

static int16_t q4(float v){return (int16_t)lroundf(v*4);}

/* physical x, y, w, h; u0..v1 the box (mode 0) or texels (modes 1, 2); colours premultiplied */
static void quad(float x,float y,float w,float h,float u0,float v0,float u1,float v1,const uint8_t top[4],const uint8_t bottom[4],float rad,int mode){
	if(w<=0 || h<=0 || x>=cx1 || y>=cy1 || x+w<=cx0 || y+h<=cy0){return;}
	/* shortcut: a frame past QUADS is cut off, more room if an app ever draws that much */
	if(a.nq==QUADS){return;}
	struct vtx *v=a.v+a.nq++*4;int16_t hw=q4(w/2),hh=q4(h/2),r=q4(rad),m=(int16_t)(mode*4);
	v[0]=(struct vtx){q4(x),q4(y),q4(u0),q4(v0),top[0],top[1],top[2],top[3],hw,hh,r,m};
	v[1]=(struct vtx){q4(x+w),q4(y),q4(u1),q4(v0),top[0],top[1],top[2],top[3],hw,hh,r,m};
	v[2]=(struct vtx){q4(x),q4(y+h),q4(u0),q4(v1),bottom[0],bottom[1],bottom[2],bottom[3],hw,hh,r,m};
	v[3]=(struct vtx){q4(x+w),q4(y+h),q4(u1),q4(v1),bottom[0],bottom[1],bottom[2],bottom[3],hw,hh,r,m};
}

static void col(uint32_t c,float al,uint8_t out[4]){
	al=clampf(al,0,1);
	out[0]=(uint8_t)lroundf((c>>16&255)*al);out[1]=(uint8_t)lroundf((c>>8&255)*al);out[2]=(uint8_t)lroundf((c&255)*al);out[3]=(uint8_t)lroundf(255*al);
}

void fill(float x,float y,float w,float h,float rad,uint32_t top,uint32_t bottom,float al){
	if(w<=0 || h<=0 || al<=0){return;}
	uint8_t t[4],b[4];col(top,al,t);col(bottom,al,b);
	x*=app_scale;y*=app_scale;w*=app_scale;h*=app_scale;rad=fminf(rad*app_scale,fminf(w,h)/2);
	/* big square boxes (backgrounds, dimming) skip the box shader, the VC's fill rate is the limit */
	bool flat=rad<=0 && w*h>=16000;
	if(flat){flush();glUseProgram(a.flat);}
	quad(x,y,w,h,-w/2,-h/2,w/2,h/2,t,b,rad,0);
	if(flat){flush();glUseProgram(a.program);}
}

void background(void){
	/* the shade's gradient is linear in y, the clip's ends are enough */
	float y0=cy0/app_scale,y1=cy1/app_scale;
	uint32_t c0=(uint32_t)(0x26-0x1c*clampf((y0+app_h*.25f)/(app_h*1.25f),0,1))*0x010101;
	uint32_t c1=(uint32_t)(0x26-0x1c*clampf((y1+app_h*.25f)/(app_h*1.25f),0,1))*0x010101;
	fill(cx0/app_scale,y0,(cx1-cx0)/app_scale,y1-y0,0,c0,c1,1);
}

/* textures for pictures and bitmaps, by their pixel pointer */
static struct tex *texture(const unsigned char *px,int w,int h,bool rgb,float rad){
	for(int i=0;i<a.ntex;i++){if(a.tex[i].px==px){return &a.tex[i];}}
	if(a.ntex==TEXTURES){app_forget(a.tex[0].px);}
	unsigned char *rgba=malloc((size_t)w*h*4);
	if(!rgba){return NULL;}
	for(int j=0;j<h;j++){
		float qy=fabsf(j+.5f-h/2.f)-h/2.f+rad;
		for(int i=0;i<w;i++){
			unsigned char *d=rgba+((size_t)j*w+i)*4;
			if(!rgb){memcpy(d,px+((size_t)j*w+i)*4,4);continue;}
			/* rounded corners baked in, premultiplied */
			float qx=fabsf(i+.5f-w/2.f)-w/2.f+rad,dist=qx>0 && qy>0 ? hypotf(qx,qy)-rad : fmaxf(qx,qy)-rad,cov=clampf(.5f-dist,0,1);
			const unsigned char *p=px+((size_t)j*w+i)*3;
			for(int k=0;k<3;k++){d[k]=(unsigned char)lroundf(p[k]*cov);}
			d[3]=(unsigned char)lroundf(255*cov);
		}
	}
	struct tex *t=&a.tex[a.ntex++];
	*t=(struct tex){px,0,w,h};
	flush();
	glGenTextures(1,&t->id);glBindTexture(GL_TEXTURE_2D,t->id);a.bound=0;
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
	free(rgba);
	return t;
}

void app_forget(const unsigned char *px){
	for(int i=0;i<a.ntex;i++){
		if(a.tex[i].px!=px){continue;}
		flush();
		if(a.bound==a.tex[i].id){a.bound=0;}
		glDeleteTextures(1,&a.tex[i].id);
		a.tex[i]=a.tex[--a.ntex];
		return;
	}
}

static void image(float x,float y,int w,int h,const unsigned char *px,bool rgb,float rad,float al){
	if(!px || al<=0){return;}
	struct tex *t=texture(px,w,h,rgb,rad);
	if(!t){return;}
	uint8_t c[4];col(C_WHITE,al,c);
	bind(t->id,w,h);
	quad(roundf(x*app_scale),roundf(y*app_scale),(float)w,(float)h,0,0,(float)w,(float)h,c,c,0,2);
}

void picture(float x,float y,int w,int h,const unsigned char *rgb,float rad,float al){image(x,y,w,h,rgb,true,rad*app_scale,al);}
void bitmap(float x,float y,int w,int h,const unsigned char *rgba,float al){image(x,y,w,h,rgba,false,0,al);}

static uint32_t utf8(const char **p){
	const unsigned char *s=(const unsigned char *)*p;uint32_t c=*s++;
	if(c>=0xf0 && s[0] && s[1] && s[2]){c=(c&7)<<18|(s[0]&63)<<12|(s[1]&63)<<6|(s[2]&63);s+=3;}
	else if(c>=0xe0 && s[0] && s[1]){c=(c&15)<<12|(s[0]&63)<<6|(s[1]&63);s+=2;}
	else if(c>=0xc0 && s[0]){c=(c&31)<<6|(s[0]&63);s++;}
	*p=(const char *)s;return c;
}

static void atlas_reset(void){
	flush();
	a.nglyphs=0;a.ax=a.ay=a.arow=0;
	memset(a.slot,0xff,sizeof(a.slot));
}

/* size in physical pixels */
static struct glyph *glyph(int face,int size,uint32_t cp){
	unsigned h=(cp*2654435761u^(unsigned)size*40503u^(unsigned)face)&(SLOTS-1);
	for(;a.slot[h]>=0;h=(h+1)&(SLOTS-1)){struct glyph *g=&a.glyphs[a.slot[h]];if(g->cp==cp && g->size==size && g->face==face){return g;}}
	FT_Face f=a.face[face];
	if(!f || FT_Set_Pixel_Sizes(f,0,size) || FT_Load_Char(f,cp,FT_LOAD_RENDER|FT_LOAD_TARGET_LIGHT)){return NULL;}
	FT_Bitmap *b=&f->glyph->bitmap;int w=(int)b->width,gh=(int)b->rows;
	if(w+1>ATLAS || gh+1>ATLAS){return NULL;}
	if(a.ax+w+1>ATLAS){a.ax=0;a.ay+=a.arow+1;a.arow=0;}
	/* shortcut: a full atlas starts over, fine for a few screens of text, an lru if apps get text heavy */
	if(a.ay+gh+1>ATLAS || a.nglyphs==GLYPHS){atlas_reset();return glyph(face,size,cp);}
	struct glyph *g=&a.glyphs[a.nglyphs];
	*g=(struct glyph){face,size,a.ax,a.ay,w,gh,f->glyph->bitmap_left,f->glyph->bitmap_top,cp,f->glyph->advance.x/64.f};
	if(w && gh){
		unsigned char *rgba=malloc((size_t)w*gh*4);
		if(!rgba){return NULL;}
		for(int j=0;j<gh;j++){
			for(int i=0;i<w;i++){unsigned char *d=rgba+((size_t)j*w+i)*4;d[0]=d[1]=d[2]=255;d[3]=b->buffer[(ptrdiff_t)j*b->pitch+i];}
		}
		bind(a.atlas,ATLAS,ATLAS);
		glTexSubImage2D(GL_TEXTURE_2D,0,a.ax,a.ay,w,gh,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
		free(rgba);
	}
	a.ax+=w+1;if(gh>a.arow){a.arow=gh;}
	a.slot[h]=a.nglyphs++;
	return g;
}

static void put(struct glyph *g,float x,float y,const uint8_t c[4]){
	if(!g->w || !g->h){return;}
	bind(a.atlas,ATLAS,ATLAS);
	quad(x+g->left,y-g->top,(float)g->w,(float)g->h,(float)g->x,(float)g->y,(float)(g->x+g->w),(float)(g->y+g->h),c,c,0,1);
}

float text(int face,float x,float y,const char *s,int size,uint32_t c,float al,int align){
	int ps=(int)lroundf(size*app_scale);float w=0;struct glyph *g;
	for(const char *p=s;*p;){if((g=glyph(face,ps,utf8(&p)))){w+=g->adv;}}
	if(al<=0){return w/app_scale;}
	uint8_t k[4];col(c,al,k);
	float px=roundf(x*app_scale-(align==1 ? w/2 : align==2 ? w : 0)),py=roundf(y*app_scale);
	for(const char *p=s;*p;){
		if(!(g=glyph(face,ps,utf8(&p)))){continue;}
		put(g,roundf(px),py,k);
		px+=g->adv;
	}
	return w/app_scale;
}

void icon(float cx,float cy,uint32_t cp,int size,uint32_t c,float al){
	struct glyph *g=glyph(F_ICON,(int)lroundf(size*app_scale),cp);
	if(!g || al<=0){return;}
	uint8_t k[4];col(c,al,k);
	put(g,roundf(cx*app_scale-g->w/2.f)-g->left,roundf(cy*app_scale-g->h/2.f)+g->top,k);
}

/* gl setup */
static GLuint compile(GLenum type,const char *src){
	GLuint id=glCreateShader(type);GLint ok;
	glShaderSource(id,1,&src,NULL);glCompileShader(id);glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
	if(!ok){fprintf(stderr,"yamashell app: shader failed\n");glDeleteShader(id);return 0;}
	return id;
}

static void projection(void){
	glUseProgram(a.flat);glUniform2f(glGetUniformLocation(a.flat,"view"),2.f/a.pw,2.f/a.ph);
	glUseProgram(a.program);glUniform2f(glGetUniformLocation(a.program,"view"),2.f/a.pw,2.f/a.ph);
}

static GLuint program(GLuint v,GLuint f){
	GLuint p=glCreateProgram();GLint ok=0;
	glAttachShader(p,v);glAttachShader(p,f);
	glBindAttribLocation(p,0,"pos");glBindAttribLocation(p,1,"uv");
	glBindAttribLocation(p,2,"col");glBindAttribLocation(p,3,"prm");
	glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);
	if(!ok){glDeleteProgram(p);return 0;}
	return p;
}

static bool gl_init(void){
	/* mode p.w: 0 rect (rounded box distance in pixels), 1 text (atlas coverage), 2 picture (premultiplied colour) */
	const char *vs="attribute vec2 pos; attribute vec2 uv; attribute vec4 col; attribute vec4 prm; uniform vec2 view; uniform vec2 ts;"
		"varying vec2 b; varying vec2 t; varying vec4 c; varying vec4 p;"
		"void main(){gl_Position=vec4(pos.x*view.x*.25-1.,1.-pos.y*view.y*.25,0.,1.); b=uv*.25; t=uv*ts; c=col; p=prm*.25;}";
	const char *fs="precision mediump float; uniform sampler2D tex; varying vec2 b; varying vec2 t; varying vec4 c; varying vec4 p;"
		"void main(){vec4 g=texture2D(tex,t); vec2 q=abs(b)-p.xy+p.z; float d=length(max(q,0.))+min(max(q.x,q.y),0.)-p.z;"
		"vec4 a=c*mix(clamp(.5-d,0.,1.),g.a,min(p.w,1.)); gl_FragColor=mix(a,g*c.a,step(1.5,p.w));}";
	const char *flat="precision mediump float; varying vec2 b; varying vec2 t; varying vec4 c; varying vec4 p; void main(){gl_FragColor=c;}";
	GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs),ff=compile(GL_FRAGMENT_SHADER,flat);
	if(v && f && ff){a.program=program(v,f);a.flat=program(v,ff);}
	if(v){glDeleteShader(v);}if(f){glDeleteShader(f);}if(ff){glDeleteShader(ff);}
	if(!a.program || !a.flat){return false;}
	glUseProgram(a.program);
	a.ts=glGetUniformLocation(a.program,"ts");
	glUniform1i(glGetUniformLocation(a.program,"tex"),0);
	projection();
	for(int i=0;i<4;i++){glEnableVertexAttribArray(i);}
	glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_SCISSOR_TEST);
	glGenTextures(1,&a.atlas);glBindTexture(GL_TEXTURE_2D,a.atlas);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,ATLAS,ATLAS,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
	a.bound=0;bind(a.atlas,ATLAS,ATLAS);
	uint16_t *idx=malloc(QUADS*6*sizeof(*idx));
	if(!idx){return false;}
	for(int i=0;i<QUADS;i++){
		uint16_t k=(uint16_t)(i*4),*d=idx+i*6;
		d[0]=k;d[1]=k+1;d[2]=k+2;d[3]=k+1;d[4]=k+3;d[5]=k+2;
	}
	glGenBuffers(1,&a.ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,a.ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,QUADS*6*sizeof(*idx),idx,GL_STATIC_DRAW);
	free(idx);
	glGenBuffers(1,&a.vbo);glBindBuffer(GL_ARRAY_BUFFER,a.vbo);
	glBufferData(GL_ARRAY_BUFFER,sizeof(a.v),NULL,GL_DYNAMIC_DRAW);
	atlas_reset();
	return true;
}

static bool frame(void){
	uint32_t now=app_ms();
	float dt=a.last ? fminf((now-a.last)/1000.f,.1f) : 1/60.f;
	a.moving=app_step(dt);a.last=a.moving ? now : 0;a.redraw=false;
	if(a.resized){wl_egl_window_resize(a.win,a.pw,a.ph,0,0);projection();a.resized=false;}
	glViewport(0,0,a.pw,a.ph);
	/* a cleared tile isn't loaded back from memory first */
	glDisable(GL_SCISSOR_TEST);glClear(GL_COLOR_BUFFER_BIT);glEnable(GL_SCISSOR_TEST);
	clip(0,0,app_w,app_h);
	app_draw();
	flush();
	a.nq=a.first=0;
	return eglSwapBuffers(a.egl,a.es);
}

/* input */
static void key(void *data,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t code,uint32_t state){
	(void)data;(void)k;(void)serial;(void)time;
	if(state==WL_KEYBOARD_KEY_STATE_PRESSED){app_key(code);a.redraw=true;}
}
static void keymap(void *data,struct wl_keyboard *k,uint32_t format,int32_t fd,uint32_t size){(void)data;(void)k;(void)format;(void)size;close(fd);}
/* app may have changed meanwhile */
static void enter(void *data,struct wl_keyboard *k,uint32_t serial,struct wl_surface *v,struct wl_array *keys){(void)data;(void)k;(void)serial;(void)v;(void)keys;a.redraw=true;}
static void leave(void *data,struct wl_keyboard *k,uint32_t serial,struct wl_surface *v){(void)data;(void)k;(void)serial;(void)v;}
static void mods(void *data,struct wl_keyboard *k,uint32_t serial,uint32_t d,uint32_t l,uint32_t lk,uint32_t g){(void)data;(void)k;(void)serial;(void)d;(void)l;(void)lk;(void)g;}
static void repeat(void *data,struct wl_keyboard *k,int32_t rate,int32_t delay){(void)data;(void)k;(void)rate;(void)delay;}
static const struct wl_keyboard_listener keyboard_listener={.keymap=keymap,.enter=enter,.leave=leave,.key=key,.modifiers=mods,.repeat_info=repeat};

static void touch_down(void *data,struct wl_touch *t,uint32_t serial,uint32_t time,struct wl_surface *v,int32_t id,wl_fixed_t x,wl_fixed_t y){
	(void)data;(void)t;(void)serial;(void)time;(void)v;
	if(a.touch>=0){return;}
	a.touch=id;app_touch(T_DOWN,wl_fixed_to_double(x)/app_scale,wl_fixed_to_double(y)/app_scale);a.redraw=true;
}
static void touch_motion(void *data,struct wl_touch *t,uint32_t time,int32_t id,wl_fixed_t x,wl_fixed_t y){
	(void)data;(void)t;(void)time;
	if(id==a.touch){app_touch(T_MOVE,wl_fixed_to_double(x)/app_scale,wl_fixed_to_double(y)/app_scale);a.redraw=true;}
}

static void touch_up(void *data,struct wl_touch *t,uint32_t serial,uint32_t time,int32_t id){
	(void)data;(void)t;(void)serial;(void)time;
	if(id==a.touch){a.touch=-1;app_touch(T_UP,-1,-1);a.redraw=true;}
}

static void touch_frame(void *data,struct wl_touch *t){(void)data;(void)t;}
static void touch_cancel(void *data,struct wl_touch *t){(void)data;(void)t;if(a.touch>=0){a.touch=-1;app_touch(T_UP,-1,-1);a.redraw=true;}}
static const struct wl_touch_listener touch_listener={.down=touch_down,.up=touch_up,.motion=touch_motion,.frame=touch_frame,.cancel=touch_cancel};

static void capabilities(void *data,struct wl_seat *seat,uint32_t caps){
	(void)data;
	if(caps & WL_SEAT_CAPABILITY_KEYBOARD){wl_keyboard_add_listener(wl_seat_get_keyboard(seat),&keyboard_listener,NULL);}
	if(caps & WL_SEAT_CAPABILITY_TOUCH){wl_touch_add_listener(wl_seat_get_touch(seat),&touch_listener,NULL);}
}

static void seat_name(void *data,struct wl_seat *seat,const char *name){(void)data;(void)seat;(void)name;}
static const struct wl_seat_listener seat_listener={.capabilities=capabilities,.name=seat_name};

static void global(void *data,struct wl_registry *r,uint32_t id,const char *iface,uint32_t version){
	(void)data;
	if(!strcmp(iface,"wl_compositor")){a.compositor=wl_registry_bind(r,id,&wl_compositor_interface,4);}
	else if(!strcmp(iface,"xdg_wm_base")){a.wm=wl_registry_bind(r,id,&xdg_wm_base_interface,1);}
	else if(!strcmp(iface,"wl_seat") && !a.seat){a.seat=wl_registry_bind(r,id,&wl_seat_interface,version<5 ? version : 5);wl_seat_add_listener(a.seat,&seat_listener,NULL);}
}

static void global_remove(void *data,struct wl_registry *r,uint32_t id){(void)data;(void)r;(void)id;}
static const struct wl_registry_listener registry_listener={global,global_remove};

static void ping(void *data,struct xdg_wm_base *wm,uint32_t serial){(void)data;xdg_wm_base_pong(wm,serial);}
static const struct xdg_wm_base_listener wm_listener={ping};
static void xdg_configure(void *data,struct xdg_surface *x,uint32_t serial){(void)data;xdg_surface_ack_configure(x,serial);a.configured=true;a.redraw=true;}
static const struct xdg_surface_listener xdg_listener={xdg_configure};
static void top_configure(void *data,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states){
	(void)data;(void)t;(void)states;
	if(w>0 && h>0 && (w!=a.pw || h!=a.ph)){a.pw=w;a.ph=h;a.resized=a.win!=NULL;}
	app_w=(int)lroundf(a.pw/app_scale);app_h=(int)lroundf(a.ph/app_scale);
}

static void top_close(void *data,struct xdg_toplevel *t){(void)data;(void)t;a.quit=true;}
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=top_close};

static FT_Face font(const char *env,const char *path){
	FT_Face f=NULL;
	if(getenv(env)){path=getenv(env);}
	if(FT_New_Face(a.ft,path,0,&f)){fprintf(stderr,"yamashell app: no font %s\n",path);return NULL;}
	return f;
}

static bool egl_init(const char *id){
	static const EGLint cfg_attr[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,5,EGL_GREEN_SIZE,6,EGL_BLUE_SIZE,5,EGL_NONE};
	static const EGLint ctx_attr[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
	EGLConfig cfg;EGLint n=0;
	a.egl=eglGetDisplay((EGLNativeDisplayType)a.display);
	if(a.egl==EGL_NO_DISPLAY || !eglInitialize(a.egl,NULL,NULL) || !eglChooseConfig(a.egl,cfg_attr,&cfg,1,&n) || n<1){fprintf(stderr,"%s: no egl\n",id);return false;}
	a.win=wl_egl_window_create(a.surface,a.pw,a.ph);
	a.ctx=eglCreateContext(a.egl,cfg,EGL_NO_CONTEXT,ctx_attr);
	a.es=eglCreateWindowSurface(a.egl,cfg,(EGLNativeWindowType)a.win,NULL);
	if(a.ctx==EGL_NO_CONTEXT || a.es==EGL_NO_SURFACE || !eglMakeCurrent(a.egl,a.es,a.es,a.ctx)){fprintf(stderr,"%s: no egl surface %#x\n",id,eglGetError());return false;}
	if(!gl_init()){fprintf(stderr,"%s: no gl program\n",id);return false;}
	return true;
}

/* events without blocking, while frames run */
static void pump(void){
	while(wl_display_prepare_read(a.display)){wl_display_dispatch_pending(a.display);}
	wl_display_flush(a.display);
	struct pollfd p={wl_display_get_fd(a.display),POLLIN,0};
	if(poll(&p,1,0)>0){wl_display_read_events(a.display);}
	else{wl_display_cancel_read(a.display);}
	wl_display_dispatch_pending(a.display);
}

int app_run(const char *title,const char *id){
	a.display=wl_display_connect(NULL);
	if(!a.display){fprintf(stderr,"%s: no wayland display\n",id);return 1;}
	struct wl_registry *registry=wl_display_get_registry(a.display);
	wl_registry_add_listener(registry,&registry_listener,NULL);
	wl_display_roundtrip(a.display);
	if(!a.compositor || !a.wm){fprintf(stderr,"%s: compositor lacks xdg-shell\n",id);return 1;}
	if(!FT_Init_FreeType(&a.ft)){
		a.face[F_TEXT]=font("YAMASHELL_FONT",ASSETS "Ubuntu-Regular.ttf");
		a.face[F_LIGHT]=font("YAMASHELL_FONT_LIGHT",ASSETS "Ubuntu-Light.ttf");
		a.face[F_ICON]=font("YAMASHELL_ICONS",ASSETS "MaterialIcons-Regular.ttf");
	}
	xdg_wm_base_add_listener(a.wm,&wm_listener,NULL);
	a.surface=wl_compositor_create_surface(a.compositor);
	struct xdg_surface *xdg=xdg_wm_base_get_xdg_surface(a.wm,a.surface);
	xdg_surface_add_listener(xdg,&xdg_listener,NULL);
	struct xdg_toplevel *top=xdg_surface_get_toplevel(xdg);
	xdg_toplevel_add_listener(top,&top_listener,NULL);
	xdg_toplevel_set_title(top,title);
	xdg_toplevel_set_app_id(top,id);
	wl_surface_commit(a.surface);
	while(!a.configured && wl_display_dispatch(a.display)>=0){}
	int ret=a.configured && egl_init(id) ? 0 : 1;
	/* the swap waits for the compositor's frame callback, that paces the animation */
	while(!ret && !a.quit){
		if(a.redraw || a.moving){
			pump();
			if(!frame()){fprintf(stderr,"%s: swap failed %#x\n",id,eglGetError());ret=1;}
		}
		else if(wl_display_dispatch(a.display)<0){break;}
	}
	if(a.egl!=EGL_NO_DISPLAY){
		while(a.ntex){app_forget(a.tex[0].px);}
		eglMakeCurrent(a.egl,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
		if(a.es!=EGL_NO_SURFACE){eglDestroySurface(a.egl,a.es);}
		if(a.ctx!=EGL_NO_CONTEXT){eglDestroyContext(a.egl,a.ctx);}
		eglTerminate(a.egl);
	}
	if(a.win){wl_egl_window_destroy(a.win);}
	xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(a.surface);
	wl_display_disconnect(a.display);
	for(int i=0;i<F_COUNT;i++){if(a.face[i]){FT_Done_Face(a.face[i]);}}
	if(a.ft){FT_Done_FreeType(a.ft);}
	return ret;
}