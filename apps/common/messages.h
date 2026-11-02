// SPDX-License-Identifier: GPL-2.0-only
#ifndef YAMA_MESSAGES_H
#define YAMA_MESSAGES_H

#include <stdint.h>

#define MESSAGE_TEXT 2681
#define MESSAGE_LIMIT 512

enum{MESSAGE_PENDING,MESSAGE_SENT,MESSAGE_UNCONFIRMED};
struct message{uint32_t magic,outgoing,status;uint64_t time;char number[24],body[MESSAGE_TEXT],id[48];};
int messages_load(struct message *list,int capacity);
bool message_save(struct message *message);
#endif
