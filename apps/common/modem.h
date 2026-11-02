// SPDX-License-Identifier: GPL-2.0-only
#ifndef YAMA_MODEM_H
#define YAMA_MODEM_H

#include <stdbool.h>
#include <sys/types.h>

enum modem_action{MODEM_STATUS,MODEM_CALL,MODEM_SMS};
struct modem_job{pid_t pid;int fd,result;enum modem_action action;bool running,delivered,accepted;char line[768],status[192];unsigned used;};
bool modem_number(const char *number);
bool modem_job_start(struct modem_job *job,enum modem_action action,const char *number,const char *body);
bool modem_job_poll(struct modem_job *job);
void modem_job_stop(struct modem_job *job);
void modem_job_close(struct modem_job *job);
void modem_backspace(char *value);
bool modem_append(char *value,unsigned capacity,const char *utf8,bool number);
#endif
