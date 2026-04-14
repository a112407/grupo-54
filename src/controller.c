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

void send_response(pid_t runner_pid, Response *resp) {
    char path[128];
 
    snprintf(path, sizeof(path), RUNNER_PIPE_FMT, (int)runner_pid);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;  
    
    write(fd, resp, sizeof(Response));
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

            case MSG_SUBMIT:
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

            case MSG_DONE:
                break;

            case MSG_QUERY:  
                break;

            case MSG_SHUTDOWN:  
                break;
        }
    }

    close(pipe_fd);
    unlink(CONTROLLER_PIPE);

    return 0;
}