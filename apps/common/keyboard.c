// SPDX-License-Identifier: GPL-2.0-only
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include "app.h"
#include "config.h"

static double down_x,down_y;
static bool shift;

bool app_hardware_keyboard(void){
#ifdef YAMASHELL_HOST
	const char *v=getenv("YAMASHELL_HARDWARE_KEYBOARD");
	if(v){return strcmp(v,"0")!=0;}
#endif
	return strcmp(config_device(),"n8")!=0;
}

float keyboard_height(void){return app_hardware_keyboard() ? 0 : 196;}

static const char *row(int r,bool numeric){
	static const char *letters[]={"qwertyuiop","asdfghjkl","zxcvbnm"," "};
	static const char *digits[]={"123","456","789","+0"};
	return numeric ? digits[r] : letters[r];
}

void keyboard_draw(bool numeric){
	if(!keyboard_height()){return;}
	float top=app_h-keyboard_height();
	fill(0,top,app_w,keyboard_height(),0,C_BUBBLE,C_BUBBLE,1);
	for(int r=0;r<4;r++){
		const char *s=row(r,numeric);int n=strlen(s)+((r==2 || r==3) ? 1 : 0);float w=(float)app_w/n;
		for(int c=0;c<n;c++){
			char label[8]={0};
			if(c<(int)strlen(s)){label[0]=shift && s[c]>='a' && s[c]<='z' ? s[c]-32 : s[c];if(s[c]==' '){strcpy(label,"Space");}}
			fill(c*w+2,top+r*40+3,w-4,35,6,C_TILE,C_TILE2,1);
			if(c==(int)strlen(s)){icon((c+.5f)*w,top+r*40+20,r==2 ? 0xe14a : numeric ? 0xe5ca : 0xe5d8,22,shift && !numeric && r==3 ? C_ACCENT : C_WHITE,1);}
			else{text(F_TEXT,(c+.5f)*w,top+r*40+27,label,17,C_WHITE,1,1);}
		}
	}
}

bool keyboard_touch(int state,double x,double y,bool numeric){
	if(!keyboard_height()){return false;}
	if(state==T_DOWN){down_x=x;down_y=y;}
	if(down_y<app_h-keyboard_height()){return false;}
	if(state!=T_UP){return true;}
	if(x<0 || fabs(x-down_x)>16 || fabs(y-down_y)>16){return true;}
	int r=(down_y-(app_h-keyboard_height()))/40;if(r<0 || r>3){return true;}
	const char *s=row(r,numeric);int len=strlen(s),n=len+((r==2 || r==3) ? 1 : 0),c=down_x*n/app_w;
	if(c<0 || c>=n){return true;}
	if(c==len){if(r==2){app_key(KEY_BACKSPACE);}else if(numeric){app_key(KEY_ENTER);}else{shift=!shift;}}
	else{char value[2]={shift && s[c]>='a' && s[c]<='z' ? s[c]-32 : s[c],0};app_text(value);}
	return true;
}