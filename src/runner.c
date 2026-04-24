#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>
#include "common.h"

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

static int open_runner_pipe(char *path, size_t path_len) {
	pid_t pid = getpid();
	snprintf(path, path_len, RUNNER_PIPE_FMT, (int)pid);

	unlink(path);
	if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
		return -1;
	}

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		unlink(path);
		return -1;
	}

	return fd;
}

static int send_message(const Message *msg) {
	int fd = open(CONTROLLER_PIPE, O_WRONLY);
	if (fd < 0) {
		return -1;
	}

	int ok = (write_all(fd, msg, sizeof(Message)) == (ssize_t)sizeof(Message));
	close(fd);
	return ok ? 0 : -1;
}

static int generate_cmd_id(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int id = (int)(((tv.tv_sec & 0xFFFF) << 16) ^ (tv.tv_usec & 0xFFFF) ^ (getpid() & 0x7FFF));
    if (id < 0) id = -id;   /* garantir que é positivo */
    return id;
}

static void build_cmd_string(int argc, char *argv[], int from, char *dst, size_t dst_size) {
	size_t used = 0;
	dst[0] = '\0';

	for (int i = from; i < argc; ++i) {
		int n = snprintf(dst + used, dst_size - used, "%s%s", (i == from) ? "" : " ", argv[i]);
		if (n < 0 || (size_t)n >= dst_size - used) {
			dst[dst_size - 1] = '\0';
			return;
		}
		used += (size_t)n;
	}
}

/* separa a string cmd em argumentos, preenche args[] e devolve quantos */
static int parse_command(char *cmd, char *args[], int max_args) {
    int count = 0;
    char *token = strtok(cmd, " \t");

    while (token != NULL && count < max_args - 1) {
        args[count++] = token;
        token = strtok(NULL, " \t");
    }
    args[count] = NULL;   /* execvp exige que o último seja NULL */
    return count;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		write_str(STDERR_FILENO, "Uso: ./runner -e <user> <cmd> [args...] | -c | -s\n");
		return 1;
	}

	mkdir("tmp", 0755);

	if (strcmp(argv[1], "-e") == 0) {
		if (argc < 4) {
			write_str(STDERR_FILENO, "Uso: ./runner -e <user> <cmd> [args...]\n");
			return 1;
		}

		char runner_pipe[128];
		int runner_fd = open_runner_pipe(runner_pipe, sizeof(runner_pipe));
		if (runner_fd < 0) {
			write_str(STDERR_FILENO, "Erro ao criar pipe do runner\n");
			return 1;
		}

		Message msg;
		memset(&msg, 0, sizeof(msg));
		msg.type = MSG_SUBMIT;
		msg.runner_pid = getpid();
		msg.cmd_id = generate_cmd_id();
		strncpy(msg.user_id, argv[2], MAX_USER_LEN - 1);
		build_cmd_string(argc, argv, 3, msg.cmd, sizeof(msg.cmd));
		gettimeofday(&msg.submit_time, NULL);

		char line[128];
		int len = snprintf(line, sizeof(line), "[runner] command %d submitted\n", msg.cmd_id);
		write(STDOUT_FILENO, line, len);

		if (send_message(&msg) < 0) {
			write_str(STDERR_FILENO, "Erro ao contactar controller\n");
			close(runner_fd);
			unlink(runner_pipe);
			return 1;
		}

		Response resp;
		if (read_all(runner_fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp) || resp.type != RESP_GO) {
			write_str(STDERR_FILENO, "Resposta invalida do controller\n");
			close(runner_fd);
			unlink(runner_pipe);
			return 1;
		}

		len = snprintf(line, sizeof(line), "[runner] executing command %d...\n", msg.cmd_id);
		write(STDOUT_FILENO, line, len);

		pid_t child = fork();
		if (child == 0) {
    		char cmd_copy[MAX_CMD_LEN];
    		strncpy(cmd_copy, msg.cmd, MAX_CMD_LEN - 1);
    		cmd_copy[MAX_CMD_LEN - 1] = '\0';

    		char *args[MAX_ARGS];
    		parse_command(cmd_copy, args, MAX_ARGS);

    		if (args[0] == NULL) {
        		_exit(1);  
    		}

    		execvp(args[0], args);
    		_exit(127);    
}

		if (child < 0) {
			write_str(STDERR_FILENO, "Erro ao executar comando\n");
		} else {
			int status;
			while (waitpid(child, &status, 0) < 0) {
				if (errno != EINTR) {
					break;
				}
			}
		}

		Message done;
		memset(&done, 0, sizeof(done));
		done.type = MSG_DONE;
		done.runner_pid = getpid();
		done.cmd_id = msg.cmd_id;
		strncpy(done.user_id, msg.user_id, MAX_USER_LEN - 1);
		strncpy(done.cmd, msg.cmd, MAX_CMD_LEN - 1);
		done.submit_time = msg.submit_time;
		send_message(&done);

		len = snprintf(line, sizeof(line), "[runner] command %d finished\n", msg.cmd_id);
		write(STDOUT_FILENO, line, len);

		close(runner_fd);
		unlink(runner_pipe);
		return 0;
	}

	if (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "-s") == 0) {
		char runner_pipe[128];
		int runner_fd = open_runner_pipe(runner_pipe, sizeof(runner_pipe));
		if (runner_fd < 0) {
			write_str(STDERR_FILENO, "Erro ao criar pipe do runner\n");
			return 1;
		}

		Message msg;
		memset(&msg, 0, sizeof(msg));
		msg.type = (strcmp(argv[1], "-c") == 0) ? MSG_QUERY : MSG_SHUTDOWN;
		msg.runner_pid = getpid();
		msg.cmd_id = generate_cmd_id();
		strncpy(msg.user_id, "admin", MAX_USER_LEN - 1);
		gettimeofday(&msg.submit_time, NULL);

		 if (msg.type == MSG_SHUTDOWN) {
        	write_str(STDOUT_FILENO, "[runner] sent shutdown notification\n");
   		}

		if (send_message(&msg) < 0) {
			write_str(STDERR_FILENO, "Erro ao contactar controller\n");
			close(runner_fd);
			unlink(runner_pipe);
			return 1;
		}

		if (msg.type == MSG_SHUTDOWN) {
        	write_str(STDOUT_FILENO, "[runner] waiting for controller to shutdown...\n");
    	}

		Response resp;
		if (read_all(runner_fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
			write_str(STDERR_FILENO, "Erro a ler resposta do controller\n");
			close(runner_fd);
			unlink(runner_pipe);
			return 1;
		}

		if (msg.type == MSG_QUERY && resp.type == RESP_QUERY) {
			write_str(STDOUT_FILENO, resp.body);
		} else if (msg.type == MSG_SHUTDOWN && resp.type == RESP_SHUTDOWN_ACK) {
			write_str(STDOUT_FILENO, "Controller notificado para terminar\n");
		} else {
			write_str(STDERR_FILENO, "Resposta inesperada do controller\n");
			close(runner_fd);
			unlink(runner_pipe);
			return 1;
		}

		close(runner_fd);
		unlink(runner_pipe);
		return 0;
	}

	write_str(STDERR_FILENO, "Opcao invalida. Use -e, -c ou -s\n");
	return 1;
}
