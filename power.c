// SPDX-License-Identifier: GPL-2.0-only
/* screen-off idle keeps input, USB and the watchdogs alive */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "yamashell.h"
#include "config.h"

struct power{
	struct server *server;
	struct wl_event_source *timer;
	const char *screen, *keyboard;
	uint32_t activity, key_activity, dim_ms, off_ms, key_ms;
	unsigned char swallowed[KEY_MAX+1];
	unsigned touch_mask;
	int brightness, key_brightness, screen_value, key_value;
	bool asleep, dimmed;
};

static int read_value(const char *path){
	char buf[32];
	int fd=open(path, O_RDONLY|O_CLOEXEC);
	if(fd<0){return -1;}
	ssize_t n=read(fd, buf, sizeof(buf)-1);
	close(fd);
	if(n<=0){return -1;}
	buf[n]=0;
	return atoi(buf);
}

static bool write_value(const char *path, int value, int *last){
	char buf[32];
	if(!path || *last==value){return true;}
	int n=snprintf(buf, sizeof(buf), "%d\n", value);
	int fd=open(path, O_WRONLY|O_TRUNC|O_CLOEXEC);
	if(fd<0){return false;}
	bool ok=write(fd, buf, n)==n;
	close(fd);
	if(ok){*last=value;}
	else{fprintf(stderr, "yamashell: backlight write failed: %s\n", path);}
	return ok;
}

static uint32_t timeout_ms(const char *name, unsigned seconds){
	const char *env=getenv(name);
	char *end;
	unsigned long n=env ? strtoul(env, &end, 10) : seconds;
	if(env && (!*env || *end || n>86400)){n=seconds;}
	return n*1000;
}

static uint32_t setting_ms(const char *env,const char *key,unsigned def){
	char v[16];
	if(!getenv(env) && config_get(key,v,sizeof(v))){int n=atoi(v);if(n>=0 && n<=86400){def=(unsigned)n;}}
	return timeout_ms(env,def);
}

static void arm(struct power *p){
	uint32_t now=milliseconds(), delay=0, elapsed=now-p->activity;
	if(!p->asleep){
		if(!p->dimmed && p->dim_ms && elapsed<p->dim_ms){delay=p->dim_ms-elapsed;}
		if(p->off_ms){
			uint32_t next=elapsed<p->off_ms ? p->off_ms-elapsed : 1000;
			if(!delay || next<delay){delay=next;}
		}
	}
	if(p->key_value>0 && p->key_ms){
		uint32_t elapsed_key=now-p->key_activity;
		uint32_t next=elapsed_key<p->key_ms ? p->key_ms-elapsed_key : 1000;
		if(!delay || next<delay){delay=next;}
	}
	wl_event_source_timer_update(p->timer, delay);
}

bool power_asleep(struct server *s){return s->power && s->power->asleep;}

/* the shade and settings sliders write the backlight too, pick their value up before dimming */
static void sync_brightness(struct power *p){
	if(p->asleep || p->dimmed){return;}
	int v=read_value(p->screen);
	if(v>0){p->brightness=p->screen_value=v;}
}

static void wake(struct power *p){
	bool was_asleep=p->asleep;
	p->activity=milliseconds();
	p->dimmed=false;
	write_value(p->screen, p->brightness, &p->screen_value);
	p->asleep=false;
	if(was_asleep){fprintf(stderr, "yamashell: screen awake\n");schedule_repaint(p->server, true);}
	arm(p);
}

void power_sleep(struct server *s){
	struct power *p=s->power;
	if(!p || p->asleep){return;}
	sync_brightness(p);
	/* keep drawing if the hardware could not actually turn the light off */
	if(!write_value(p->screen, 0, &p->screen_value)){return;}
	write_value(p->keyboard, 0, &p->key_value);
	p->asleep=true;
	if(s->repaint_pending){wl_event_source_timer_update(s->repaint, 0);s->repaint_pending=false;}
	fprintf(stderr, "yamashell: screen asleep\n");
	arm(p);
}

static int tick(void *data){
	struct power *p=data;
	struct server *s=p->server;
	uint32_t now=milliseconds(), elapsed=now-p->activity;
	if(s->npressed || s->ntouches || p->touch_mask){p->activity=now;elapsed=0;}
	sync_brightness(p);
	if(!p->asleep && p->off_ms && elapsed>=p->off_ms){power_sleep(s);}
	else if(!p->asleep && !p->dimmed && p->dim_ms && elapsed>=p->dim_ms){
		int dim=p->brightness/8;
		if(dim<1){dim=1;}
		p->dimmed=write_value(p->screen, dim, &p->screen_value);
	}
	if(p->key_ms && now-p->key_activity>=p->key_ms){write_value(p->keyboard, 0, &p->key_value);}
	arm(p);
	return 0;
}

bool power_key(struct server *s, uint32_t key, uint32_t state){
	struct power *p=s->power;
	if(!p || key>KEY_MAX){return false;}
	if(!state && p->swallowed[key]){p->swallowed[key]=0;return true;}
	if(key==KEY_POWER || key==KEY_SLEEP || key==KEY_SCREENLOCK){
		if(state){
			p->swallowed[key]=1;
			if(p->asleep){wake(p);}else{power_sleep(s);}
		}
		return true;
	}
	if(state){
		bool consume=p->asleep;
		wake(p);
		p->key_activity=milliseconds();
		write_value(p->keyboard, p->key_brightness, &p->key_value);
		arm(p);
		if(consume){p->swallowed[key]=1;return true;}
	}
	return false;
}

bool power_touch(struct server *s, int id, int state){
	struct power *p=s->power;
	if(!p || id<0 || id>=16){return false;}
	unsigned bit=1u<<id;
	if(state==1 && (p->asleep || p->touch_mask)){p->touch_mask|=bit;}
	bool consume=p->touch_mask&bit;
	if(state){wake(p);}
	if(!state){p->touch_mask&=~bit;}
	return consume;
}

bool power_pointer(struct server *s){
	if(!s->power){return false;}
	bool consume=s->power->asleep;
	wake(s->power);
	return consume;
}

void power_init(struct server *s){
	const char *screen=getenv("YAMASHELL_BACKLIGHT");
	const char *keyboard=getenv("YAMASHELL_KEYLIGHT");
	/* host previews never touch the host's backlights */
	if(s->headless && !screen){return;}
	if(!screen){screen="/sys/class/backlight/rapuyama-vc/brightness";}
	if(!keyboard){keyboard="/sys/class/leds/e6:white:kbd_backlight/brightness";}
	int brightness=read_value(screen);
	if(brightness<0){return;}
	struct power *p=calloc(1, sizeof(*p));
	if(!p){return;}
	p->server=s;p->screen=screen;p->keyboard=keyboard;
	p->screen_value=brightness;
	p->brightness=brightness ? brightness : 63;
	p->asleep=brightness==0;
	p->key_brightness=p->key_value=read_value(keyboard);
	if(p->key_brightness<1){p->key_brightness=229;}
	if(p->key_value<0){p->keyboard=NULL;}
	p->key_ms=timeout_ms("YAMASHELL_KEYLIGHT_SECONDS", 5);
	p->activity=p->key_activity=milliseconds();
	p->timer=wl_event_loop_add_timer(s->loop, tick, p);
	if(!p->timer){free(p);return;}
	s->power=p;
	write_value(p->keyboard, 0, &p->key_value);
	power_settings(s);
}

void power_settings(struct server *s){
	struct power *p=s->power;
	if(!p){return;}
	p->dim_ms=setting_ms("YAMASHELL_DIM_SECONDS","dim_seconds",30);
	p->off_ms=setting_ms("YAMASHELL_SLEEP_SECONDS","sleep_seconds",60);
	arm(p);
}

void power_finish(struct server *s){
	struct power *p=s->power;
	if(!p){return;}
	wl_event_source_remove(p->timer);
	write_value(p->screen, p->brightness, &p->screen_value);
	write_value(p->keyboard, 0, &p->key_value);
	s->power=NULL;
	free(p);
}