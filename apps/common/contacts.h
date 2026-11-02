// SPDX-License-Identifier: GPL-2.0-only
#ifndef CONTACTS_H
#define CONTACTS_H

#include <stddef.h>
#include <stdbool.h>

struct contact{char name[128],number[24];};
int contacts_load(struct contact *out,int capacity);
int contacts_read(const char *path,struct contact *out,int capacity);
bool contacts_save(const struct contact *contacts,int count);
const char *contacts_name(const char *number);

#endif
