// SPDX-License-Identifier: GPL-2.0-only
/* the settings that outlive a reboot: key=value lines in $XDG_CONFIG_HOME/yamashell/settings.conf,
 * shared by the shell and the system apps */
#ifndef CONFIG_H
#define CONFIG_H
#include <stdbool.h>
#include <stddef.h>
/* directory. false without $HOME */
bool config_dir(char *path,size_t n);
/* false when the key isn't set */
bool config_get(const char *key,char *val,size_t n);
void config_set(const char *key,const char *val);
#endif
