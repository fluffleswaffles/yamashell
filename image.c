// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <math.h>
#include <png.h>
#include <jpeglib.h>
#include "image.h"

struct jpeg_fail{struct jpeg_error_mgr mgr;jmp_buf jump;};
static void jpeg_fail(j_common_ptr c){longjmp(((struct jpeg_fail *)c->err)->jump,1);}

static unsigned char *load_jpeg(FILE *f,int *w,int *h,int want_w,int want_h){
	struct jpeg_decompress_struct c;struct jpeg_fail e;unsigned char *volatile rgb=NULL;
	c.err=jpeg_std_error(&e.mgr);e.mgr.error_exit=jpeg_fail;
	if(setjmp(e.jump)){jpeg_destroy_decompress(&c);free(rgb);return NULL;}
	jpeg_create_decompress(&c);jpeg_stdio_src(&c,f);jpeg_read_header(&c,TRUE);
	/* DCT scaling decodes  straight at about the wanted size */
	c.scale_num=1;c.scale_denom=1;
	while(c.scale_denom<8 && c.image_width/(c.scale_denom*2)>=(unsigned)want_w && c.image_height/(c.scale_denom*2)>=(unsigned)want_h){c.scale_denom*=2;}
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

unsigned char *image_load(const char *path,int ww,int wh){
	FILE *f=fopen(path,"rb");
	unsigned char magic[4]={0},*rgb=NULL,*out;int w=0,h=0;
	if(f && fread(magic,1,4,f)==4){
		rewind(f);
		rgb=magic[0]==0xff && magic[1]==0xd8 ? load_jpeg(f,&w,&h,ww,wh) : magic[0]==0x89 && magic[1]=='P' ? load_png(f,&w,&h) : NULL;
	}
	if(f){fclose(f);}
	if(!rgb || !(out=malloc((size_t)ww*wh*3))){free(rgb);return NULL;}
	float s=fmaxf((float)ww/w,(float)wh/h),ox=(w*s-ww)/2,oy=(h*s-wh)/2;
	for(int y=0;y<wh;y++){
		int sy=(int)((y+oy)/s);if(sy>=h){sy=h-1;}
		for(int x=0;x<ww;x++){
			int sx=(int)((x+ox)/s);if(sx>=w){sx=w-1;}
			const unsigned char *p=rgb+((size_t)sy*w+sx)*3;unsigned char *d=out+((size_t)y*ww+x)*3;
			d[0]=p[0];d[1]=p[1];d[2]=p[2];
		}
	}
	free(rgb);
	return out;
}

const char *wallpaper_default(void){
	const char *p=getenv("YAMASHELL_WALLPAPER");
	return p && *p ? p : "/usr/share/yamashell/wallpaper.jpg";
}
