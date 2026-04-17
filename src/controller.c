#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "common.h"

#define MAX_QUEUE 100  

typedef struct {
    int   cmd_id;
    char  user_id[MAX_USER_LEN];
    char  cmd[MAX_CMD_LEN];
    pid_t runner_pid;
    int   state;        
    struct timeval submit_time;
} CmdEntry;

CmdEntry queue[MAX_QUEUE];  
int queue_size = 0;         
int running = 0;
int shutdown_pending = 0;    
pid_t shutdown_runner = 0;            

void send_response(pid_t runner_pid, Response *resp) {
    char path[128];
 
    snprintf(path, sizeof(path), RUNNER_PIPE_FMT, (int)runner_pid);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;  
    
    write(fd, resp, sizeof(Response));
    close(fd);
}

void write_log(CmdEntry *entry) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;

    struct timeval now;
    gettimeofday(&now, NULL);

    long elapsed = (now.tv_sec  - entry->submit_time.tv_sec)  * 1000
                 + (now.tv_usec - entry->submit_time.tv_usec) / 1000;

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "user=%s cmd_id=%d elapsed_ms=%ld cmd=%s\n",
        entry->user_id, entry->cmd_id, elapsed, entry->cmd);

    write(fd, buf, len);
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        char *msg = "Uso: ./controller <parallel-commands> <sched-policy>\n";
        write(STDERR_FILENO, msg, strlen(msg));
        return 1;
    }

    int max_parallel = atoi(argv[1]);  
    char *policy = argv[2];            

    mkdir("tmp", 0755);

    unlink(CONTROLLER_PIPE);

    if (mkfifo(CONTROLLER_PIPE, 0666) < 0) {
        char *err = "Erro ao criar pipe\n";
        write(STDERR_FILENO, err, strlen(err));
        return 1;
    }

    int pipe_fd = open(CONTROLLER_PIPE, O_RDONLY);
    if (pipe_fd < 0) {
        char *err = "Erro ao abrir pipe\n";
        write(STDERR_FILENO, err, strlen(err));
        return 1;
    }

    Message msg;
    ssize_t n;

    while ((n = read(pipe_fd, &msg, sizeof(Message))) > 0) {
        if (n < (ssize_t)sizeof(Message)) continue;

        switch (msg.type) {

            case MSG_SUBMIT: {
                if (queue_size < MAX_QUEUE) {
                    queue[queue_size].cmd_id      = msg.cmd_id;
                    queue[queue_size].runner_pid  = msg.runner_pid;
                    queue[queue_size].submit_time = msg.submit_time;
                    strncpy(queue[queue_size].user_id, msg.user_id, MAX_USER_LEN - 1);
                    strncpy(queue[queue_size].cmd,     msg.cmd,     MAX_CMD_LEN  - 1);

                    if (running < max_parallel) {
                        queue[queue_size].state = 1;
                        running++;

                        Response resp;
                        memset(&resp, 0, sizeof(resp));
                        resp.type   = RESP_GO;
                        resp.cmd_id = msg.cmd_id;
                        send_response(msg.runner_pid, &resp);
                    } else {
                        queue[queue_size].state = 0;
                    }
                    queue_size++;
                }
                break;
            }    
            
            case MSG_DONE: {
                int i;
                for (i = 0; i < queue_size; i++) {
                    if (queue[i].cmd_id == msg.cmd_id) break;
                }

                if (i < queue_size) {
                    write_log(&queue[i]);
                    queue[i] = queue[queue_size - 1];
                    queue_size--;
                    running--;

                    for (int j = 0; j < queue_size; j++) {
                        if (queue[j].state == 0) {
                            queue[j].state = 1;
                            running++;

                            Response resp;
                            memset(&resp, 0, sizeof(resp));
                            resp.type   = RESP_GO;
                            resp.cmd_id = queue[j].cmd_id;
                            send_response(queue[j].runner_pid, &resp);
                            break;
                        }
                    }
                }

                if (shutdown_pending && running == 0 && queue_size == 0) {
                    Response resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.type = RESP_SHUTDOWN_ACK;
                    send_response(shutdown_runner, &resp);

                    close(pipe_fd);
                    unlink(CONTROLLER_PIPE);
                    exit(0);
                }
                break;
            }    

            case MSG_QUERY: {
                Response resp;
                memset(&resp, 0, sizeof(resp));
                resp.type = RESP_QUERY;

                int offset = 0;

                offset += snprintf(resp.body + offset, sizeof(resp.body) - offset,
                    "---\nExecuting\n");

                for (int i = 0; i < queue_size; i++) {
                    if (queue[i].state == 1) {
                        offset += snprintf(resp.body + offset, sizeof(resp.body) - offset,
                            "user-id %s - command-id %d\n",
                            queue[i].user_id, queue[i].cmd_id);
                    }
                }

                offset += snprintf(resp.body + offset, sizeof(resp.body) - offset,
                    "---\nScheduled\n");

                for (int i = 0; i < queue_size; i++) {
                    if (queue[i].state == 0) {
                        offset += snprintf(resp.body + offset, sizeof(resp.body) - offset,
                            "user-id %s - command-id %d\n",
                            queue[i].user_id, queue[i].cmd_id);
                    }
                }
                send_response(msg.runner_pid, &resp);
                break;
            }

            case MSG_SHUTDOWN: {
                shutdown_pending = 1;
                shutdown_runner  = msg.runner_pid;

                if (running == 0 && queue_size == 0) {
                    Response resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.type = RESP_SHUTDOWN_ACK;
                    send_response(shutdown_runner, &resp);

                    close(pipe_fd);
                    unlink(CONTROLLER_PIPE);
                    exit(0);
                }
                break;
            }
        }
    }

    close(pipe_fd);
    unlink(CONTROLLER_PIPE);

    return 0;
}