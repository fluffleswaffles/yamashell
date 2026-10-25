// SPDX-License-Identifier: GPL-2.0-only
/* This is a symbian+android style-shell. daily reminder: emmgee can suck a fart.
  * home screen, app menu, notification shade and edge gestures. Every layer is
  * one VBO rebuilt only when its content changes. Animation only mobed a layer
  * with a uniform so nothing crosses the bus per frame (except commands). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>
#include <jpeglib.h>
#include "yamashell.h"

#define STATUS 30
#define EDGE 24 /* back zone at the left/right edge */
#define BOTTOM 36 /* home / app menu zone at the bottom edge */
#define BACK_DIST 110 /* finger travel that arms back / home */
#define TASKS_DEAD 90 /* past home, travel that does nothing before running apps */
#define FLICK 700 /* px/s: a quick short swipe counts as a full one */
#define HOLD 500
#define ATLAS_W 512
#define ATLAS_H 1024 /* glyphs in the top half, app icons below */
#define GLYPHS_H 512
#define ICON_SLOTS (ATLAS_W/ICON*(ATLAS_H-GLYPHS_H)/ICON)
#define ASSETS "/usr/share/yamashell/"

enum{L_HOME,L_MENU,L_SHADE,L_STATUS,L_COUNT};
enum{A_NONE,A_LAUNCH,A_MENU,A_TOGGLE,A_CLOSE};
enum{G_NONE,G_SHADE_OPEN,G_SHADE,G_MENU_OPEN,G_MENU,G_HOME,G_BACK_L,G_BACK_R,G_TAP,G_HELD,G_TASKS};
enum{F_TEXT,F_LIGHT,F_ICON,F_COUNT};
struct col{float r,g,b,a;};
struct vtx{float x,y,u,v,r,g,b,a,hw,hh,rad,mode;};
struct layer{GLuint vbo;struct vtx *v;int n,cap,drawn;};
struct item{int layer,act,arg;float x,y,w,h;};
struct glyph{uint32_t cp;int face,size,x,y,w,h,left,top,adv;};
struct toggle{const char *name;uint32_t icon;bool on,na;};
/* critically-ish damped spring: a little overshoot when it lands */
struct spring{float x,v,to;};
struct status{int pct;bool charging,usb,card;};
struct ui{
	GLuint program,atlas,wall;
	GLint off;
	FT_Library ft;FT_Face face[F_COUNT];
	unsigned char *pixels;
	int ax,ay,arow,dirty_y0,dirty_y1;
	struct glyph glyphs[640];int nglyphs;
	struct layer layers[L_COUNT],dynamic,taskl;
	struct item items[96];int nitems;
	struct app *apps;int napps,terminal;
	struct status st;
	bool dirty,home,keys,armed,pull_home,task_hint;
	struct spring shade,menu,pull,tasks,task_pos,hint,app,fly;
	float zoom_x,zoom_y;bool zoom_from; /* where the next app opens from */
	int fly_i;pid_t fly_pid; /* a task card flying off before its app is closed */
	int task_focus,task_lift;float lift;
	float dock[4]; /* the home bar: x, y, w, h; dragging it pulls the app menu */
	int focus,gesture,last_minute,held_key,uevent_fd;
	double x0,y0,x,y,vel;
	uint32_t anim_time,move_time;
	struct wl_event_source *clock,*hold,*press,*child,*poll,*uevent;
};

static struct toggle toggles[]={{"WLAN",0xe648,false,true},{"Bluetooth",0xe1a9,false,true},{"Silent",0xe04f,false,false},{"Torch",0xe3e7,false,false}};
#define NTOGGLES (int)(sizeof(toggles)/sizeof(*toggles))
/* Material Icons code points, system pictograms only: apps bring their own icons */
#define I_USB 0xe1e0
#define I_BATTERY 0xe1a5
#define I_CHARGING 0xe1a3
#define I_CARD 0xe623
#define I_BACK 0xe5c4
#define I_HOME 0xe88a
#define I_TASKS 0xe8eb
#define MAX_TASKS 16

static struct col rgb(uint32_t c,float a){return (struct col){(c>>16&255)/255.f*a,(c>>8&255)/255.f*a,(c&255)/255.f*a,a};}
static struct col white(float a){return (struct col){a,a,a,a};}
static float clampf(float v,float lo,float hi){return v<lo ? lo : v>hi ? hi : v;}

/* glyph atlas */
static uint32_t utf8(const char **p){
	const unsigned char *s=(const unsigned char *)*p;uint32_t c=*s++;
	if(c>=0xf0 && s[0] && s[1] && s[2]){c=(c&7)<<18|(s[0]&63)<<12|(s[1]&63)<<6|(s[2]&63);s+=3;}
	else if(c>=0xe0 && s[0] && s[1]){c=(c&15)<<12|(s[0]&63)<<6|(s[1]&63);s+=2;}
	else if(c>=0xc0 && s[0]){c=(c&31)<<6|(s[0]&63);s++;}
	*p=(const char *)s;return c;
}
static void dirty_rows(struct ui *u,int y0,int y1){
	if(y0<u->dirty_y0){u->dirty_y0=y0;}
	if(y1>u->dirty_y1){u->dirty_y1=y1;}
}
static struct glyph *glyph(struct ui *u,int face,uint32_t cp,int size){
	for(int i=0;i<u->nglyphs;i++){struct glyph *g=&u->glyphs[i];if(g->cp==cp && g->size==size && g->face==face){return g;}}
	FT_Face f=u->face[face];
	if(!f){f=u->face[F_TEXT];}
	if(!f || u->nglyphs==(int)(sizeof(u->glyphs)/sizeof(*u->glyphs))){return NULL;}
	FT_Set_Pixel_Sizes(f,0,size);
	if(FT_Load_Char(f,cp,FT_LOAD_RENDER|FT_LOAD_TARGET_LIGHT)){return NULL;}
	FT_GlyphSlot g=f->glyph;FT_Bitmap *b=&g->bitmap;
	int w=(int)b->width,h=(int)b->rows;
	if(u->ax+w+1>ATLAS_W){u->ax=0;u->ay+=u->arow+1;u->arow=0;}
	if(u->ay+h+1>GLYPHS_H){return NULL;}
	struct glyph *gl=&u->glyphs[u->nglyphs++];
	*gl=(struct glyph){cp,face,size,u->ax,u->ay,w,h,g->bitmap_left,g->bitmap_top,(int)((g->advance.x+32)>>6)};
	for(int y=0;y<h;y++){for(int x=0;x<w;x++){
		unsigned char *d=u->pixels+((size_t)(u->ay+y)*ATLAS_W+u->ax+x)*4;
		d[0]=d[1]=d[2]=255;d[3]=b->buffer[y*b->pitch+x];
	}}
	dirty_rows(u,u->ay,u->ay+h);
	u->ax+=w+1;if(h>u->arow){u->arow=h;}
	return gl;
}

/* geometry */
static void push(struct layer *l,struct vtx v){
	if(l->n==l->cap){l->cap=l->cap ? l->cap*2 : 256;l->v=realloc(l->v,sizeof(*l->v)*l->cap);if(!l->v){abort();}}
	l->v[l->n++]=v;
}

static void rect(struct layer *l,float x,float y,float w,float h,float rad,struct col a,struct col b){
	float hw=w/2,hh=h/2;
	struct vtx tl={x,y,-hw,-hh,a.r,a.g,a.b,a.a,hw,hh,rad,0},tr={x+w,y,hw,-hh,a.r,a.g,a.b,a.a,hw,hh,rad,0};
	struct vtx bl={x,y+h,-hw,hh,b.r,b.g,b.b,b.a,hw,hh,rad,0},br={x+w,y+h,hw,hh,b.r,b.g,b.b,b.a,hw,hh,rad,0};
	push(l,tl);push(l,tr);push(l,bl);push(l,tr);push(l,br);push(l,bl);
}

/* mode 1 = atlas alpha tinted by c (text), 2 = atlas colour faded by c.a (icons) */
static void quad_atlas(struct layer *l,int ax,int ay,int aw,int ah,float x,float y,float w,float h,struct col c,float mode){
	float u0=(float)ax/ATLAS_W,v0=(float)ay/ATLAS_H,u1=(float)(ax+aw)/ATLAS_W,v1=(float)(ay+ah)/ATLAS_H;
	struct vtx tl={x,y,u0,v0,c.r,c.g,c.b,c.a,0,0,0,mode},tr={x+w,y,u1,v0,c.r,c.g,c.b,c.a,0,0,0,mode};
	struct vtx bl={x,y+h,u0,v1,c.r,c.g,c.b,c.a,0,0,0,mode},br={x+w,y+h,u1,v1,c.r,c.g,c.b,c.a,0,0,0,mode};
	push(l,tl);push(l,tr);push(l,bl);push(l,tr);push(l,br);push(l,bl);
}

static void quad_glyph(struct layer *l,struct glyph *g,float gx,float gy,struct col c){quad_atlas(l,g->x,g->y,g->w,g->h,gx,gy,g->w,g->h,c,1);}
static float measure(struct ui *u,int face,const char *s,int size){
	float w=0;
	while(*s){struct glyph *g=glyph(u,face,utf8(&s),size);if(g){w+=g->adv;}}
	return w;
}

/* align: 0 left, 1 centre, 2 right; y is the baseline */
static float text(struct ui *u,struct layer *l,int face,float x,float y,const char *s,int size,struct col c,int align){
	float w=measure(u,face,s,size);
	if(align==1){x-=w/2;}else if(align==2){x-=w;}
	while(*s){
		struct glyph *g=glyph(u,face,utf8(&s),size);
		if(!g){continue;}
		if(g->w && g->h){quad_glyph(l,g,roundf(x)+g->left,roundf(y)-g->top,c);}
		x+=g->adv;
	}
	return w;
}

/* cut to max pixels with an ellipsis */
static void fit_text(struct ui *u,int face,const char *s,int size,float max,char *out,size_t n){
	snprintf(out,n,"%s",s);
	if(measure(u,face,out,size)<=max){return;}
	size_t len=strlen(out);
	while(len>0){
		do{len--;}while(len>0 && ((unsigned char)out[len]&0xc0)==0x80);
		if(len+4>=n){continue;}
		memcpy(out+len,"\xe2\x80\xa6",4);
		if(measure(u,face,out,size)<=max){return;}
	}
}

/* icon glyph centred on (cx, cy) */
static void icon(struct ui *u,struct layer *l,float cx,float cy,uint32_t cp,int size,struct col c){
	struct glyph *g=glyph(u,F_ICON,cp,size);
	if(g && g->w && g->h){quad_glyph(l,g,roundf(cx-g->w/2.f),roundf(cy-g->h/2.f),c);}
}

static void item(struct ui *u,int layer,int act,int arg,float x,float y,float w,float h){
	if(u->nitems<(int)(sizeof(u->items)/sizeof(*u->items))){u->items[u->nitems++]=(struct item){layer,act,arg,x,y,w,h};}
}

static bool running(struct app *a){return a->pid>0 && kill(a->pid,0)==0;}
/* the app's own icon, or its first letter on a tile when it has none */
static void app_icon(struct ui *u,struct layer *l,float x,float y,int i){
	struct app *a=&u->apps[i];
	if(a->icon && i<ICON_SLOTS){
		quad_atlas(l,i%(ATLAS_W/ICON)*ICON,GLYPHS_H+i/(ATLAS_W/ICON)*ICON,ICON,ICON,x,y,ICON,ICON,white(1),2);
	}
	else{
		uint32_t h=2166136261u;
		for(const char *p=a->name;*p;p++){h=(h^(unsigned char)*p)*16777619u;}
		rect(l,x+4,y+4,ICON-8,ICON-8,14,rgb(0x404040|(h&0x3f3f3f)<<1,1),rgb(0x202020|(h&0x3f3f3f),1));
		char first[8]={0};const char *p=a->name;utf8(&p);memcpy(first,a->name,(size_t)(p-a->name));
		text(u,l,F_TEXT,x+ICON/2.f,y+ICON/2.f+11,first,30,white(1),1);
	}
	if(running(a)){rect(l,x+ICON/2.f-3,y+ICON+4,6,6,3,white(.9f),white(.9f));}
}

static void app_cell(struct ui *u,struct layer *l,int layer,int i,float x,float y,float w){
	char name[96];
	app_icon(u,l,x+(w-ICON)/2,y+8,i);
	fit_text(u,F_TEXT,u->apps[i].name,15,w-10,name,sizeof(name));
	text(u,l,F_TEXT,x+w/2,y+8+ICON+28,name,15,white(1),1);
	item(u,layer,A_LAUNCH,i,x+2,y,w-4,ICON+40);
}

/* system state: battery, usb, card */
static struct status read_status(void){
	struct status st={-1,false,false,false};
	char buf[32]={0};
	FILE *f=fopen("/sys/class/power_supply/battery/capacity","r");
	if(f){if(fscanf(f,"%d",&st.pct)!=1){st.pct=-1;}fclose(f);}
	f=fopen("/sys/class/power_supply/battery/status","r");
	if(f){if(!fgets(buf,sizeof(buf),f)){buf[0]=0;}fclose(f);}
	st.charging=!strncmp(buf,"Charging",8);
	int on=0;f=fopen("/sys/class/power_supply/usb/online","r");
	if(f){if(fscanf(f,"%d",&on)!=1){on=0;}fclose(f);}
	st.usb=on;
	st.card=access("/dev/mmcblk0",F_OK)==0;
	return st;
}

static void refresh_status(struct server *s){
	struct ui *u=s->ui;struct status st=read_status();
	if(memcmp(&st,&u->st,sizeof(st))){u->st=st;u->dirty=true;schedule_repaint(s,true);}
}

/* layer contents */
static const char *days[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static void build(struct ui *u){
	struct layer *home=&u->layers[L_HOME],*menu=&u->layers[L_MENU],*shade=&u->layers[L_SHADE],*bar=&u->layers[L_STATUS];
	for(int i=0;i<L_COUNT;i++){u->layers[i].n=0;}
	u->nitems=0;
	time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
	char buf[96],hm[8];struct status st=u->st;
	strftime(hm,sizeof(hm),"%H:%M",&tm);

	/* status bar */
	rect(bar,0,0,screen_w,STATUS,0,(struct col){0,0,0,.7f},(struct col){0,0,0,.7f});
	if(st.usb){icon(u,bar,20,STATUS/2.f,I_USB,20,white(.9f));}
	float rx=screen_w-12-text(u,bar,F_TEXT,screen_w-12,21,hm,17,white(1),2)-12;
	if(st.pct>=0){
		snprintf(buf,sizeof(buf),"%d%%",st.pct);
		rx-=text(u,bar,F_TEXT,rx,21,buf,16,white(.9f),2)+14;
		icon(u,bar,rx+4,STATUS/2.f,st.charging ? I_CHARGING : I_BATTERY,21,white(.9f));
	}

	/* home: clock, date, the terminal, the handle that hints at the app menu */
	int big=(screen_w<screen_h ? screen_w : screen_h)/4;
	float base=STATUS+30+big*.72f;
	text(u,home,F_LIGHT,34,base+2,hm,big,(struct col){0,0,0,.3f},0);
	text(u,home,F_LIGHT,32,base,hm,big,white(1),0);
	snprintf(buf,sizeof(buf),"%s, %d %s",days[tm.tm_wday],tm.tm_mday,months[tm.tm_mon]);
	text(u,home,F_TEXT,35,base+39,buf,22,(struct col){0,0,0,.3f},0);
	text(u,home,F_TEXT,34,base+38,buf,22,white(.95f),0);
	if(u->terminal>=0){
		float cw=120,ch=ICON+48,cx=(screen_w-cw)/2,cy=screen_h-BOTTOM-ch;
		u->dock[0]=cx;u->dock[1]=cy;u->dock[2]=cw;u->dock[3]=ch;
		rect(home,cx,cy,cw,ch,16,(struct col){0,0,0,.42f},(struct col){0,0,0,.5f});
		app_cell(u,home,L_HOME,u->terminal,cx,cy+2,cw);
	}
	rect(home,screen_w/2.f-36,screen_h-14,72,6,3,white(.75f),white(.75f));
	item(u,L_HOME,A_MENU,0,screen_w/2.f-80,screen_h-BOTTOM,160,BOTTOM);

	/* app menu: every installed app. the sheet reaches past the screen so the spring overshoot never shows a gap */
	rect(menu,0,0,screen_w,screen_h*1.25f,0,(struct col){0,0,0,.93f},(struct col){0,0,0,.96f});
	int cols=screen_w>screen_h ? 6 : 4;float mc=(float)screen_w/cols,row=ICON+52;
	/* shortcut: no scrolling, apps past the bottom are cut off; add paging when there are that many */
	for(int i=0;i<u->napps;i++){
		float y=STATUS+16+(i/cols)*row;
		if(y+row>screen_h){break;}
		app_cell(u,menu,L_MENU,i,mc*(i%cols),y,mc);
	}

	if(!u->napps){text(u,menu,F_TEXT,screen_w/2.f,screen_h/2.f,"No apps installed",18,white(.7f),1);}

	/* notification shade */
	rect(shade,0,-screen_h*.25f,screen_w,screen_h*1.25f,0,rgb(0x262626,.97f),rgb(0x0a0a0a,.97f));
	float gw=(screen_w-24)/(float)NTOGGLES,tw=gw-12<88 ? gw-12 : 88;
	for(int i=0;i<NTOGGLES;i++){
		struct toggle *t=&toggles[i];float x=12+gw*i+(gw-tw)/2,y=STATUS+14;float a=t->na ? .42f : 1;
		if(t->on){rect(shade,x,y,tw,60,10,rgb(0x3cb2ff,a),rgb(0x0b72c9,a));}
		else{rect(shade,x,y,tw,60,10,rgb(0x484848,a),rgb(0x1f1f1f,a));}
		icon(u,shade,x+tw/2,y+30,t->icon,32,white(a));
		text(u,shade,F_TEXT,x+tw/2,y+82,t->name,14,white(.75f*a),1);
		item(u,L_SHADE,A_TOGGLE,i,x-4,y-4,tw+8,96);
	}
	float ny=STATUS+130;
	text(u,shade,F_TEXT,16,ny,"Notifications",15,white(.65f),0);
	ny+=12;
	struct{uint32_t icon;const char *title;char sub[96];bool show;}notes[3]={
		{I_USB,"USB connected","Shell on ttyGS0",st.usb},
		{st.charging ? I_CHARGING : I_BATTERY,"Battery","",st.pct>=0},
		{I_CARD,"Memory card","Root filesystem on mmcblk0p2",st.card}};
	if(st.pct>=0){snprintf(notes[1].sub,sizeof(notes[1].sub),"%d%%, %s",st.pct,st.charging ? "charging" : "not charging");}

	for(int i=0;i<3;i++){
		if(!notes[i].show){continue;}
		icon(u,shade,34,ny+34,notes[i].icon,30,rgb(0x3cb2ff,1));
		text(u,shade,F_TEXT,66,ny+30,notes[i].title,19,white(1),0);
		text(u,shade,F_TEXT,66,ny+53,notes[i].sub,15,white(.65f),0);
		rect(shade,12,ny+68,screen_w-24,1,0,white(.12f),white(.12f));
		ny+=70;
	}

	rect(shade,screen_w/2.f-36,screen_h-14,72,6,3,white(.5f),white(.5f));
	item(u,L_SHADE,A_CLOSE,0,screen_w/2.f-80,screen_h-BOTTOM,160,BOTTOM);

	for(int i=0;i<L_COUNT;i++){
		struct layer *l=&u->layers[i];
		if(!l->vbo){glGenBuffers(1,&l->vbo);}
		glBindBuffer(GL_ARRAY_BUFFER,l->vbo);
		glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(sizeof(*l->v)*l->n),l->v,GL_STATIC_DRAW);
		l->drawn=l->n;
	}

	glBindBuffer(GL_ARRAY_BUFFER,0);
	if(u->dirty_y1>u->dirty_y0){
		glBindTexture(GL_TEXTURE_2D,u->atlas);
		glTexSubImage2D(GL_TEXTURE_2D,0,0,u->dirty_y0,ATLAS_W,u->dirty_y1-u->dirty_y0,GL_RGBA,GL_UNSIGNED_BYTE,u->pixels+(size_t)u->dirty_y0*ATLAS_W*4);
		u->dirty_y0=ATLAS_H;u->dirty_y1=0;
	}

	u->last_minute=tm.tm_hour*60+tm.tm_min;
	u->dirty=false;
}

/* wallpaper: RGB565 texture (jpg/png file)*/
struct jpeg_fail{struct jpeg_error_mgr mgr;jmp_buf jump;};
static void jpeg_fail(j_common_ptr c){longjmp(((struct jpeg_fail *)c->err)->jump,1);}

static unsigned char *load_jpeg(FILE *f,int *w,int *h){
	struct jpeg_decompress_struct c;struct jpeg_fail e;unsigned char *volatile rgb=NULL;
	c.err=jpeg_std_error(&e.mgr);e.mgr.error_exit=jpeg_fail;
	if(setjmp(e.jump)){jpeg_destroy_decompress(&c);free(rgb);return NULL;}
	jpeg_create_decompress(&c);jpeg_stdio_src(&c,f);jpeg_read_header(&c,TRUE);
	/* DCT scaling decodes a big photo straight at about screen size */
	c.scale_num=1;c.scale_denom=1;
	while(c.scale_denom<8 && c.image_width/(c.scale_denom*2)>=(unsigned)screen_w && c.image_height/(c.scale_denom*2)>=(unsigned)screen_h){c.scale_denom*=2;}
	c.out_color_space=JCS_RGB;
	jpeg_start_decompress(&c);
	*w=(int)c.output_width;*h=(int)c.output_height;
	rgb=malloc((size_t)*w**h*3);
	if(!rgb){jpeg_destroy_decompress(&c);return NULL;}
	while(c.output_scanline<c.output_height){JSAMPROW row=rgb+(size_t)c.output_scanline**w*3;jpeg_read_scanlines(&c,&row,1);}
	jpeg_finish_decompress(&c);jpeg_destroy_decompress(&c);
	return rgb;
}

static unsigned char *load_png(FILE *f,int *w,int *h){
	png_image img={.version=PNG_IMAGE_VERSION};
	if(!png_image_begin_read_from_stdio(&img,f)){return NULL;}
	img.format=PNG_FORMAT_RGB;
	unsigned char *rgb=malloc(PNG_IMAGE_SIZE(img));
	if(!rgb || !png_image_finish_read(&img,NULL,rgb,0,NULL)){free(rgb);png_image_free(&img);return NULL;}
	*w=(int)img.width;*h=(int)img.height;
	return rgb;
}

static void wallpaper(struct ui *u){
	const char *path=getenv("YAMASHELL_WALLPAPER");
	if(!path){path=ASSETS "wallpaper.jpg";}
	uint16_t *px=calloc((size_t)screen_w*screen_h,2);
	if(!px){return;}
	FILE *f=fopen(path,"rb");
	unsigned char magic[4]={0},*rgb=NULL;int w=0,h=0;
	if(f && fread(magic,1,4,f)==4){
		rewind(f);
		rgb=magic[0]==0xff && magic[1]==0xd8 ? load_jpeg(f,&w,&h) : magic[0]==0x89 && magic[1]=='P' ? load_png(f,&w,&h) : NULL;
	}
	if(f){fclose(f);}
	if(rgb){
		float s=fmaxf((float)screen_w/w,(float)screen_h/h),ox=(w*s-screen_w)/2,oy=(h*s-screen_h)/2;
		for(int y=0;y<screen_h;y++){
			int sy=(int)((y+oy)/s);if(sy>=h){sy=h-1;}
			for(int x=0;x<screen_w;x++){
				int sx=(int)((x+ox)/s);if(sx>=w){sx=w-1;}
				const unsigned char *p=rgb+((size_t)sy*w+sx)*3;
				px[y*screen_w+x]=(uint16_t)((p[0]>>3)<<11|(p[1]>>2)<<5|(p[2]>>3));
			}
		}
		free(rgb);
	}
	else{
		fprintf(stderr,"yamashell: no wallpaper %s, plain background\n",path);
		for(int i=0;i<screen_w*screen_h;i++){px[i]=0x18e5;}
	}
	glGenTextures(1,&u->wall);glBindTexture(GL_TEXTURE_2D,u->wall);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT,2);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,screen_w,screen_h,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,px);
	glPixelStorei(GL_UNPACK_ALIGNMENT,4);
	free(px);
}

/* animation */
static bool spring_step(struct spring *s,float dt,float k){
	if(s->x==s->to && s->v==0){return false;}
	float c=2*.72f*sqrtf(k);
	for(float t=0;t<dt;t+=.004f){
		float h=fminf(.004f,dt-t);
		s->v+=(-k*(s->x-s->to)-c*s->v)*h;s->x+=s->v*h;
	}
	if(fabsf(s->x-s->to)<.002f && fabsf(s->v)<.02f){s->x=s->to;s->v=0;}
	return true;
}
static void spring_to(struct spring *s,float to){s->to=to;}
static bool spring_moving(struct spring *s){return s->x!=s->to || s->v!=0;}
static bool is_open(struct spring *s){return s->to>.5f;}

static float rubber(float x){return x>1 ? 1+(x-1)*.3f : x<0 ? 0 : x;}

static void fling(struct spring *s,float v){
	s->v=v;
	s->to=s->x+v*.12f>.5f ? 1 : 0;
}

/* state */
static struct surface *top_window(struct server *s){
	struct surface *v;
	wl_list_for_each_reverse(v,&s->surfaces,link){if(!v->parent && !v->cursor && v->xdg && surface_visible(v)){return v;}}
	return NULL;
}

static int task_list(struct server *s,struct surface **out){
	struct surface *v;int n=0;
	wl_list_for_each_reverse(v,&s->surfaces,link){if(n<MAX_TASKS && !v->parent && !v->cursor && v->xdg && surface_visible(v)){out[n++]=v;}}
	return n;
}

static void card(struct ui *u,int i,float *x,float *y,float *w,float *h){
	*w=screen_w*(screen_w>screen_h ? .5f : .64f);*h=*w*(screen_h-STATUS)/screen_w;
	*x=screen_w/2.f-*w/2+(i-u->task_pos.x)*(*w+28);
	*y=STATUS+(screen_h-STATUS-*h)/2+14+(1-u->tasks.x)*screen_h;
}

static void open_tasks(struct server *s,bool on){
	struct ui *u=s->ui;struct surface *list[MAX_TASKS];
	int n=task_list(s,list);
	if(on){
		/* like android: from an app the previous one is preselected */
		u->task_focus=n>1 && !u->home ? 1 : 0;
		u->task_pos.x=u->task_pos.to=(float)u->task_focus;u->task_pos.v=0;
		spring_to(&u->menu,0);spring_to(&u->shade,0);
	}
	spring_to(&u->tasks,on ? 1 : 0);
	schedule_repaint(s,true);
}

static void go_home(struct server *s){
	struct ui *u=s->ui;
	u->home=true;spring_to(&u->menu,0);spring_to(&u->shade,0);spring_to(&u->tasks,0);u->focus=-1;
	/* the window shrinks into the home handle */
	u->zoom_x=screen_w/2.f;u->zoom_y=(float)screen_h;spring_to(&u->app,0);
	input_focus(s,NULL);
	schedule_repaint(s,true);
}

static void show_app(struct server *s,struct surface *v){
	struct ui *u=s->ui;
	/* from home the window grows out of what was tapped, or the middle */
	if(u->app.x<.5f && !u->zoom_from){u->zoom_x=screen_w/2.f;u->zoom_y=screen_h/2.f;}
	u->zoom_from=false;
	u->home=false;spring_to(&u->menu,0);spring_to(&u->shade,0);spring_to(&u->tasks,0);spring_to(&u->app,1);
	if(v){surface_focus(v);}
	schedule_repaint(s,true);
}

void ui_window_mapped(struct server *s,struct surface *v){
	if(s->ui){show_app(s,v);}
}

static pid_t window_pid(struct surface *v){
	pid_t pid=0;
	wl_client_get_credentials(wl_resource_get_client(v->resource),&pid,NULL,NULL);
	return pid;
}

/* ask every top-level window of the app to close, like its own close button */
static void close_windows(struct server *s,pid_t pid){
	struct surface *v;
	wl_list_for_each(v,&s->surfaces,link){
		if(!v->parent && v->xdg && v->role && surface_visible(v) && (pid<0 || window_pid(v)==pid)){xdg_toplevel_send_close(v->role);}
	}
}

static const char *task_name(struct ui *u,struct surface *v){
	pid_t pid=window_pid(v);
	for(int i=0;i<u->napps;i++){if(u->apps[i].pid==pid){return u->apps[i].name;}}
	return "Window";
}

/* the card flies up first, the app is asked to close once it is off screen */
static void task_dismiss(struct server *s,int i,float from){
	struct ui *u=s->ui;struct surface *list[MAX_TASKS];
	if(u->fly_i>=0 || i<0 || i>=task_list(s,list)){return;}
	u->fly_i=i;u->fly_pid=window_pid(list[i]);
	u->fly.x=from;u->fly.v=-400;
	schedule_repaint(s,true);
}

static void zoom_from_card(struct ui *u,int i){
	float x,y,w,h;card(u,i,&x,&y,&w,&h);
	u->zoom_x=x+w/2;u->zoom_y=y+h/2;u->zoom_from=true;
}

static void launch(struct server *s,int i){
	struct ui *u=s->ui;struct surface *v;
	if(running(&u->apps[i])){
		wl_list_for_each(v,&s->surfaces,link){
			if(v->parent || !v->xdg || !surface_visible(v)){continue;}
			if(window_pid(v)==u->apps[i].pid){show_app(s,v);return;}
		}
	}
	pid_t pid=fork();
	if(pid==0){
		/* the event loop blocks SIGCHLD/SIGTERM/SIGINT for signalfd; apps must not inherit that */
		sigset_t none;sigemptyset(&none);sigprocmask(SIG_SETMASK,&none,NULL);
		setsid();execl("/bin/sh","sh","-c",u->apps[i].exec,(char *)NULL);_exit(127);
	}
	if(pid>0){u->apps[i].pid=pid;u->dirty=true;}
	spring_to(&u->menu,0);schedule_repaint(s,true);
}

void ui_start_terminal(struct server *s){
	if(s->ui && s->ui->terminal>=0){launch(s,s->ui->terminal);}
}

static int reap(int sig,void *data){
	struct server *s=data;struct ui *u=s->ui;
	(void)sig;
	pid_t pid;
	while((pid=waitpid(-1,NULL,WNOHANG))>0){for(int i=0;i<u->napps;i++){if(u->apps[i].pid==pid){u->apps[i].pid=0;u->dirty=true;}}}
	if(u->dirty){schedule_repaint(s,true);}
	return 0;
}

static int clock_tick(void *data){
	struct server *s=data;struct ui *u=s->ui;
	time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
	if(tm.tm_hour*60+tm.tm_min!=u->last_minute){u->dirty=true;schedule_repaint(s,true);}
	wl_event_source_timer_update(u->clock,(60-tm.tm_sec)*1000+20);
	return 0;
}

/* the battery driver may not send uevents for every percent, so also look now and then */
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

static int active_layer(struct ui *u){
	if(is_open(&u->shade)){return L_SHADE;}
	if(is_open(&u->menu)){return L_MENU;}
	return u->home ? L_HOME : -1;
}

static void layer_offset(struct ui *u,int l,float *x,float *y){
	*x=0;*y=0;
	if(l==L_SHADE){*y=-(1-u->shade.x)*screen_h;}
	if(l==L_MENU){*y=(1-u->menu.x)*screen_h;}
}

static void back(struct server *s){
	struct ui *u=s->ui;
	if(is_open(&u->shade)){spring_to(&u->shade,0);}
	else if(is_open(&u->tasks)){spring_to(&u->tasks,0);}
	else if(is_open(&u->menu)){spring_to(&u->menu,0);}
	/* inside an app back is Esc, the usual linux cancel */
	else if(!u->home){input_key_tap(s,KEY_ESC);}
	schedule_repaint(s,true);
}

static void open_menu(struct server *s,bool on){
	struct ui *u=s->ui;
	if(on && !u->home){u->home=true;input_focus(s,NULL);}
	spring_to(&u->menu,on ? 1 : 0);spring_to(&u->shade,0);spring_to(&u->tasks,0);u->focus=-1;
	schedule_repaint(s,true);
}

static void open_shade(struct server *s,bool on){
	struct ui *u=s->ui;
	spring_to(&u->shade,on ? 1 : 0);u->focus=-1;
	schedule_repaint(s,true);
}

static void activate(struct server *s,struct item *it){
	struct ui *u=s->ui;
	if(it->act==A_LAUNCH){
		float ox,oy;layer_offset(u,it->layer,&ox,&oy);
		u->zoom_x=it->x+ox+it->w/2;u->zoom_y=it->y+oy+ICON/2.f+8;u->zoom_from=true;
		launch(s,it->arg);
	}
	else if(it->act==A_MENU){open_menu(s,true);}
	else if(it->act==A_CLOSE){spring_to(&u->shade,0);}
	else if(it->act==A_TOGGLE && !toggles[it->arg].na){toggles[it->arg].on=!toggles[it->arg].on;u->dirty=true;}
	schedule_repaint(s,true);
}

static struct item *item_at(struct ui *u,double x,double y){
	int l=active_layer(u);
	for(int i=0;i<u->nitems;i++){
		struct item *it=&u->items[i];float ox,oy;layer_offset(u,it->layer,&ox,&oy);
		if(it->layer==l && x>=it->x+ox && y>=it->y+oy && x<it->x+ox+it->w && y<it->y+oy+it->h){return it;}
	}
	return NULL;
}

/* keys. home, messages, end, d-pad (only when the sheell has focus) */
/* long presses: home (running apps), messages (shade), end (close app) */
static int hold_fire(void *data){
	struct server *s=data;struct ui *u=s->ui;int key=u->held_key;
	u->held_key=0;
	if(key==KEY_HOMEPAGE){open_tasks(s,!is_open(&u->tasks));}
	else if(key==KEY_MESSENGER){open_shade(s,!is_open(&u->shade));}
	else if(key==KEY_HANGUP_PHONE && is_open(&u->tasks)){task_dismiss(s,u->task_focus,0);}
	else if(key==KEY_HANGUP_PHONE && !u->home){struct surface *v=top_window(s);if(v){close_windows(s,window_pid(v));}}
	return 0;
}

static void move_focus(struct ui *u,int dx,int dy,struct server *s){
	int l=active_layer(u),best=-1;float bd=1e9f;
	if(l<0){return;}
	if(u->focus<0 || u->items[u->focus].layer!=l){
		if(l==L_HOME && dy<0){open_shade(s,true);}
		else{for(int i=0;i<u->nitems;i++){if(u->items[i].layer==l){u->focus=i;break;}}}
		schedule_repaint(s,true);
		return;
	}

	struct item *a=&u->items[u->focus];float ax=a->x+a->w/2,ay=a->y+a->h/2;
	for(int i=0;i<u->nitems;i++){
		struct item *b=&u->items[i];
		if(i==u->focus || b->layer!=l){continue;}
		float vx=b->x+b->w/2-ax,vy=b->y+b->h/2-ay,along=dx ? vx*dx : vy*dy;
		if(along<=4){continue;}
		float d=along+(dx ? fabsf(vy) : fabsf(vx))*2.2f;
		if(d<bd){bd=d;best=i;}
	}

	if(best>=0){u->focus=best;}
	else if(l==L_HOME && dy<0){open_shade(s,true);}
	else if(l==L_SHADE && dy>0){open_shade(s,false);}
	schedule_repaint(s,true);
}

bool ui_key(struct server *s,uint32_t key,uint32_t state){
	struct ui *u=s->ui;
	if(!u){return false;}
	if(key==KEY_HOMEPAGE || key==KEY_MESSENGER || key==KEY_HANGUP_PHONE){
		if(state){if(!u->held_key){u->held_key=key;wl_event_source_timer_update(u->hold,HOLD);}return true;}
		if(u->held_key!=(int)key){return true;} /* the long press already fired */
		u->held_key=0;wl_event_source_timer_update(u->hold,0);
		if(key==KEY_HOMEPAGE){
			if(!u->home || is_open(&u->shade) || is_open(&u->tasks)){go_home(s);}
			else{open_menu(s,!is_open(&u->menu));}
		}

		else if(key==KEY_MESSENGER){open_menu(s,!is_open(&u->menu));}

		else{back(s);}
		return true;
	}
	if(is_open(&u->tasks)){
		struct surface *list[MAX_TASKS];
		int n=task_list(s,list);
		if(!state){return true;}
		u->keys=true;
		if(key==KEY_LEFT && u->task_focus>0){u->task_focus--;}
		else if(key==KEY_RIGHT && u->task_focus<n-1){u->task_focus++;}
		else if((key==KEY_OK || key==KEY_ENTER) && u->task_focus<n){zoom_from_card(u,u->task_focus);show_app(s,list[u->task_focus]);}

		else if(key==KEY_UP || key==KEY_DELETE){task_dismiss(s,u->task_focus,0);}
		else if(key==KEY_BACKSPACE || key==KEY_ESC){back(s);}
		spring_to(&u->task_pos,(float)u->task_focus);
		schedule_repaint(s,true);
		return true;
	}

	if(active_layer(u)<0){return false;}
	if(!state){return key==KEY_UP || key==KEY_DOWN || key==KEY_LEFT || key==KEY_RIGHT || key==KEY_OK || key==KEY_ENTER;}
	u->keys=true;
	if(key==KEY_UP){move_focus(u,0,-1,s);return true;}
	if(key==KEY_DOWN){move_focus(u,0,1,s);return true;}
	if(key==KEY_LEFT){move_focus(u,-1,0,s);return true;}
	if(key==KEY_RIGHT){move_focus(u,1,0,s);return true;}
	if(key==KEY_OK || key==KEY_ENTER){
		if(u->focus>=0 && u->items[u->focus].layer==active_layer(u)){activate(s,&u->items[u->focus]);}
		else{move_focus(u,0,0,s);}
		return true;
	}

	if(key==KEY_BACKSPACE || key==KEY_ESC){back(s);return true;}
	return u->home;
}

static int press_fire(void *data){
	struct server *s=data;struct ui *u=s->ui;
	if(u->gesture!=G_TAP && u->gesture!=G_MENU && u->gesture!=G_MENU_OPEN){return 0;}
	struct item *it=item_at(u,u->x0,u->y0);
	if(!it || it->act!=A_LAUNCH || !running(&u->apps[it->arg])){return 0;}
	close_windows(s,u->apps[it->arg].pid);
	u->gesture=G_HELD;
	return 0;
}

static void track(struct ui *u,double pos){
	uint32_t now=milliseconds();
	float dt=(now-u->move_time)/1000.f;
	if(dt>.0005f){u->vel=u->vel*.4+((pos-u->x)/dt)*.6;}
	u->move_time=now;u->x=pos;
}

bool ui_touch(struct server *s,int id,double x,double y,int state){
	struct ui *u=s->ui;
	if(!u || (id!=0 && u->gesture==G_NONE)){return false;}
	if(id!=0){return true;}
	if(state==1){
		u->x0=x;u->y0=y;u->y=y;u->keys=false;u->vel=0;u->move_time=milliseconds();u->armed=false;
		if(x<EDGE){u->gesture=G_BACK_L;}
		else if(x>screen_w-EDGE){u->gesture=G_BACK_R;}
		else if(is_open(&u->shade)){u->gesture=G_SHADE;}
		else if(y<STATUS+6){u->gesture=G_SHADE_OPEN;}
		else if(is_open(&u->tasks)){u->gesture=G_TASKS;}
		else if(is_open(&u->menu)){u->gesture=G_MENU;}
		/* the bottom handle is home / running apps everywhere, on home the bar pulls the app menu */
		else if(y>screen_h-BOTTOM){u->gesture=G_HOME;}
		else if(u->home && u->terminal>=0 && x>=u->dock[0] && y>=u->dock[1] && x<u->dock[0]+u->dock[2] && y<u->dock[1]+u->dock[3]){u->gesture=G_MENU_OPEN;}
		else if(u->home){u->gesture=G_TAP;}
		else{return false;}
		u->x=u->gesture==G_SHADE_OPEN || u->gesture==G_SHADE ? u->shade.x : u->gesture==G_MENU_OPEN || u->gesture==G_MENU ? u->menu.x : 0;
		u->pull_home=u->gesture==G_HOME;u->task_hint=false;u->task_lift=-1;u->lift=0;
		if(u->gesture==G_TASKS){
			struct surface *list[MAX_TASKS];int n=task_list(s,list);
			for(int i=0;i<n;i++){float cx,cy,cw,ch;card(u,i,&cx,&cy,&cw,&ch);if(x>=cx && x<cx+cw && y>=cy && y<cy+ch){u->task_lift=i;}}
		}
		if(u->gesture==G_TAP || u->gesture==G_MENU || u->gesture==G_MENU_OPEN){wl_event_source_timer_update(u->press,HOLD);}
		return true;
	}
	if(u->gesture==G_NONE){return false;}
	double dx=x-u->x0,dy=y-u->y0;
	bool moved=fabs(dx)>10 || fabs(dy)>10;
	if(moved){wl_event_source_timer_update(u->press,0);}
	if(u->gesture==G_HELD){if(state==0){u->gesture=G_NONE;}return true;}
	if(state==2){
		u->y=y;
		if(u->gesture==G_SHADE_OPEN && moved){track(u,y/screen_h);u->shade.x=u->shade.to=rubber((float)u->x);u->shade.v=0;}
		if(u->gesture==G_SHADE && dy<-10){track(u,1+dy/screen_h);u->shade.x=u->shade.to=rubber((float)u->x);u->shade.v=0;}
		if(u->gesture==G_MENU_OPEN && moved){track(u,-dy/screen_h);u->menu.x=u->menu.to=rubber((float)u->x);u->menu.v=0;}
		if(u->gesture==G_MENU && dy>10){track(u,1-dy/screen_h);u->menu.x=u->menu.to=rubber((float)u->x);u->menu.v=0;}
		/* back / home: hard to pull out, then it pops once the travel arms it */
		if(u->gesture==G_BACK_L || u->gesture==G_BACK_R || u->gesture==G_HOME){
			float travel=(float)(u->gesture==G_HOME ? -dy : u->gesture==G_BACK_L ? dx : -dx),p=travel/BACK_DIST;
			track(u,travel);
			/* home, then a dead zone, then running apps; a little back off on the way down */
			if(u->gesture==G_HOME){
				if(!u->task_hint && travel>BACK_DIST+TASKS_DEAD){u->task_hint=true;}
				else if(u->task_hint && travel<BACK_DIST+TASKS_DEAD-30){u->task_hint=false;}
				spring_to(&u->hint,u->task_hint ? 1 : 0);
			}
			if(!u->armed && p>=1){u->armed=true;}
			else if(u->armed && p<.8f){u->armed=false;}
			spring_to(&u->pull,u->armed ? 1 : .6f*clampf(p,0,1));
		}
		if(u->gesture==G_TASKS && moved){
			float cw,cx,cy,ch;card(u,0,&cx,&cy,&cw,&ch);
			if(fabs(dx)>fabs(dy)){u->task_pos.x=u->task_pos.to=u->task_focus-(float)dx/(cw+28);u->task_pos.v=0;u->lift=0;}
			else if(u->task_lift>=0){u->lift=(float)fmin(0,dy);}
		}
		schedule_repaint(s,true);
		return true;
	}

	int g=u->gesture;u->gesture=G_NONE;
	wl_event_source_timer_update(u->press,0);
	if(g==G_TASKS){
		struct surface *list[MAX_TASKS];int n=task_list(s,list);
		if(!moved){
			if(u->task_lift>=0){zoom_from_card(u,u->task_lift);show_app(s,list[u->task_lift]);}
			else if(top_window(s) && !u->home){show_app(s,top_window(s));}
			else{open_tasks(s,false);}
		}else if(fabs(dx)>fabs(dy)){
			u->task_focus=(int)lroundf(clampf(u->task_pos.x,0,(float)(n>0 ? n-1 : 0)));
			spring_to(&u->task_pos,(float)u->task_focus);
		}else if(dy<-80 && u->task_lift>=0){task_dismiss(s,u->task_lift,u->lift);}
		u->lift=0;
	}

	else if(g==G_BACK_L || g==G_BACK_R || g==G_HOME){
		bool flick=u->vel>FLICK && (g==G_HOME ? -dy : g==G_BACK_L ? dx : -dx)>30;
		if(g==G_HOME && u->task_hint){open_tasks(s,true);}
		else if(u->armed || flick){if(g==G_HOME){go_home(s);}else{back(s);}}
		/* a tap on the home handle opens the app menu */
		else if(g==G_HOME && !moved && u->home){struct item *it=item_at(u,x,y);if(it){activate(s,it);}}
		u->armed=false;u->task_hint=false;spring_to(&u->pull,0);spring_to(&u->hint,0);
	}

	else if((g==G_SHADE_OPEN || g==G_SHADE) && moved){fling(&u->shade,(float)u->vel);}
	else if(g==G_SHADE_OPEN){open_shade(s,true);}
	else if((g==G_MENU_OPEN || g==G_MENU) && moved){fling(&u->menu,(float)u->vel);}
	else if(!moved){
		struct item *it=item_at(u,x,y);
		if(it){u->focus=(int)(it-u->items);activate(s,it);}
	}
	schedule_repaint(s,true);
	return true;
}

/* drawing */
/* base is a client array, or NULL for offsets into the bound VBO */
static void attribs(const struct vtx *base){
	uintptr_t p=(uintptr_t)base;
	glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(struct vtx),(const void *)(p+offsetof(struct vtx,x)));
	glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(struct vtx),(const void *)(p+offsetof(struct vtx,u)));
	glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(struct vtx),(const void *)(p+offsetof(struct vtx,r)));
	glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,sizeof(struct vtx),(const void *)(p+offsetof(struct vtx,hw)));
}

static void draw_layer(struct ui *u,int l,float x,float y){
	struct layer *ly=&u->layers[l];
	if(!ly->drawn){return;}
	glUniform2f(u->off,x,y);
	glBindBuffer(GL_ARRAY_BUFFER,ly->vbo);
	attribs(NULL);
	glDrawArrays(GL_TRIANGLES,0,ly->drawn);
	glBindBuffer(GL_ARRAY_BUFFER,0);
}

static void draw_dynamic(struct ui *u,struct layer *l){
	if(!l->n){return;}
	glUniform2f(u->off,0,0);
	if(!l->vbo){glGenBuffers(1,&l->vbo);}
	glBindBuffer(GL_ARRAY_BUFFER,l->vbo);
	glBufferData(GL_ARRAY_BUFFER,sizeof(*l->v)*l->n,l->v,GL_STREAM_DRAW);
	attribs(NULL);
	glDrawArrays(GL_TRIANGLES,0,l->n);
	glBindBuffer(GL_ARRAY_BUFFER,0);
}

static void begin(struct server *s){
	struct ui *u=s->ui;
	glUseProgram(u->program);
	glEnableVertexAttribArray(2);glEnableVertexAttribArray(3);
	glBindTexture(GL_TEXTURE_2D,u->atlas);
}

static void end(struct server *s){
	glDisableVertexAttribArray(2);glDisableVertexAttribArray(3);
	glUseProgram(s->program);
}

bool ui_animating(struct server *s){
	struct ui *u=s->ui;
	if(!u){return false;}
	bool sheet=u->gesture!=G_SHADE_OPEN && u->gesture!=G_SHADE && u->gesture!=G_MENU_OPEN && u->gesture!=G_MENU;
	/* new glyphs drawn this frame reach the atlas on the next one */
	return u->dirty || (sheet && (spring_moving(&u->shade) || spring_moving(&u->menu))) || spring_moving(&u->pull) || spring_moving(&u->tasks) || spring_moving(&u->task_pos) || spring_moving(&u->hint) || spring_moving(&u->app) || u->fly_i>=0;
}
/* before the windows: wallpaper and home; returns whether app windows are drawn */
bool ui_draw_under(struct server *s){
	struct ui *u=s->ui;
	if(!u){return true;}
	uint32_t now=milliseconds();float dt=u->anim_time ? (now-u->anim_time)/1000.f : 0;
	if(dt>.1f){dt=.1f;}
	u->anim_time=now;
	if(u->gesture!=G_SHADE_OPEN && u->gesture!=G_SHADE){spring_step(&u->shade,dt,260);}
	if(u->gesture!=G_MENU_OPEN && u->gesture!=G_MENU){spring_step(&u->menu,dt,260);}
	spring_step(&u->pull,dt,u->armed ? 700 : 420);
	spring_step(&u->tasks,dt,260);
	spring_step(&u->hint,dt,700);
	if(u->gesture!=G_TASKS){spring_step(&u->task_pos,dt,400);}
	spring_step(&u->app,dt,300);
	if(u->fly_i>=0){
		/* thrown up: speeds up until it is off screen, about 0.3 s */
		u->fly.v-=5000*dt;u->fly.x+=u->fly.v*dt;
		if(u->fly.x<-screen_h*.9f){close_windows(s,u->fly_pid);u->fly_i=-1;u->fly=(struct spring){0,0,0};}
	}
	if(!u->home && !top_window(s)){u->home=true;spring_to(&u->app,0);u->app.x=0;u->app.v=0;input_focus(s,NULL);}
	if(u->dirty){build(u);}
	s->zoom=u->app.x>0 ? u->app.x : 0;s->zoom_x=u->zoom_x;s->zoom_y=u->zoom_y;
	/* a settled app covers the screen: no wallpaper or home behind it */
	if(!u->home && !spring_moving(&u->app)){s->zoom=1;return true;}
	glBindTexture(GL_TEXTURE_2D,u->wall);
	glUniform1f(s->alpha,1.f);glUniform1f(s->native,1.f);
	renderer_quad(s,-1,1,1,-1,true);
	begin(s);
	draw_layer(u,L_HOME,0,0);
	end(s);
	return u->app.x>.01f;
}

/* android-style back bubble: slides in a little, follows the finger only a bit up and down */
static void draw_pull(struct ui *u,struct layer *dyn){
	float b=u->pull.x;
	if(b<=.01f){return;}
	bool home=u->pull_home;
	float size=(36+20*clampf(b,0,1))*(home ? 1+.3f*u->hint.x : 1),a=clampf(b*2,0,1),x,y;
	if(home){x=screen_w/2.f-size/2;y=screen_h-b*90;}
	else{
		float cy=(float)u->y0+clampf((float)(u->y-u->y0)*.25f,-30,30);
		y=cy-size/2;
		x=u->x0<screen_w/2.f ? -size+b*(size+20) : screen_w-b*(size+20);
	}
	rect(dyn,x,y,size,size,size/2,(struct col){.1f*a,.1f*a,.1f*a,.82f*a},(struct col){.1f*a,.1f*a,.1f*a,.82f*a});
	float ia=clampf((b-.3f)/.4f,0,1);
	if(ia>0){icon(u,dyn,x+size/2,y+size/2,home ? (u->hint.x>.5f ? I_TASKS : I_HOME) : I_BACK,(int)(size*.6f),white(ia));}
}

/* running apps: dark sheet, one card per window with its live texture */
static void draw_tasks(struct server *s){
	struct ui *u=s->ui;struct layer *l=&u->taskl;struct surface *list[MAX_TASKS];
	int n=task_list(s,list);
	char name[96];
	if(u->task_focus>=n){u->task_focus=n>0 ? n-1 : 0;spring_to(&u->task_pos,(float)u->task_focus);}
	l->n=0;
	float off=(1-u->tasks.x)*screen_h;
	rect(l,0,off,screen_w,screen_h*1.25f,0,(struct col){0,0,0,.92f},(struct col){0,0,0,.96f});
	if(!n){text(u,l,F_TEXT,screen_w/2.f,off+screen_h/2.f,"No running apps",18,white(.7f),1);}
	for(int i=0;i<n;i++){
		float x,y,w,h;card(u,i,&x,&y,&w,&h);
		if(i==u->task_lift){y+=u->lift;}
		if(i==u->fly_i){y+=u->fly.x;}
		if(u->keys && i==u->task_focus){rect(l,x-5,y-5,w+10,h+10,14,rgb(0x3cb2ff,.9f),rgb(0x0b72c9,.9f));}
		rect(l,x-1,y-1,w+2,h+2,10,white(.25f),white(.25f));
		fit_text(u,F_TEXT,task_name(u,list[i]),16,w,name,sizeof(name));
		text(u,l,F_TEXT,x+w/2,y-12,name,16,white(.9f),1);
	}
	draw_dynamic(u,l);
	end(s);
	for(int i=0;i<n;i++){
		float x,y,w,h;card(u,i,&x,&y,&w,&h);
		if(i==u->task_lift){y+=u->lift;}
		if(i==u->fly_i){y+=u->fly.x;}
		if(x+w>0 && x<screen_w){renderer_thumb(list[i],x,y,w,h);}
	}
	begin(s);
}

void ui_draw_over(struct server *s){
	struct ui *u=s->ui;
	if(!u){return;}
	begin(s);
	if(u->menu.x>0){draw_layer(u,L_MENU,0,(1-u->menu.x)*screen_h);}
	if(u->tasks.x>0){draw_tasks(s);}
	if(u->shade.x>0){draw_layer(u,L_SHADE,0,-(1-u->shade.x)*screen_h);}
	draw_layer(u,L_STATUS,0,0);
	struct layer *dyn=&u->dynamic;dyn->n=0;
	int l=active_layer(u);
	if(u->keys && !is_open(&u->tasks) && u->focus>=0 && u->items[u->focus].layer==l){
		struct item *it=&u->items[u->focus];float ox,oy;layer_offset(u,l,&ox,&oy);
		rect(dyn,it->x+ox,it->y+oy,it->w,it->h,10,(struct col){.08f,.3f,.5f,.35f},(struct col){.03f,.2f,.36f,.35f});
	}
	draw_pull(u,dyn);
	if(u->dirty_y1>u->dirty_y0){u->dirty=true;}
	draw_dynamic(u,dyn);
	end(s);
}

/* setup */
static GLuint compile(GLenum type,const char *src){
	GLuint id=glCreateShader(type);GLint ok;
	glShaderSource(id,1,&src,NULL);glCompileShader(id);glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
	if(!ok){fprintf(stderr,"yamashell: ui shader failed\n");glDeleteShader(id);return 0;}
	return id;
}

static FT_Face font(struct ui *u,const char *env,const char *path){
	FT_Face f=NULL;
	if(getenv(env)){path=getenv(env);}
	if(FT_New_Face(u->ft,path,0,&f)){fprintf(stderr,"yamashell: no font %s\n",path);return NULL;}
	return f;
}

static void load_apps(struct ui *u){
	u->napps=apps_load(&u->apps);u->terminal=-1;
	for(int i=0;i<u->napps;i++){
		struct app *a=&u->apps[i];
		if(a->terminal && u->terminal<0){u->terminal=i;}
		if(!a->icon || i>=ICON_SLOTS){continue;}
		int ax=i%(ATLAS_W/ICON)*ICON,ay=GLYPHS_H+i/(ATLAS_W/ICON)*ICON;
		for(int y=0;y<ICON;y++){memcpy(u->pixels+((size_t)(ay+y)*ATLAS_W+ax)*4,a->icon+(size_t)y*ICON*4,ICON*4);}
		dirty_rows(u,ay,ay+ICON);
	}
	fprintf(stderr,"yamashell: %d apps\n",u->napps);
}

void ui_init(struct server *s){
	struct ui *u=calloc(1,sizeof(*u));
	if(!u){return;}
	char vs[640];
	snprintf(vs,sizeof(vs),"attribute vec2 pos; attribute vec2 uv; attribute vec4 col; attribute vec4 prm; uniform vec2 off;"
		"varying vec2 t; varying vec4 c; varying vec4 p;"
		"void main(){vec2 q=pos+off; gl_Position=vec4(q.x*%f-1.,1.-q.y*%f,0.,1.); t=uv; c=col; p=prm;}",2.0/screen_w,2.0/screen_h);
	/* mode p.w: 0 rect (rounded box distance in pixels), 1 text (atlas coverage), 2 icon (atlas colour) */
	const char *fs="precision mediump float; uniform sampler2D tex; varying vec2 t; varying vec4 c; varying vec4 p;"
		"void main(){vec4 g=texture2D(tex,t); vec2 q=abs(t)-p.xy+p.z; float d=length(max(q,0.))+min(max(q.x,q.y),0.)-p.z;"
		"vec4 a=c*mix(clamp(.5-d,0.,1.),g.a,min(p.w,1.)); gl_FragColor=mix(a,g*c.a,step(1.5,p.w));}";
	GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs);GLint ok=0;
	if(v && f){
		u->program=glCreateProgram();glAttachShader(u->program,v);glAttachShader(u->program,f);
		glBindAttribLocation(u->program,0,"pos");glBindAttribLocation(u->program,1,"uv");
		glBindAttribLocation(u->program,2,"col");glBindAttribLocation(u->program,3,"prm");
		glLinkProgram(u->program);glGetProgramiv(u->program,GL_LINK_STATUS,&ok);
	}

	if(v){glDeleteShader(v);}if(f){glDeleteShader(f);}
	if(!ok){fprintf(stderr,"yamashell: ui disabled (shader)\n");if(u->program){glDeleteProgram(u->program);}free(u);return;}
	glUseProgram(u->program);
	u->off=glGetUniformLocation(u->program,"off");
	glUniform1i(glGetUniformLocation(u->program,"tex"),0);
	glUseProgram(s->program);
	if(FT_Init_FreeType(&u->ft)){u->ft=NULL;}
	else{
		u->face[F_TEXT]=font(u,"YAMASHELL_FONT",ASSETS "Ubuntu-Regular.ttf");
		u->face[F_LIGHT]=font(u,"YAMASHELL_FONT_LIGHT",ASSETS "Ubuntu-Light.ttf");
		u->face[F_ICON]=font(u,"YAMASHELL_ICONS",ASSETS "MaterialIcons-Regular.ttf");
	}

	u->pixels=calloc((size_t)ATLAS_W*ATLAS_H,4);
	if(!u->pixels){abort();}
	u->dirty_y0=ATLAS_H;
	glGenTextures(1,&u->atlas);glBindTexture(GL_TEXTURE_2D,u->atlas);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,ATLAS_W,ATLAS_H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
	wallpaper(u);
	load_apps(u);
	u->home=true;u->focus=-1;u->fly_i=-1;u->st=read_status();
	/* items exist before the first frame, a tap may come first */
	build(u);
	s->ui=u;s->inset=STATUS;
	u->clock=wl_event_loop_add_timer(s->loop,clock_tick,s);clock_tick(s);
	u->hold=wl_event_loop_add_timer(s->loop,hold_fire,s);
	u->press=wl_event_loop_add_timer(s->loop,press_fire,s);
	u->child=wl_event_loop_add_signal(s->loop,SIGCHLD,reap,s);
	u->poll=wl_event_loop_add_timer(s->loop,status_poll,s);wl_event_source_timer_update(u->poll,10000);
	u->uevent_fd=uevent_open();
	if(u->uevent_fd>=0){u->uevent=wl_event_loop_add_fd(s->loop,u->uevent_fd,WL_EVENT_READABLE,uevent,s);}
	if(glGetError()!=GL_NO_ERROR){fprintf(stderr,"yamashell: ui setup GL error\n");}
}

void ui_finish(struct server *s){
	struct ui *u=s->ui;
	if(!u){return;}
	wl_event_source_remove(u->clock);wl_event_source_remove(u->hold);wl_event_source_remove(u->press);
	wl_event_source_remove(u->child);wl_event_source_remove(u->poll);
	if(u->uevent){wl_event_source_remove(u->uevent);}
	if(u->uevent_fd>=0){close(u->uevent_fd);}
	for(int i=0;i<L_COUNT;i++){if(u->layers[i].vbo){glDeleteBuffers(1,&u->layers[i].vbo);}free(u->layers[i].v);}
	if(u->dynamic.vbo){glDeleteBuffers(1,&u->dynamic.vbo);}free(u->dynamic.v);
	if(u->taskl.vbo){glDeleteBuffers(1,&u->taskl.vbo);}free(u->taskl.v);
	glDeleteTextures(1,&u->atlas);glDeleteTextures(1,&u->wall);glDeleteProgram(u->program);
	for(int i=0;i<F_COUNT;i++){if(u->face[i]){FT_Done_Face(u->face[i]);}}
	if(u->ft){FT_Done_FreeType(u->ft);}
	apps_free(u->apps,u->napps);
	free(u->pixels);free(u);s->ui=NULL;s->inset=0;
}