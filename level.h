// SPDX-License-Identifier: GPL-2.0-only
/* brightness and volume in percent, shared by the shade and yamashell-settings */
#ifndef LEVEL_H
#define LEVEL_H
enum{LEVEL_BRIGHTNESS,LEVEL_VOLUME,LEVEL_COUNT};
/* 0..100 */
int level_get(int which);
void level_set(int which,int pct);
/* the saved brightness back on the panel at the shell's start */
void level_restore(void);
#endif
