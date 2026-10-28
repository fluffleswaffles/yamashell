// SPDX-License-Identifier: GPL-2.0-only
/* lock screen */
#include <stdio.h>
#include "ui.h"

#define BAR_W 220

void ui_lock(struct server *s){
	struct ui *u=s->ui;
	if(!u){return;}
	u->locked=true;u->dirty=true;
	u->lock=(struct spring){1,0,1};
	u->charge.x=0;
	schedule_repaint(s,true);
}

void ui_unlock(struct server *s){
	struct ui *u=s->ui;
	if(!u || !u->locked){return;}
	u->locked=false;spring_to(&u->lock,0);
	schedule_repaint(s,true);
}

bool ui_locked(struct server *s){return s->ui && s->ui->locked;}

void ui_aod(struct server *s,bool on){
	struct ui *u=s->ui;
	if(!u){return;}
	u->aod=on;u->dirty=true;
	schedule_repaint(s,true);
}

static void aod_build(struct ui *u,struct layer *l,const char *hm,const char *date,int minute){
	rect(l,0,-screen_h*.25f,screen_w,screen_h*1.25f,0,(struct col){0,0,0,1},(struct col){0,0,0,1});
	float dx=(minute%5-2)*6.f,dy=(minute/5%3-1)*6.f,cx=screen_w/2.f+dx,base=screen_h*.4f+dy;
	text(u,l,F_LIGHT,cx,base,hm,(screen_w<screen_h ? screen_w : screen_h)/6,white(.6f),1);
	text(u,l,F_TEXT,cx,base+30,date,16,white(.45f),1);
	if(u->st.charger && u->st.pct>=0){
		char buf[16];snprintf(buf,sizeof(buf),"%d%%",u->st.pct);
		icon(u,l,cx-16,base+60,I_CHARGING,16,white(.45f));
		text(u,l,F_TEXT,cx-6,base+66,buf,15,white(.45f),0);
	}
	u->charge=(struct spring){0,0,0};
}

void lock_build(struct ui *u,const char *hm,const char *date,int minute){
	struct layer *l=&u->layers[L_LOCK];
	if(u->aod){aod_build(u,l,hm,date,minute);return;}

	rect(l,0,-screen_h*.25f,screen_w,screen_h*1.25f,0,(struct col){0,0,0,.3f},(struct col){0,0,0,.6f});
	int big=(screen_w<screen_h ? screen_w : screen_h)/4;
	float base=screen_h*.42f;
	text(u,l,F_LIGHT,screen_w/2.f+2,base+2,hm,big,(struct col){0,0,0,.3f},1);
	text(u,l,F_LIGHT,screen_w/2.f,base,hm,big,white(1),1);
	text(u,l,F_TEXT,screen_w/2.f,base+40,date,22,white(.95f),1);

	if(u->st.charger){
		char buf[48];
		if(u->st.pct>=0){snprintf(buf,sizeof(buf),u->st.charging ? "Charging, %d%%" : "Charged, %d%%",u->st.pct);}
		else{snprintf(buf,sizeof(buf),"Charging");}
		float y=screen_h-96;
		icon(u,l,screen_w/2.f-BAR_W/2.f+10,y-6,I_CHARGING,20,white(.9f));
		text(u,l,F_TEXT,screen_w/2.f-BAR_W/2.f+26,y,buf,16,white(.9f),0);
		rect(l,(screen_w-BAR_W)/2.f,y+10,BAR_W,6,3,white(.2f),white(.2f));
	}
	icon(u,l,screen_w/2.f,screen_h-46,I_UP,28,white(.7f));
	text(u,l,F_TEXT,screen_w/2.f,screen_h-18,"Swipe up to unlock",15,white(.7f),1);
	spring_to(&u->charge,u->st.charger && u->st.pct>0 ? u->st.pct/100.f : 0);
}

void lock_step(struct ui *u,float dt){
	if(u->gesture!=G_LOCK){spring_step(&u->lock,dt,260);}
	spring_step(&u->charge,dt,12);
}

void lock_draw(struct server *s,struct layer *dyn){
	struct ui *u=s->ui;
	if(u->lock.x<=0){return;}
	float y=fminf(0,-(1-u->lock.x)*screen_h),gy=2*y/screen_h;
	if(!u->aod){
		end(s);
		glBindTexture(GL_TEXTURE_2D,u->wall);
		glUniform1f(s->alpha,1.f);glUniform1f(s->native,1.f);
		renderer_quad(s,-1,1-gy,1,-1-gy,true);
		begin(s);
	}
	draw_layer(u,L_LOCK,0,y);
	if(!u->aod && u->st.charger && u->charge.x>0){
		float w=BAR_W*clampf(u->charge.x,0,1);
		rect(dyn,(screen_w-BAR_W)/2.f,screen_h-86+y,w,6,3,rgb(0x8ad06a,1),rgb(0x5fb048,1));
	}
}
