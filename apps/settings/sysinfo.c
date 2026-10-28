// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include "sysinfo.h"

#define DT "/sys/firmware/devicetree/base/"
#ifndef APK_DB
#define APK_DB "/lib/apk/db/installed"
#endif

/* a whole small file, NULs (devicetree string lists) kept; length or -1 */
static int slurp(const char *path,char *buf,size_t n){
	FILE *f=fopen(path,"r");
	if(!f){buf[0]=0;return -1;}
	size_t r=fread(buf,1,n-1,f);
	fclose(f);buf[r]=0;
	return (int)r;
}

static void chomp(char *s){s[strcspn(s,"\r\n")]=0;}

/* "key: value" or "key=value" line of a text file */
static bool field(const char *path,const char *key,char *val,size_t n){
	char line[256];FILE *f=fopen(path,"r");bool found=false;size_t k=strlen(key);
	while(f && !found && fgets(line,sizeof(line),f)){
		if(strncmp(line,key,k)){continue;}
		const char *v=line+k;
		while(*v==' ' || *v=='\t' || *v==':' || *v=='='){v++;}
		snprintf(val,n,"%s",*v=='"' ? v+1 : v);chomp(val);
		size_t l=strlen(val);if(l && val[l-1]=='"'){val[l-1]=0;}
		found=true;
	}
	if(f){fclose(f);}
	return found;
}

void sysinfo_size(long long b,char *buf,size_t n){
	if(b>=1LL<<30){snprintf(buf,n,"%.1f GB",b/(double)(1LL<<30));}
	else if(b>=1LL<<20){snprintf(buf,n,"%.1f MB",b/(double)(1LL<<20));}
	else{snprintf(buf,n,"%lld KB",(b+1023)/1024);}
}

void sysinfo_model(char *buf,size_t n){
	if(slurp(DT "model",buf,n)>0 || slurp("/sys/class/dmi/id/product_name",buf,n)>0){chomp(buf);}
	if(!buf[0]){snprintf(buf,n,"Unknown device");}
}

void sysinfo_release(char *buf,size_t n){
	if(slurp("/etc/rapuyama-release",buf,n)>0){chomp(buf);}
	if(!buf[0]){snprintf(buf,n,"Unknown version");}
}

static void cpu(char *buf,size_t n){
	char c[128];long cores=sysconf(_SC_NPROCESSORS_CONF);
	/* "arm,arm1176jzf-s" reads as ARM1176JZF-S */
	if(slurp(DT "cpus/cpu@0/compatible",c,sizeof(c))>0 && strchr(c,',')){
		snprintf(buf,n,"%s",strchr(c,',')+1);
		for(char *p=buf;*p;p++){*p=(char)toupper((unsigned char)*p);}
	}
	else if(!field("/proc/cpuinfo","model name",buf,n)){snprintf(buf,n,"Unknown");}
	if(cores>1){size_t l=strlen(buf);snprintf(buf+l,n-l,", %ld cores",cores);}
}

/* shortcut: only the e6's VideoCore by its device node and the pc's drm driver name; a table if more boards come */
static void gpu(char *buf,size_t n){
	char path[64],drv[64];
	if(!access("/dev/vc-khrn",F_OK)){snprintf(buf,n,"Broadcom VideoCore IV");return;}
	for(int i=0;i<4;i++){
		snprintf(path,sizeof(path),"/sys/class/drm/card%d/device/uevent",i);
		if(field(path,"DRIVER",drv,sizeof(drv))){snprintf(buf,n,"%s (DRM)",drv);return;}
	}
	snprintf(buf,n,"Unknown");
}

static void used(struct info *r,long long use,long long total){
	char a[24],b[24];
	sysinfo_size(use,a,sizeof(a));sysinfo_size(total,b,sizeof(b));
	snprintf(r->value,sizeof(r->value),"%s of %s used",a,b);
	r->bar=total>0 ? (float)use/(float)total : -1;
}

static struct info *add(struct info *rows,int *n,int max,const char *label,uint32_t icon){
	if(*n>=max){return NULL;}
	struct info *r=&rows[(*n)++];
	*r=(struct info){.icon=icon,.bar=-1};
	snprintf(r->label,sizeof(r->label),"%.39s",label);
	return r;
}

static void storage(struct info *rows,int *n,int max){
	char line[512],dev[256],mnt[256],seen[8][256];int nseen=0;
	FILE *f=fopen("/proc/mounts","r");
	struct statvfs st;struct info *r;
	if(!statvfs("/",&st) && (r=add(rows,n,max,"System storage",0xe1db))){used(r,(long long)(st.f_blocks-st.f_bfree)*st.f_frsize,(long long)st.f_blocks*st.f_frsize);}
	while(f && nseen<8 && fgets(line,sizeof(line),f)){
		if(sscanf(line,"%255s %255s",dev,mnt)!=2 || strncmp(dev,"/dev/",5) || !strncmp(dev,"/dev/loop",9) || !strncmp(dev,"/dev/zram",9) || !strcmp(mnt,"/")){continue;}
		bool dup=false;
		for(int i=0;i<nseen;i++){dup|=!strcmp(seen[i],dev);}
		if(dup || statvfs(mnt,&st) || !st.f_blocks){continue;}
		snprintf(seen[nseen++],sizeof(seen[0]),"%s",dev);
		if((r=add(rows,n,max,mnt,0xe1c2))){used(r,(long long)(st.f_blocks-st.f_bfree)*st.f_frsize,(long long)st.f_blocks*st.f_frsize);}
	}
	if(f){fclose(f);}
}

int sysinfo_read(struct info *rows,int max){
	int n=0;struct info *r;char buf[128];long long total=0,avail=0,zsize=0,zused=0;
	if((r=add(rows,&n,max,"Processor",0xe322))){cpu(r->value,sizeof(r->value));}
	if((r=add(rows,&n,max,"Graphics",0xe30d))){gpu(r->value,sizeof(r->value));}
	if(field("/proc/meminfo","MemTotal",buf,sizeof(buf))){total=atoll(buf)*1024;}
	if(field("/proc/meminfo","MemAvailable",buf,sizeof(buf))){avail=atoll(buf)*1024;}
	if(total && (r=add(rows,&n,max,"Memory",0xe1af))){used(r,total-avail,total);}
	/* /proc/swaps: name type size used priority, in KiB */
	FILE *f=fopen("/proc/swaps","r");char line[256],name[128];long long s,u;
	while(f && fgets(line,sizeof(line),f)){if(sscanf(line,"%127s %*s %lld %lld",name,&s,&u)==3 && strstr(name,"zram")){zsize+=s*1024;zused+=u*1024;}}
	if(f){fclose(f);}
	if((r=add(rows,&n,max,"Compressed swap (zram)",0xe8d4))){
		if(zsize){used(r,zused,zsize);}
		else{snprintf(r->value,sizeof(r->value),"Off");}
	}
	storage(rows,&n,max);
	int pct=-1;char st[32]="";
	if(slurp("/sys/class/power_supply/battery/capacity",buf,sizeof(buf))>0){pct=atoi(buf);}
	if(slurp("/sys/class/power_supply/battery/status",st,sizeof(st))>0){chomp(st);}
	if((r=add(rows,&n,max,"Battery",!strcmp(st,"Charging") ? 0xe1a3 : 0xe1a4))){
		if(pct<0){snprintf(r->value,sizeof(r->value),"Not found");}
		else{snprintf(r->value,sizeof(r->value),"%d%%%s%s",pct,st[0] ? ", " : "",st);r->bar=pct/100.f;}
	}
	struct utsname un;
	if(!uname(&un) && (r=add(rows,&n,max,"Linux kernel",0xe86f))){snprintf(r->value,sizeof(r->value),"%.60s, %.20s",un.release,un.machine);}
	if((r=add(rows,&n,max,"Userspace",0xe875))){
		if(!field("/etc/os-release","PRETTY_NAME",r->value,sizeof(r->value))){snprintf(r->value,sizeof(r->value),"Unknown");}
	}
	return n;
}

/* the program an Exec line starts: past "env", VAR=x and the terminal wrapper, then looked up in $PATH */
/* path holds PATH_MAX, what realpath wants */
static bool program(const char *exec,char *path){
	char cmd[512],*save=NULL,*t;
	snprintf(cmd,sizeof(cmd),"%s",exec);
	for(t=strtok_r(cmd," \t",&save);t;t=strtok_r(NULL," \t",&save)){
		if(!strcmp(t,"env") || strchr(t,'=') || !strcmp(t,"-e") || (!strcmp(t,"alacritty") && strstr(exec,"alacritty -e "))){continue;}
		break;
	}
	if(!t){return false;}
	if(strchr(t,'/')){return realpath(t,path)!=NULL;}
	char dirs[1024],full[600],*s2=NULL;
	snprintf(dirs,sizeof(dirs),"%s",getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	for(char *d=strtok_r(dirs,":",&s2);d;d=strtok_r(NULL,":",&s2)){
		snprintf(full,sizeof(full),"%s/%s",d,t);
		if(!access(full,X_OK)){return realpath(full,path)!=NULL;}
	}
	return false;
}

/* the apk database: P: package, I: installed size, F: dir, R: file in it, blank line between packages */
static long long apk_size(const char *path,char *pkg,size_t n){
	char line[1024],name[128]="",dir[512]="";long long size=-1,found=-1;
	FILE *f=fopen(APK_DB,"r");
	while(f && found<0 && fgets(line,sizeof(line),f)){
		chomp(line);
		if(!line[0]){name[0]=dir[0]=0;size=-1;}
		else if(!strncmp(line,"P:",2)){snprintf(name,sizeof(name),"%.127s",line+2);}
		else if(!strncmp(line,"I:",2)){size=atoll(line+2);}
		else if(!strncmp(line,"F:",2)){snprintf(dir,sizeof(dir),"%.511s",line+2);}
		/* I: comes before the file list in apk's own order */
		else if(!strncmp(line,"R:",2) && size>=0){
			char full[1600];
			snprintf(full,sizeof(full),"/%s/%s",dir,line+2);
			if(!strcmp(full,path)){found=size;snprintf(pkg,n,"%s",name);}
		}
	}
	if(f){fclose(f);}
	return found;
}

long long sysinfo_app_size(const char *exec,char *pkg,size_t n){
	char path[PATH_MAX];struct stat st;long long s;
	pkg[0]=0;
	if(!program(exec,path)){return -1;}
	if((s=apk_size(path,pkg,n))>=0){return s;}
	/* shortcut: without an apk database (the pc) only the program file counts; rpm/dpkg lookups if that ever matters */
	return stat(path,&st) ? -1 : (long long)st.st_size;
}
