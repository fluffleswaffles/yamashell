// SPDX-License-Identifier: GPL-2.0-only
/* installed apps from .desktop files, shared by the shell's menu and yamashell-settings */
#ifndef APPS_H
#define APPS_H
#include <stdbool.h>
#include <sys/types.h>
/* an installed app; icon = size x size premultiplied RGBA or NULL */
struct app{char *name, *exec, *icon_name;unsigned char *icon;bool terminal;pid_t pid;};
int apps_load(struct app **list, int icon_size);
void apps_free(struct app *list, int n);
/* an icon by name (icon theme, pixmaps) or path, size x size premultiplied RGBA or NULL */
unsigned char *icon_load(const char *name, int size);
#endif
