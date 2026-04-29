#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
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
static pid_t shutdown_runner_pid = 0;
static char sched_policy[32] = "fifo";
static char last_user[MAX_USER_LEN] = "";
static int pipe_fd = -1;

static void write_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

/* Garante escrita total, tratando escritas parciais e EINTR */
static ssize_t write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = (const char *)buf;

    while (total < len) {
        ssize_t n = write(fd, ptr + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* Garante leitura total, tratando leituras parciais e EINTR */
static ssize_t read_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *ptr = (char *)buf;

    while (total < len) {
        ssize_t n = read(fd, ptr + total, len - total);
        if (n == 0) return (total == 0) ? 0 : -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/*
 * Envia uma resposta para o FIFO privado do runner.
 * Usa O_NONBLOCK com retries curtos porque o runner abre o seu pipe
 * antes de enviar a mensagem ao controller, pelo que o pipe está
 * quase sempre pronto à primeira tentativa.
 */
static int send_response(pid_t runner_pid, const Response *resp) {
    char path[128];
    snprintf(path, sizeof(path), RUNNER_PIPE_FMT, (int)runner_pid);

    int fd = -1;
    int retries = 50;
    while (retries-- > 0) {
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) break;
        if (errno != ENXIO && errno != ENOENT) break;
        usleep(20000); /* 20ms entre tentativas */
    }

    if (fd < 0) return -1;

    if (write_all(fd, resp, sizeof(Response)) < 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/* Regista no ficheiro de log o término de um comando */
static void append_log(const CmdEntry *entry, const struct timeval *end_time) {
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
static void remove_index(int index) {
    if (index < 0 || index >= queue_size) return;
    for (int i = index; i + 1 < queue_size; ++i)
        queue[i] = queue[i + 1];
    queue_size--;
}

/*
 * Tenta despachar comandos da queue até ao limite de paralelismo.
 *
 * Políticas suportadas:
 *   fifo — despacha pela ordem de chegada (primeiro a entrar, primeiro a sair)
 *   rr   — round-robin por utilizador: tenta alternar entre utilizadores
 *           diferentes do último despachado; se não houver outro, cai em FIFO
 *
 * O send_response do RESP_GO não usa fork porque o runner está bloqueado
 * no read_all à espera deste sinal — o pipe está aberto e o envio é imediato.
 * Só se o send_response falhar é que não marcamos o comando como running,
 * evitando que um slot fique preso para sempre.
 */
static void try_dispatch(void) {
    while (running < max_parallel) {
        int chosen = -1;

        if (strcmp(sched_policy, "rr") == 0) {
            /* 1.ª passagem: procura utilizador diferente do último */
            for (int i = 0; i < queue_size; ++i) {
                if (queue[i].state == STATE_WAITING &&
                    strcmp(queue[i].user_id, last_user) != 0) {
                    chosen = i;
                    break;
                }
            }
            /* 2.ª passagem: se não encontrou, aceita qualquer um (FIFO) */
            if (chosen == -1) {
                for (int i = 0; i < queue_size; ++i) {
                    if (queue[i].state == STATE_WAITING) {
                        chosen = i;
                        break;
                    }
                }
            }
        } else { /* fifo */
            for (int i = 0; i < queue_size; ++i) {
                if (queue[i].state == STATE_WAITING) {
                    chosen = i;
                    break;
                }
            }
        }

        if (chosen == -1) break;

        Response resp;
        memset(&resp, 0, sizeof(resp));
        resp.type   = RESP_GO;
        resp.cmd_id = queue[chosen].cmd_id;

        /* Só atualiza o estado se o envio for bem-sucedido */
        if (send_response(queue[chosen].runner_pid, &resp) == 0) {
            queue[chosen].state = STATE_RUNNING;
            running++;
            strncpy(last_user, queue[chosen].user_id, MAX_USER_LEN - 1);
            last_user[MAX_USER_LEN - 1] = '\0';
        } else {
            break;
        }
    }
}

/* Adiciona um novo comando à queue */
static void handle_submit(const Message *msg) {
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
static void handle_done(const Message *msg) {
    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].cmd_id == msg->cmd_id &&
            queue[i].runner_pid == msg->runner_pid) {

            if (queue[i].state == STATE_RUNNING && running > 0)
                running--;

            struct timeval end_time;
            gettimeofday(&end_time, NULL);
            append_log(&queue[i], &end_time);
            remove_index(i);
            return;
        }
    }
}

/*
 * Responde à query -c com a lista de comandos em execução e em espera.
 *
 * O send_response é feito num processo filho para que o controller
 * regresse imediatamente ao select() e continue a receber mensagens
 * de outros runners enquanto a resposta é entregue.
 * A queue não é modificada pelo filho — só o pai a toca.
 */
static void handle_query(pid_t runner_pid) {
    Response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = RESP_QUERY;

    size_t used = 0;
    int n;

    n = snprintf(resp.body + used, sizeof(resp.body) - used, "---\nExecuting\n");
    if (n > 0) used += (size_t)n;

    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].state == STATE_RUNNING) {
            n = snprintf(resp.body + used, sizeof(resp.body) - used,
                "user-id %s - command-id %d\n",
                queue[i].user_id, queue[i].cmd_id);
            if (n < 0 || (size_t)n >= sizeof(resp.body) - used) break;
            used += (size_t)n;
        }
    }

    if (used < sizeof(resp.body)) {
        n = snprintf(resp.body + used, sizeof(resp.body) - used, "---\nScheduled\n");
        if (n > 0) used += (size_t)n;
    }

    for (int i = 0; i < queue_size; ++i) {
        if (queue[i].state == STATE_WAITING) {
            n = snprintf(resp.body + used, sizeof(resp.body) - used,
                "user-id %s - command-id %d\n",
                queue[i].user_id, queue[i].cmd_id);
            if (n < 0 || (size_t)n >= sizeof(resp.body) - used) break;
            used += (size_t)n;
        }
    }

    /* Fork só para o envio — o pai regressa imediatamente ao select() */
    pid_t child = fork();
    if (child == 0) {
        send_response(runner_pid, &resp);
        _exit(0);
    }
}

/* Regista o pedido de shutdown — o ACK só é enviado quando a queue esvaziar */
static void handle_shutdown(const Message *msg) {
    shutdown_requested   = 1;
    shutdown_runner_pid  = msg->runner_pid;
}

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        max_parallel = atoi(argv[1]);
        if (max_parallel <= 0) max_parallel = DEFAULT_PARALLEL;
    }

    if (argc >= 3) {
        strncpy(sched_policy, argv[2], sizeof(sched_policy) - 1);
        sched_policy[sizeof(sched_policy) - 1] = '\0';
    }

    mkdir("tmp", 0755);
    unlink(CONTROLLER_PIPE);

    if (mkfifo(CONTROLLER_PIPE, 0666) < 0 && errno != EEXIST) {
        write_str(STDERR_FILENO, "Erro ao criar pipe do controller\n");
        return 1;
    }

    pipe_fd = open(CONTROLLER_PIPE, O_RDWR | O_NONBLOCK);
    if (pipe_fd < 0) {
        write_str(STDERR_FILENO, "Erro ao abrir pipe do controller\n");
        return 1;
    }

    /*
     * SIG_IGN no SIGCHLD faz com que o kernel limpe automaticamente
     * os filhos que terminam (sem criar zombies), sem precisar de waitpid.
     */
    signal(SIGCHLD, SIG_IGN);

    Message msg;
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pipe_fd, &readfds);

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ret = select(pipe_fd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret == 0) {
            /* timeout: verifica condição de shutdown */
            if (shutdown_requested && queue_size == 0 && running == 0) {
                if (shutdown_runner_pid > 0) {
                    Response resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.type = RESP_SHUTDOWN_ACK;
                    /* Fork para o ACK também, por consistência */
                    pid_t child = fork();
                    if (child == 0) {
                        send_response(shutdown_runner_pid, &resp);
                        _exit(0);
                    }
                }
                break;
            }
            continue;
        }

        ssize_t n = read_all(pipe_fd, &msg, sizeof(Message));
        if (n <= 0) continue;

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
    }

    close(pipe_fd);
    unlink(CONTROLLER_PIPE);
    return 0;
}