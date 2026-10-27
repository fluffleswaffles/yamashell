// SPDX-License-Identifier: GPL-2.0-only
/* the ui's drawing parts: glyph atlas, text, rounded rects, the wallpaper texture */
#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "image.h"
#include "config.h"

/* glyph atlas */
uint32_t utf8(const char **p){
	const unsigned char *s=(const unsigned char *)*p;uint32_t c=*s++;
	if(c>=0xf0 && s[0] && s[1] && s[2]){c=(c&7)<<18|(s[0]&63)<<12|(s[1]&63)<<6|(s[2]&63);s+=3;}
	else if(c>=0xe0 && s[0] && s[1]){c=(c&15)<<12|(s[0]&63)<<6|(s[1]&63);s+=2;}
	else if(c>=0xc0 && s[0]){c=(c&31)<<6|(s[0]&63);s++;}
	*p=(const char *)s;return c;
}
void dirty_rows(struct ui *u,int y0,int y1){
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

void rect(struct layer *l,float x,float y,float w,float h,float rad,struct col a,struct col b){
	float hw=w/2,hh=h/2;
	struct vtx tl={x,y,-hw,-hh,a.r,a.g,a.b,a.a,hw,hh,rad,0},tr={x+w,y,hw,-hh,a.r,a.g,a.b,a.a,hw,hh,rad,0};
	struct vtx bl={x,y+h,-hw,hh,b.r,b.g,b.b,b.a,hw,hh,rad,0},br={x+w,y+h,hw,hh,b.r,b.g,b.b,b.a,hw,hh,rad,0};
	push(l,tl);push(l,tr);push(l,bl);push(l,tr);push(l,br);push(l,bl);
}

/* mode 1 = atlas alpha tinted by c (text), 2 = atlas colour faded by c.a (icons) */
void quad_atlas(struct layer *l,int ax,int ay,int aw,int ah,float x,float y,float w,float h,struct col c,float mode){
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
float text(struct ui *u,struct layer *l,int face,float x,float y,const char *s,int size,struct col c,int align){
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
void fit_text(struct ui *u,int face,const char *s,int size,float max,char *out,size_t n){
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
void icon(struct ui *u,struct layer *l,float cx,float cy,uint32_t cp,int size,struct col c){
	struct glyph *g=glyph(u,F_ICON,cp,size);
	if(g && g->w && g->h){quad_glyph(l,g,roundf(cx-g->w/2.f),roundf(cy-g->h/2.f),c);}
}

void item(struct ui *u,int layer,int act,int arg,float x,float y,float w,float h){
	if(u->nitems<(int)(sizeof(u->items)/sizeof(*u->items))){u->items[u->nitems++]=(struct item){layer,act,arg,x,y,w,h};}
}

/* wallpaper: RGB565 texture */
void wallpaper(struct ui *u){
	char path[300];unsigned char *rgb=NULL;
	if(!config_get("wallpaper",path,sizeof(path)) || !(rgb=image_load(path,screen_w,screen_h))){
		snprintf(path,sizeof(path),"%s",wallpaper_default());
		rgb=image_load(path,screen_w,screen_h);
	}
	uint16_t *px=calloc((size_t)screen_w*screen_h,2);
	if(!px){free(rgb);return;}
	if(rgb){
		for(int i=0;i<screen_w*screen_h;i++){const unsigned char *p=rgb+(size_t)i*3;px[i]=(uint16_t)((p[0]>>3)<<11|(p[1]>>2)<<5|(p[2]>>3));}
		free(rgb);
	}
	else{
		fprintf(stderr,"yamashell: no wallpaper %s, plain background\n",path);
		for(int i=0;i<screen_w*screen_h;i++){px[i]=0x18e5;}
	}
	if(u->wall){glDeleteTextures(1,&u->wall);}
	glGenTextures(1,&u->wall);glBindTexture(GL_TEXTURE_2D,u->wall);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT,2);
	glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,screen_w,screen_h,0,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,px);
	glPixelStorei(GL_UNPACK_ALIGNMENT,4);
	free(px);
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

void draw_layer(struct ui *u,int l,float x,float y){
	struct layer *ly=&u->layers[l];
	if(!ly->drawn){return;}
	glUniform2f(u->off,x,y);
	glBindBuffer(GL_ARRAY_BUFFER,ly->vbo);
	attribs(NULL);
	glDrawArrays(GL_TRIANGLES,0,ly->drawn);
	glBindBuffer(GL_ARRAY_BUFFER,0);
}

void draw_dynamic(struct ui *u,struct layer *l){
	if(!l->n){return;}
	glUniform2f(u->off,0,0);
	if(!l->vbo){glGenBuffers(1,&l->vbo);}
	glBindBuffer(GL_ARRAY_BUFFER,l->vbo);
	glBufferData(GL_ARRAY_BUFFER,sizeof(*l->v)*l->n,l->v,GL_STREAM_DRAW);
	attribs(NULL);
	glDrawArrays(GL_TRIANGLES,0,l->n);
	glBindBuffer(GL_ARRAY_BUFFER,0);
}

void begin(struct server *s){
	struct ui *u=s->ui;
	glUseProgram(u->program);
	glEnableVertexAttribArray(2);glEnableVertexAttribArray(3);
	glBindTexture(GL_TEXTURE_2D,u->atlas);
}

void end(struct server *s){
	glDisableVertexAttribArray(2);glDisableVertexAttribArray(3);
	glUseProgram(s->program);
}