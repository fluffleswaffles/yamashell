// SPDX-License-Identifier: GPL-2.0-only
/* jpeg / png pictures for the wallpaper and its thumbnails */
#ifndef IMAGE_H
#define IMAGE_H
/* w*h rgb bytes, scaled and cropped to cover w x h
* NULL when it isn't a readable jpeg or png */
unsigned char *image_load(const char *path,int w,int h);
/* the default wallpaper: $YAMASHELL_WALLPAPER */
const char *wallpaper_default(void);
#endif
