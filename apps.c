// SPDX-License-Identifier: GPL-2.0-only
/* installed apps from .desktop files (XDG desktop entry spec, the parts that
 * matter here) with their own PNG or SVG icons scaled to ICON pixels */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <png.h>
#include "yamashell.h"
/* THIS IS THIRD PARTY!!!!! */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvgrast.h"
#pragma GCC diagnostic pop

#define ASSETS "/usr/share/yamashell/"

static int data_dirs(char dirs[][256],int max){
	const char *env=getenv("XDG_DATA_DIRS");
	char list[1024];
	int n=0;
	snprintf(dirs[n++],256,"%s",ASSETS);
	snprintf(list,sizeof(list),"%s",env && *env ? env : "/usr/local/share:/usr/share");
	for(char *save=NULL,*d=strtok_r(list,":",&save);d && n<max;d=strtok_r(NULL,":",&save)){snprintf(dirs[n++],256,"%s/",d);}
	return n;
}

static unsigned char *load_png(const char *path,int *w,int *h){
	png_image img={.version=PNG_IMAGE_VERSION};
	if(!png_image_begin_read_from_file(&img,path)){return NULL;}
	img.format=PNG_FORMAT_RGBA;
	unsigned char *px=malloc(PNG_IMAGE_SIZE(img));
	if(!px || !png_image_finish_read(&img,NULL,px,0,NULL)){free(px);png_image_free(&img);return NULL;}
	*w=(int)img.width;*h=(int)img.height;
	return px;
}

static unsigned char *load_svg(const char *path,int size,int *w,int *h){
	NSVGimage *img=nsvgParseFromFile(path,"px",96);
	if(!img){return NULL;}
	float big=img->width>img->height ? img->width : img->height,s=big>0 ? size/big : 0;
	NSVGrasterizer *r=s>0 ? nsvgCreateRasterizer() : NULL;
	unsigned char *px=r ? calloc((size_t)size*size,4) : NULL;
	if(px){nsvgRasterize(r,img,(size-img->width*s)/2,(size-img->height*s)/2,s,px,size,size,size*4);*w=*h=size;}
	if(r){nsvgDeleteRasterizer(r);}
	nsvgDelete(img);
	return px;
}

static bool is_svg(const char *path){size_t n=strlen(path);return n>4 && !strcasecmp(path+n-4,".svg");}

/* box filter down, bilinear up, premultiplied for the shader */
static unsigned char *fit(const unsigned char *src,int w,int h,int size){
	unsigned char *dst=malloc((size_t)size*size*4);
	if(!dst){return NULL;}
	for(int y=0;y<size;y++){for(int x=0;x<size;x++){
		float acc[4]={0},n=0;
		if(w>size || h>size){
			int x0=x*w/size,x1=(x+1)*w/size,y0=y*h/size,y1=(y+1)*h/size;
			if(x1<=x0){x1=x0+1;}if(y1<=y0){y1=y0+1;}
			for(int sy=y0;sy<y1;sy++){for(int sx=x0;sx<x1;sx++){
				const unsigned char *p=src+((size_t)sy*w+sx)*4;float a=p[3]/255.f;
				acc[0]+=p[0]*a;acc[1]+=p[1]*a;acc[2]+=p[2]*a;acc[3]+=p[3];n++;
			}}
		}
		else{
			float fx=(x+.5f)*w/size-.5f,fy=(y+.5f)*h/size-.5f;
			int ix=fx<0 ? 0 : (int)fx,iy=fy<0 ? 0 : (int)fy;
			float tx=fx-ix,ty=fy-iy;if(tx<0){tx=0;}if(ty<0){ty=0;}
			for(int k=0;k<4;k++){
				int sx=ix+(k&1),sy=iy+(k>>1);if(sx>=w){sx=w-1;}if(sy>=h){sy=h-1;}
				const unsigned char *p=src+((size_t)sy*w+sx)*4;float a=p[3]/255.f,f=(k&1 ? tx : 1-tx)*(k>>1 ? ty : 1-ty);
				acc[0]+=p[0]*a*f;acc[1]+=p[1]*a*f;acc[2]+=p[2]*a*f;acc[3]+=p[3]*f;
			}
			n=1;
		}
		unsigned char *d=dst+((size_t)y*size+x)*4;
		for(int k=0;k<4;k++){d[k]=(unsigned char)(acc[k]/n+.5f);}
	}}
	return dst;
}

static unsigned char *find_icon(const char *name,char dirs[][256],int ndirs,int size){
	static const char *sizes[]={"64x64","72x72","96x96","128x128","48x48","256x256","512x512","32x32"};
	char path[600];
	int w,h;
	unsigned char *px=NULL;
	if(!name || !*name){return NULL;}
	if(name[0]=='/'){px=is_svg(name) ? load_svg(name,size,&w,&h) : load_png(name,&w,&h);}
	for(int d=0;d<ndirs && !px;d++){
		for(size_t i=0;i<sizeof(sizes)/sizeof(*sizes) && !px;i++){if(snprintf(path,sizeof(path),"%sicons/hicolor/%s/apps/%s.png",dirs[d],sizes[i],name)<(int)sizeof(path)){px=load_png(path,&w,&h);}}
		if(!px && snprintf(path,sizeof(path),"%sicons/hicolor/scalable/apps/%s.svg",dirs[d],name)<(int)sizeof(path)){px=load_svg(path,size,&w,&h);}
		if(!px && snprintf(path,sizeof(path),"%spixmaps/%s.png",dirs[d],name)<(int)sizeof(path)){px=load_png(path,&w,&h);}
		if(!px && snprintf(path,sizeof(path),"%spixmaps/%s.svg",dirs[d],name)<(int)sizeof(path)){px=load_svg(path,size,&w,&h);}
	}
	if(!px){return NULL;}
	unsigned char *icon=fit(px,w,h,size);
	free(px);
	return icon;
}

static char *exec_line(const char *v,bool terminal){
	size_t n=strlen(v);
	char *out=malloc(n+16),*o=out;
	if(!out){return NULL;}
	if(terminal){o+=sprintf(o,"alacritty -e ");}
	for(const char *p=v;*p;p++){
		if(*p=='%' && p[1]){if(p[1]=='%'){*o++='%';}p++;continue;}
		*o++=*p;
	}
	*o=0;
	return out;
}

static bool parse(const char *path,struct app *a){
	FILE *f=fopen(path,"r");
	char line[1024];
	bool entry=false,hide=false,app=false,terminal=false;
	char *icon=NULL,*exec=NULL;
	if(!f){return false;}

	memset(a,0,sizeof(*a));
	while(fgets(line,sizeof(line),f)){
		line[strcspn(line,"\r\n")]=0;
		if(line[0]=='['){entry=!strcmp(line,"[Desktop Entry]");continue;}
		char *eq=strchr(line,'=');
		if(!entry || !eq){continue;}
		*eq=0;
		const char *k=line,*v=eq+1;
		if(!strcmp(k,"Type")){app=!strcmp(v,"Application");}
		else if(!strcmp(k,"Name") && !a->name){a->name=strdup(v);}
		else if(!strcmp(k,"Exec") && !exec){exec=strdup(v);}
		else if(!strcmp(k,"Icon") && !icon){icon=strdup(v);}
		else if(!strcmp(k,"Terminal")){terminal=!strcmp(v,"true");}
		else if(!strcmp(k,"NoDisplay") || !strcmp(k,"Hidden")){hide|=!strcmp(v,"true");}
		else if(!strcmp(k,"Categories")){a->terminal=strstr(v,"TerminalEmulator")!=NULL;}
	}

	fclose(f);
	if(app && !hide && a->name && exec){a->exec=exec_line(exec,terminal);a->icon_name=icon;icon=NULL;}
	free(exec);free(icon);

	if(!a->exec){free(a->name);a->name=NULL;return false;}
	return true;
}

static int by_name(const void *x,const void *y){return strcasecmp(((const struct app *)x)->name,((const struct app *)y)->name);}

int apps_load(struct app **out){
	char dirs[8][256],path[600],seen[256][128];
	int ndirs=data_dirs(dirs,8),n=0,cap=0,nseen=0;
	struct app *list=NULL;
	for(int d=0;d<ndirs;d++){
		if(snprintf(path,sizeof(path),"%sapplications",dirs[d])>=(int)sizeof(path)){continue;}
		DIR *dir=opendir(path);
		struct dirent *e;
		
		if(!dir){continue;}
		while((e=readdir(dir))){
			size_t len=strlen(e->d_name);
			bool dup=false;
			if(len<9 || strcmp(e->d_name+len-8,".desktop") || len>=sizeof(*seen)){continue;}
			for(int i=0;i<nseen && !dup;i++){dup=!strcmp(seen[i],e->d_name);}
			if(dup){continue;}
			if(nseen<256){strcpy(seen[nseen++],e->d_name);}
			if(n==cap){
				cap=cap ? cap*2 : 16;
				struct app *grown=realloc(list,sizeof(*list)*cap);
				if(!grown){break;}
				list=grown;
			}
			if(snprintf(path,sizeof(path),"%sapplications/%s",dirs[d],e->d_name)<(int)sizeof(path) && parse(path,&list[n])){n++;}
		}
		closedir(dir);
	}
	if(n){qsort(list,n,sizeof(*list),by_name);}
	for(int i=0;i<n;i++){list[i].icon=find_icon(list[i].icon_name,dirs,ndirs,ICON);}
	*out=list;
	return n;
}

unsigned char *icon_load(const char *name,int size){
	char dirs[8][256];
	if(name && !strncmp(name,"file://",7)){name+=7;}
	return find_icon(name,dirs,data_dirs(dirs,8),size);
}

void apps_free(struct app *list,int n){
	for(int i=0;i<n;i++){free(list[i].name);free(list[i].exec);free(list[i].icon_name);free(list[i].icon);}
	free(list);
}