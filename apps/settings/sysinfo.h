// SPDX-License-Identifier: GPL-2.0-only
/* what about phone and the app pages show, read from /proc, /sys and the package database */
#ifndef SYSINFO_H
#define SYSINFO_H
#include <stddef.h>
#include <stdint.h>

/* bar: how full, 0..1, or -1 for none */
struct info{char label[40],value[96];uint32_t icon;float bar;};
int sysinfo_read(struct info *rows,int max);
/* the devicetree model, or the pc's product name */
void sysinfo_model(char *buf,size_t n);
/* e6, n8 or e7, from the devicetree; e6 when it isn't one of them */
/* first line of /etc/rapuyama-release */
void sysinfo_release(char *buf,size_t n);
/* bytes the program behind an Exec line takes; pkg gets its package when the apk database knows it */
long long sysinfo_app_size(const char *exec,char *pkg,size_t n);
void sysinfo_size(long long bytes,char *buf,size_t n);
#endif
