#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
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

static CmdEntry queue[MAX_QUEUE];
static int queue_size = 0;
static int running = 0;
static int max_parallel = DEFAULT_PARALLEL;
static int shutdown_requested = 0;

static void write_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

static ssize_t write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = (const char *)buf;

    while (total < len) {
        ssize_t n = write(fd, ptr + total, len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }

    return (ssize_t)total;
}

static ssize_t read_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *ptr = (char *)buf;

    while (total < len) {
        ssize_t n = read(fd, ptr + total, len - total);
        if (n == 0) {
            if (total == 0) {
                return 0;
            }
            return -1;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        total += (size_t)n;
    }

    return (ssize_t)total;
}

static int send_response(pid_t runner_pid, const Response *resp) {
    char path[128];
    snprintf(path, sizeof(path), RUNNER_PIPE_FMT, (int)runner_pid);

    int fd = -1;
    int retries = 50;
    while (retries-- > 0) {
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            break;
        }
        if (errno != ENXIO && errno != ENOENT) {
            break;
        }
        usleep(20000);
    }

    if (fd < 0) {
        return -1;
    }

    if (write_all(fd, resp, sizeof(Response)) < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void append_log(const CmdEntry *entry, const struct timeval *end_time) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }

    long sec = (long)(end_time->tv_sec - entry->submit_time.tv_sec);
    long usec = (long)(end_time->tv_usec - entry->submit_time.tv_usec);
    if (usec < 0) {
        sec -= 1;
        usec += 1000000;
    }

    char line[1400];
    int n = snprintf(
        line,
        sizeof(line),
        "user=%s cmd_id=%d duration=%ld.%06ld cmd=%s\n",
        entry->user_id,
        entry->cmd_id,
        sec,
        usec,
        entry->cmd
    );
    if (n > 0) {
        write(fd, line, (size_t)n);
    }

    close(fd);
}

static int remove_index(int index) {
    if (index < 0 || index >= queue_size) {
        return -1;
    }

    for (int i = index; i + 1 < queue_size; ++i) {
        queue[i] = queue[i + 1];
    }
    queue_size--;
    return 0;
}

static void try_dispatch(void) {
    for (int i = 0; i < queue_size && running < max_parallel; ++i) {
        if (queue[i].state == STATE_WAITING) {
            Response resp;
            memset(&resp, 0, sizeof(resp));
            resp.type = RESP_GO;
            resp.cmd_id = queue[i].cmd_id;

            if (send_response(queue[i].runner_pid, &resp) == 0) {
                queue[i].state = STATE_RUNNING;
                running++;
            }
        }
    }
}

static void handle_submit(const Message *msg) {
    if (queue_size >= MAX_QUEUE) {
        return;
    }

    queue[queue_size].cmd_id = msg->cmd_id;
    queue[queue_size].runner_pid = msg->runner_pid;
    queue[queue_size].submit_time = msg->submit_time;
    queue[queue_size].state = STATE_WAITING;

    strncpy(queue[queue_size].user_id, msg->user_id, MAX_USER_LEN - 1);
    queue[queue_size].user_id[MAX_USER_LEN - 1] = '\0';
    strncpy(queue[queue_size].cmd, msg->cmd, MAX_CMD_LEN - 1);
    queue[queue_size].cmd[MAX_CMD_LEN - 1] = '\0';
    queue_size++;
}

static void handle_done(const Message *msg) {
    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].cmd_id == msg->cmd_id && queue[i].runner_pid == msg->runner_pid) {
            if (queue[i].state == STATE_RUNNING && running > 0) {
                running--;
            }

            struct timeval end_time;
            gettimeofday(&end_time, NULL);
            append_log(&queue[i], &end_time);
            remove_index(i);
            return;
        }
    }
}

static void handle_query(pid_t runner_pid) {
    Response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = RESP_QUERY;

    size_t used = 0;
    int n = snprintf(resp.body, sizeof(resp.body), "RUNNING:\n");
    if (n < 0) {
        return;
    }
    used = (size_t)n;

    int have_running = 0;
    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].state == STATE_RUNNING) {
            have_running = 1;
            n = snprintf(resp.body + used, sizeof(resp.body) - used, "cmd_id=%d user=%s\n", queue[i].cmd_id, queue[i].user_id);
            if (n < 0 || (size_t)n >= sizeof(resp.body) - used) {
                used = sizeof(resp.body) - 1;
                break;
            }
            used += (size_t)n;
        }
    }

    if (!have_running && used < sizeof(resp.body)) {
        n = snprintf(resp.body + used, sizeof(resp.body) - used, "(none)\n");
        if (n > 0) {
            used += (size_t)n;
        }
    }

    if (used < sizeof(resp.body)) {
        n = snprintf(resp.body + used, sizeof(resp.body) - used, "WAITING:\n");
        if (n > 0) {
            used += (size_t)n;
        }
    }

    int have_waiting = 0;
    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].state == STATE_WAITING) {
            have_waiting = 1;
            n = snprintf(resp.body + used, sizeof(resp.body) - used, "cmd_id=%d user=%s\n", queue[i].cmd_id, queue[i].user_id);
            if (n < 0 || (size_t)n >= sizeof(resp.body) - used) {
                used = sizeof(resp.body) - 1;
                break;
            }
            used += (size_t)n;
        }
    }

    if (!have_waiting && used < sizeof(resp.body)) {
        snprintf(resp.body + used, sizeof(resp.body) - used, "(none)\n");
    }

    send_response(runner_pid, &resp);
}

static void handle_shutdown(const Message *msg) {
    shutdown_requested = 1;

    Response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = RESP_SHUTDOWN_ACK;
    resp.cmd_id = msg->cmd_id;
    snprintf(resp.body, sizeof(resp.body), "shutdown requested");
    send_response(msg->runner_pid, &resp);
}

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        max_parallel = atoi(argv[1]);
        if (max_parallel <= 0) {
            max_parallel = DEFAULT_PARALLEL;
        }
    }

    mkdir("tmp", 0755);
    unlink(CONTROLLER_PIPE);

    if (mkfifo(CONTROLLER_PIPE, 0666) < 0 && errno != EEXIST) {
        write_str(STDERR_FILENO, "Erro ao criar pipe do controller\n");
        return 1;
    }

    int pipe_fd = open(CONTROLLER_PIPE, O_RDWR);
    if (pipe_fd < 0) {
        write_str(STDERR_FILENO, "Erro ao abrir pipe do controller\n");
        return 1;
    }

    Message msg;
    while (1) {
        ssize_t n = read_all(pipe_fd, &msg, sizeof(Message));
        if (n <= 0) {
            continue;
        }

        if (msg.type == MSG_SUBMIT) {
            handle_submit(&msg);
        } else if (msg.type == MSG_DONE) {
            handle_done(&msg);
        } else if (msg.type == MSG_QUERY) {
            handle_query(msg.runner_pid);
        } else if (msg.type == MSG_SHUTDOWN) {
            handle_shutdown(&msg);
        }

        try_dispatch();

        if (shutdown_requested && queue_size == 0 && running == 0) {
            break;
        }
    }

    close(pipe_fd);
    unlink(CONTROLLER_PIPE);
    return 0;
}


//A primeira parte dos 12 valores esta quase feita (quem quiser que reveja ou melhore)
//Agora convinha fazer o identificador de poitica de escalonamneto no controller como pede à frente 