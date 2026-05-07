#ifndef QUEUE_H
#define QUEUE_H

#include <sys/types.h>
#include <sys/time.h>
#include "common.h"

typedef struct {
    int cmd_id;
    char user_id[MAX_USER_LEN];
    char cmd[MAX_CMD_LEN];
    pid_t runner_pid;
    int state;
    struct timeval submit_time;
} CmdEntry;

enum {
    STATE_WAITING = 0,
    STATE_RUNNING = 1
};

/* Fila de comandos (acessível ao controller) */
extern CmdEntry queue[MAX_QUEUE];
extern int queue_size;
extern int running;

/* Funções de manipulação da queue */
void remove_from_queue(int index);
void queue_add_command(const Message *msg);
void queue_mark_done(const Message *msg);
void queue_log_completion(const CmdEntry *entry, const struct timeval *end_time);

#endif /* QUEUE_H */
