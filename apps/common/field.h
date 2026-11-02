// SPDX-License-Identifier: GPL-2.0-only
#ifndef FIELD_H
#define FIELD_H

#include "app.h"

struct field{char *value;unsigned capacity,cursor;bool numeric;};
void field_init(struct field *f,char *value,unsigned capacity,bool numeric);
void field_end(struct field *f);
bool field_key(struct field *f,uint32_t key);
bool field_text(struct field *f,const char *value);
void field_draw(struct field *f,float x,float y,float w,float h,int size,const char *placeholder,bool focused,bool multiline);
#endif