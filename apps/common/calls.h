// SPDX-License-Identifier: GPL-2.0-only
#ifndef CALLS_H
#define CALLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum{CALL_RINGING,CALL_DIALING,CALL_CONNECTED,CALL_COMPLETED,CALL_MISSED,CALL_DECLINED,CALL_NOT_CONNECTED,CALL_INTERRUPTED};
struct call_record{uint32_t magic,incoming,result;uint64_t started,answered,ended;char id[48],number[24];};
struct call_state{unsigned id,state;bool incoming;char number[24];};
bool calls_decode(FILE *f,struct call_state *call);
bool call_save(struct call_record *call);
int calls_load(struct call_record *calls,int capacity);
const char *call_result(unsigned result);
#endif
