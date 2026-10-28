// SPDX-License-Identifier: GPL-2.0-only
/* the notification shade: quick toggles, system and posted notes, the toast, swiping notes away */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ui.h"

struct toggle toggles[]={{"WLAN",0xe648,false,true},{"Bluetooth",0xe1a9,false,true},{"Silent",0xe04f,false,false},{"Torch",0xe3e7,false,false}};
#define NTOGGLES (int)(sizeof(toggles)/sizeof(*toggles))

static const struct note *toast_note(struct ui *u){
	const struct note *n;int count=notify_list(&n);
	for(int i=0;i<count;i++){if(n[i].id==u->toast_id){return &n[i];}}
	return NULL;
}

/* a notification icon in the atlas  */
static int icon_slot(struct ui *u,const char *name){
	int k=-1,old=0;
	for(int i=0;i<NOTE_SLOTS && k<0;i++){
		if(!strcmp(u->nicons[i].name,name)){k=i;}
		else if(u->nicons[i].used<u->nicons[old].used){old=i;}
	}
	if(k<0){
		struct nicon *c=&u->nicons[k=old];
		unsigned char *big=icon_load(name,40),*small=big ? icon_load(name,20) : NULL;
		snprintf(c->name,sizeof(c->name),"%s",name);
		c->ok=big && small;
		if(c->ok){
			int i=APP_SLOTS+k,ax=i%(ATLAS_W/ICON)*ICON,ay=GLYPHS_H+i/(ATLAS_W/ICON)*ICON;
			for(int y=0;y<40;y++){memcpy(u->pixels+((size_t)(ay+y)*ATLAS_W+ax)*4,big+(size_t)y*40*4,40*4);}

			for(int y=0;y<20;y++){for(int x=0;x<20;x++){
				const unsigned char *p=small+((size_t)y*20+x)*4;unsigned char *d=u->pixels+((size_t)(ay+y)*ATLAS_W+ax+42+x)*4;
				float luma=p[3] ? (.3f*p[0]+.59f*p[1]+.11f*p[2])/p[3] : 0;
				d[0]=d[1]=d[2]=d[3]=(unsigned char)(p[3]*(.35f+.65f*fminf(luma,1)));
			}}
			dirty_rows(u,ay,ay+40);
		}
		free(big);free(small);
	}
	u->nicons[k].used=++u->nicon_tick;
	return u->nicons[k].ok ? k : -1;
}

/* the note's own icon, else the icon of the app that sent it, -1 = the default bell */
int note_slot(struct ui *u,const struct note *n){
	int k=n->icon[0] ? icon_slot(u,n->icon) : -1;
	for(int i=0;i<u->napps && k<0;i++){
		struct app *a=&u->apps[i];
		if(a->icon_name && n->app[0] && (!strcasecmp(a->name,n->app) || !strcasecmp(a->icon_name,n->app))){k=icon_slot(u,a->icon_name);}
	}
	return k;
}

/* size 40 (colour) or 20 (status bar: only the shape, white like android) from a slot, the glyph ic without one */
void note_icon(struct ui *u,struct layer *l,float cx,float cy,int slot,uint32_t ic,int size,float a){
	bool small=size==20;
	if(slot<0){icon(u,l,cx,cy,ic,size*3/4,small ? white(.9f*a) : rgb(0x3cb2ff,a));return;}
	int i=APP_SLOTS+slot;
	quad_atlas(l,i%(ATLAS_W/ICON)*ICON+(small ? 42 : 0),GLYPHS_H+i/(ATLAS_W/ICON)*ICON,size,size,roundf(cx-size/2.f),roundf(cy-size/2.f),size,size,white(small ? .9f*a : a),2);
}

/* icon, title and one line of text, cut to the screen */
static void note_row(struct ui *u,struct layer *l,float x,float y,float w,uint32_t ic,int slot,const char *title,const char *sub,float a){
	char buf[200];
	note_icon(u,l,x+26,y+34,slot,ic,40,a);
	fit_text(u,F_TEXT,title,19,w-74,buf,sizeof(buf));
	text(u,l,F_TEXT,x+58,y+30,buf,19,white(a),0);
	fit_text(u,F_TEXT,sub,15,w-74,buf,sizeof(buf));
	text(u,l,F_TEXT,x+58,y+53,buf,15,white(.65f*a),0);
}

/* icon, track and knob */
static void slider(struct ui *u,struct layer *l,int k,float x,float y,float w){
	int v=level_get(k);float a=v<0 ? .42f : 1,tx=x+44,tw=w-56,cy=y+20,f=v<0 ? 0 : tw*v/100.f;
	icon(u,l,x+18,cy,k==LEVEL_BRIGHTNESS ? I_BRIGHTNESS : I_VOLUME,24,white(.85f*a));
	rect(l,tx,cy-3,tw,6,3,rgb(0x484848,a),rgb(0x484848,a));
	if(v<0){return;}
	rect(l,tx,cy-3,f,6,3,rgb(0x3cb2ff,1),rgb(0x0b72c9,1));
	rect(l,tx+f-10,cy-10,20,20,10,white(1),white(1));
	/* the item reaches past the track ends so 0 and 100 are easy to hit */
	item(u,L_SHADE,A_SLIDER,k,tx-12,y,tw+24,40);
}

void slider_set(struct server *s,const struct item *it,double x){
	level_set(it->arg,(int)lround((x-it->x-12)*100/(it->w-24)));
	s->ui->dirty=true;schedule_repaint(s,true);
}

void slider_step(struct server *s,const struct item *it,int step){
	level_set(it->arg,level_get(it->arg)+step);
	s->ui->dirty=true;schedule_repaint(s,true);
}

/* the sheet's gradient at y, so the covers over the scrolled list match the background */
static struct col shade_col(float y){
	float t=clampf((y+screen_h*.25f)/(screen_h*1.25f),0,1);
	return rgb((uint32_t)(0x26-0x1c*t)*0x010101,.97f);
}

/* background behind the list, the list itself, then covers with the toggles above and the handle below */
void shade_build(struct ui *u){
	struct layer *bg=&u->layers[L_SHADE],*list=&u->layers[L_NOTES],*top=&u->layers[L_SHADE_TOP],*toast=&u->layers[L_TOAST];struct status st=u->st;
	float y0=-screen_h*.25f;
	/* side by side in landscape, one under the other in portrait*/
	bool wide=screen_w>screen_h;float extra=wide ? 44 : 84;
	u->list_top=STATUS+142+extra;u->list_bottom=screen_h-BOTTOM;
	rect(bg,0,u->list_top,screen_w,u->list_bottom-u->list_top,0,shade_col(u->list_top),shade_col(u->list_bottom));
	rect(top,0,y0,screen_w,u->list_top-y0,0,shade_col(y0),shade_col(u->list_top));
	rect(top,0,u->list_bottom,screen_w,screen_h-u->list_bottom,0,shade_col(u->list_bottom),shade_col(screen_h));
	float gw=(screen_w-24)/(float)NTOGGLES,tw=gw-12<88 ? gw-12 : 88;
	for(int i=0;i<NTOGGLES;i++){
		struct toggle *t=&toggles[i];float x=12+gw*i+(gw-tw)/2,y=STATUS+14;float a=t->na ? .42f : 1;
		if(t->on){rect(top,x,y,tw,60,10,rgb(0x3cb2ff,a),rgb(0x0b72c9,a));}
		else{rect(top,x,y,tw,60,10,rgb(0x484848,a),rgb(0x1f1f1f,a));}
		icon(u,top,x+tw/2,y+30,t->icon,32,white(a));
		text(u,top,F_TEXT,x+tw/2,y+82,t->name,14,white(.75f*a),1);
		item(u,L_SHADE,A_TOGGLE,i,x-4,y-4,tw+8,96);
	}
	for(int k=0;k<LEVEL_COUNT;k++){
		float sw=wide ? (screen_w-24)/2.f : screen_w-24;
		slider(u,top,k,12+(wide ? k*sw : 0),STATUS+108+(wide ? 0 : k*40),sw);
	}
	text(u,top,F_TEXT,16,STATUS+130+extra,"Notifications",15,white(.65f),0);
	float ny=u->list_top;
	struct{uint32_t icon;const char *title;char sub[96];bool show;}notes[3]={
		{I_USB,"USB connected","Shell on ttyGS0",st.usb},
		{st.charging ? I_CHARGING : I_BATTERY,"Battery","",st.pct>=0},
		{I_CARD,"Memory card","Root filesystem on mmcblk0p2",st.card}};
	if(st.pct>=0){snprintf(notes[1].sub,sizeof(notes[1].sub),"%d%%, %s",st.pct,st.charging ? "charging" : st.charger ? "charger connected" : "on battery");}

	for(int i=0;i<3;i++){
		if(!notes[i].show){continue;}
		note_row(u,list,12,ny,screen_w-24,notes[i].icon,-1,notes[i].title,notes[i].sub,1);
		rect(list,12,ny+68,screen_w-24,1,0,white(.12f),white(.12f));
		ny+=70;
	}
	/* posted notes under the system ones, swiped sideways they go away */
	const struct note *posted;int nposted=notify_list(&posted);
	for(int i=0;i<nposted;i++){
		if(posted[i].id==u->swipe_id){u->swipe_y=ny;}
		else{note_row(u,list,12,ny,screen_w-24,I_NOTE,note_slot(u,&posted[i]),posted[i].title,posted[i].body,1);}
		rect(list,12,ny+68,screen_w-24,1,0,white(.12f),white(.12f));
		item(u,L_NOTES,A_NOTE,i,12,ny,screen_w-24,68);
		ny+=70;
	}
	u->notes_scroll.max=fmaxf(0,ny-u->list_bottom);

	/* toast: the newest posted note, slides out from under the status bar */
	const struct note *t=toast_note(u);
	if(t){
		rect(toast,8,STATUS+6,screen_w-16,TOAST_H,12,rgb(0x333333,.97f),rgb(0x1a1a1a,.97f));
		note_row(u,toast,8,STATUS+3,screen_w-16,I_NOTE,note_slot(u,t),t->title,t->body,1);
	}

	rect(top,screen_w/2.f-36,screen_h-14,72,6,3,white(.5f),white(.5f));
	item(u,L_SHADE,A_CLOSE,0,screen_w/2.f-80,screen_h-BOTTOM,160,BOTTOM);
}

void ui_notes_changed(struct server *s,uint32_t new_id){
	struct ui *u=s->ui;
	if(!u){return;}
	if(new_id){
		u->toast_id=new_id;
		if(!is_open(&u->shade)){spring_to(&u->toast,1);wl_event_source_timer_update(u->toast_hide,TOAST_MS);}
	}
	if(!toast_note(u)){spring_to(&u->toast,0);}
	u->dirty=true;schedule_repaint(s,true);
}

static int toast_timeout(void *data){
	struct server *s=data;const struct note *t=toast_note(s->ui);
	/* a critical toast waits for the user */
	if(t && t->critical){return 0;}
	spring_to(&s->ui->toast,0);schedule_repaint(s,true);
	return 0;
}

void osd_show(struct server *s){
	struct ui *u=s->ui;
	u->osd_value=level_get(LEVEL_VOLUME);
	spring_to(&u->osd,1);wl_event_source_timer_update(u->osd_hide,OSD_MS);
	schedule_repaint(s,true);
}

static int osd_timeout(void *data){
	struct server *s=data;
	spring_to(&s->ui->osd,0);schedule_repaint(s,true);
	return 0;
}

void osd_draw(struct ui *u,struct layer *dyn){
	if(u->osd.x<=.01f){return;}
	float w=fminf(screen_w-16,300),h=48,x=(screen_w-w)/2,y=STATUS+6-(1-u->osd.x)*(STATUS+12+h);
	rect(dyn,x,y,w,h,h/2,rgb(0x333333,.97f),rgb(0x1a1a1a,.97f));
	char buf[16];int v=u->osd_value<0 ? 0 : u->osd_value;
	snprintf(buf,sizeof(buf),"%d%%",v);
	icon(u,dyn,x+28,y+h/2,v ? I_VOLUME : I_VOLUME_OFF,26,white(1));
	float tx=x+54,tw=w-54-58;
	rect(dyn,tx,y+h/2-3,tw,6,3,white(.25f),white(.25f));
	if(v){rect(dyn,tx,y+h/2-3,tw*v/100.f,6,3,rgb(0x3cb2ff,1),rgb(0x0b72c9,1));}
	text(u,dyn,F_TEXT,x+w-18,y+30,buf,16,white(.9f),2);
}

bool toast_shown(struct ui *u){return is_open(&u->toast) && toast_note(u);}

void open_shade(struct server *s,bool on){
	struct ui *u=s->ui;
	spring_to(&u->shade,on ? 1 : 0);u->focus=-1;
	if(on){spring_to(&u->toast,0);u->dirty=true;}
	schedule_repaint(s,true);
}

/* the row leaves the shade layer and follows the swipe spring */
void swipe_start(struct ui *u,uint32_t id){
	u->swipe_id=id;u->swipe_out=false;u->swipe=(struct spring){0,0,0};u->dirty=true;
}
void swipe_away(struct ui *u,float dir){u->swipe_out=true;spring_to(&u->swipe,dir>0 ? 1.2f : -1.2f);}

void shade_step(struct ui *u,float dt){
	if(u->swipe_id && u->gesture!=G_NOTE){
		spring_step(&u->swipe,dt,u->swipe_out ? 500 : 400);
		/* out of sight: closed for real; back home: drawn by the shade layer again */
		if(u->swipe_out && fabsf(u->swipe.x)>=1){uint32_t id=u->swipe_id;u->swipe_id=0;u->dirty=true;notify_close(id);}
		else if(!u->swipe_out && !spring_moving(&u->swipe)){u->swipe_id=0;u->dirty=true;}
	}
}

void shade_draw_swipe(struct ui *u,struct layer *dyn){
	if(u->swipe_id && u->shade.x>0){
		const struct note *n;int count=notify_list(&n),i=0;
		while(i<count && n[i].id!=u->swipe_id){i++;}
		/* closed by its app meanwhile */
		if(i==count){u->swipe_id=0;u->dirty=true;}
		else{note_row(u,dyn,12+u->swipe.x*screen_w,u->swipe_y-(1-u->shade.x)*screen_h-u->notes_scroll.x,screen_w-24,I_NOTE,note_slot(u,&n[i]),n[i].title,n[i].body,clampf(1-fabsf(u->swipe.x),0,1));}
	}
}

void shade_init(struct server *s){
	s->ui->toast_hide=wl_event_loop_add_timer(s->loop,toast_timeout,s);
	s->ui->osd_hide=wl_event_loop_add_timer(s->loop,osd_timeout,s);
	notify_init(s);
}

void shade_finish(struct ui *u){
	wl_event_source_remove(u->toast_hide);wl_event_source_remove(u->osd_hide);
	notify_finish();
}