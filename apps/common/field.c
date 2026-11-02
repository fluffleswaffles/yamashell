// SPDX-License-Identifier: GPL-2.0-only
#include <string.h>
#include <linux/input.h>
#include "field.h"

void field_end(struct field *f){f->cursor=strlen(f->value);}
void field_init(struct field *f,char *value,unsigned capacity,bool numeric){*f=(struct field){value,capacity,0,numeric};field_end(f);}

static unsigned previous(struct field *f){unsigned p=f->cursor;if(p){do{p--;}while(p && ((unsigned char)f->value[p]&0xc0)==0x80);}return p;}
static unsigned next(struct field *f){unsigned p=f->cursor;if(f->value[p]){do{p++;}while(((unsigned char)f->value[p]&0xc0)==0x80);}return p;}

bool field_key(struct field *f,uint32_t key){
	unsigned length=strlen(f->value),p=f->cursor;
	if(key==KEY_LEFT){f->cursor=previous(f);}
	else if(key==KEY_RIGHT){f->cursor=next(f);}
	else if(key==KEY_HOME){f->cursor=0;}
	else if(key==KEY_END){f->cursor=length;}
	else if(key==KEY_BACKSPACE){unsigned start=previous(f);memmove(f->value+start,f->value+p,length-p+1);f->cursor=start;}
	else if(key==KEY_DELETE){memmove(f->value+p,f->value+next(f),length-next(f)+1);}
	else{return false;}
	return true;
}

bool field_text(struct field *f,const char *value){
	unsigned n=strlen(value),length=strlen(f->value);
	if(!n || length+n>=f->capacity){return false;}
	for(unsigned i=0;i<n;i++){
		if(f->numeric){if((value[i]<'0' || value[i]>'9') && !(value[i]=='+' && f->cursor==0 && i==0 && f->value[0]!='+')){return false;}}
		else if((unsigned char)value[i]<32 && value[i]!='\n'){return false;}
	}
	if(f->numeric && f->cursor==0 && f->value[0]=='+' && value[0]!='+'){return false;}
	memmove(f->value+f->cursor+n,f->value+f->cursor,length-f->cursor+1);memcpy(f->value+f->cursor,value,n);f->cursor+=n;return true;
}

static void layout(struct field *f,float width,int size,unsigned stop,float *out_x,float *out_y){
	float x=0,y=0;
	for(unsigned p=0;p<stop && f->value[p];){
		unsigned end=p+1;while(((unsigned char)f->value[end]&0xc0)==0x80){end++;}
		char glyph[5]={0};unsigned n=end-p;if(n>4){break;}memcpy(glyph,f->value+p,n);
		float w=text(F_TEXT,0,0,glyph,size,C_WHITE,0,0);
		if(glyph[0]=='\n' || x+w>width){x=0;y+=size+5;}
		if(glyph[0]!='\n'){x+=w;}p=end;
	}
	*out_x=x;*out_y=y;
}

void field_draw(struct field *f,float x,float y,float w,float h,int size,const char *placeholder,bool focused,bool multiline){
	fill(x,y,w,h,10,C_BUBBLE,C_BUBBLE,1);
	if(focused){fill(x+10,y+h-2,w-20,2,1,C_ACCENT,C_ACCENT2,1);}
	float cx,cy;layout(f,multiline ? w-24 : 100000,size,f->cursor,&cx,&cy);
	float sx=multiline ? 0 : fmaxf(0,cx-(w-28)),sy=multiline ? fmaxf(0,cy-(h-size-20)) : 0;
	clip(x+10,y+6,x+w-10,y+h-6);
	if(!*f->value){text(F_TEXT,x+12,y+size+8,placeholder,size,C_WHITE,.4f,0);}
	float tx=0,ty=0;
	for(unsigned p=0;f->value[p];){
		unsigned end=p+1;while(((unsigned char)f->value[end]&0xc0)==0x80){end++;}
		char glyph[5]={0};unsigned n=end-p;if(n>4){break;}memcpy(glyph,f->value+p,n);
		float gw=text(F_TEXT,0,0,glyph,size,C_WHITE,0,0);
		if(multiline && (glyph[0]=='\n' || tx+gw>w-24)){tx=0;ty+=size+5;}
		if(glyph[0]!='\n'){text(F_TEXT,x+12+tx-sx,y+size+8+ty-sy,glyph,size,C_WHITE,1,0);tx+=gw;}p=end;
	}
	if(focused && app_caret()){fill(x+12+cx-sx,y+8+cy-sy,1.5f,size+3,0,C_WHITE,C_WHITE,1);}
	clip(0,0,app_w,app_h);
}