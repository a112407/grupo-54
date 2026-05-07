#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include "common.h"
#include "util.h"
#include "queue.h"

CmdEntry queue[MAX_QUEUE];
int queue_size = 0;
int running = 0;

/* Regista no ficheiro de log o término de um comando */
void queue_log_completion(const CmdEntry *entry, const struct timeval *end_time) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;

    long sec  = (long)(end_time->tv_sec  - entry->submit_time.tv_sec);
    long usec = (long)(end_time->tv_usec - entry->submit_time.tv_usec);
    if (usec < 0) { sec -= 1; usec += 1000000; }

    char line[1400];
    int n = snprintf(line, sizeof(line),
        "user=%s cmd_id=%d duration=%ld.%06ld cmd=%s\n",
        entry->user_id, entry->cmd_id, sec, usec, entry->cmd);
    if (n > 0) write(fd, line, (size_t)n);

    close(fd);
}

/* Remove entrada da queue deslocando as seguintes */
void remove_from_queue(int index) {
    if (index < 0 || index >= queue_size) return;
    for (int i = index; i + 1 < queue_size; ++i)
        queue[i] = queue[i + 1];
    queue_size--;
}

/* Adiciona um novo comando à queue */
void queue_add_command(const Message *msg) {
    if (queue_size >= MAX_QUEUE) return;

    queue[queue_size].cmd_id      = msg->cmd_id;
    queue[queue_size].runner_pid  = msg->runner_pid;
    queue[queue_size].submit_time = msg->submit_time;
    queue[queue_size].state       = STATE_WAITING;

    strncpy(queue[queue_size].user_id, msg->user_id, MAX_USER_LEN - 1);
    queue[queue_size].user_id[MAX_USER_LEN - 1] = '\0';
    strncpy(queue[queue_size].cmd, msg->cmd, MAX_CMD_LEN - 1);
    queue[queue_size].cmd[MAX_CMD_LEN - 1] = '\0';

    queue_size++;
}

/* Marca um comando como terminado, faz log e liberta a sua vaga */
void queue_mark_done(const Message *msg) {
    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].cmd_id == msg->cmd_id &&
            queue[i].runner_pid == msg->runner_pid) {

            if (queue[i].state == STATE_RUNNING && running > 0)
                running--;

            struct timeval end_time;
            gettimeofday(&end_time, NULL);
            queue_log_completion(&queue[i], &end_time);
            remove_from_queue(i);
            return;
        }
    }
}
