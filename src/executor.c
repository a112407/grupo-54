#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include "common.h"
#include "executor.h"

/* Separa a string cmd em argumentos, preenche args[] e devolve o número */
int parse_command(char *cmd, char *args[], int max_args) {
    int count = 0;
    char *token = strtok(cmd, " \t");

    while (token != NULL && count < max_args - 1) {
        args[count++] = token;
        token = strtok(NULL, " \t");
    }
    args[count] = NULL;   /* execvp exige que o último seja NULL */
    return count;
}

/* Processa redirects no array args. Remove os operadores e ficheiros
 * do array, e aplica o dup2 para redirecionar stdin/stdout/stderr.
 * Devolve 0 em sucesso, -1 em erro. */
int apply_redirects(char *args[]) {
    int i = 0;
    int j = 0;

    while (args[i] != NULL) {
        if (strcmp(args[i], ">") == 0 && args[i + 1] != NULL) {
            /* redirect de stdout */
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) return -1;
            dup2(fd, STDOUT_FILENO);
            close(fd);
            i += 2;   /* saltar ">" e o ficheiro */
        }
        else if (strcmp(args[i], "<") == 0 && args[i + 1] != NULL) {
            /* redirect de stdin */
            int fd = open(args[i + 1], O_RDONLY);
            if (fd < 0) return -1;
            dup2(fd, STDIN_FILENO);
            close(fd);
            i += 2;
        }
        else if (strcmp(args[i], "2>") == 0 && args[i + 1] != NULL) {
            /* redirect de stderr */
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) return -1;
            dup2(fd, STDERR_FILENO);
            close(fd);
            i += 2;
        }
        else {
            /* argumento normal - mantém no array */
            args[j++] = args[i++];
        }
    }
    args[j] = NULL;   /* terminar o array no novo tamanho */
    return 0;
}

/* Executa um comando (potencialmente com pipes e redirects) */
int execute_command(const char *cmd_str) {
    char cmd_copy[MAX_CMD_LEN];
    strncpy(cmd_copy, cmd_str, MAX_CMD_LEN - 1);
    cmd_copy[MAX_CMD_LEN - 1] = '\0';

    char *args[MAX_ARGS];
    parse_command(cmd_copy, args, MAX_ARGS);

    if (args[0] == NULL) return 1;

    /* Contar tokens e localizar pipes (|) */
    int token_count = 0;
    while (args[token_count] != NULL && token_count < MAX_ARGS) token_count++;
    int pipe_idx[MAX_ARGS];
    int npipes = 0;
    for (int i = 0; i < token_count; ++i) {
        if (strcmp(args[i], "|") == 0) {
            pipe_idx[npipes++] = i;
        }
    }

    /* Sem pipes: aplicar redirects e executar normalmente */
    if (npipes == 0) {
        if (apply_redirects(args) < 0) return 1;
        execvp(args[0], args);
        return 127;   /* só retorna se execvp falhar */
    }

    /* Preparar segmentos: substituir '|' por NULL e construir ponteiros */
    int nseg = npipes + 1;
    char **seg_ptrs[nseg];
    seg_ptrs[0] = args;
    for (int k = 0; k < npipes; ++k) {
        int idx = pipe_idx[k];
        args[idx] = NULL; /* termina segmento k */
        seg_ptrs[k+1] = &args[idx + 1];
    }

    /* Criar pipes (npipes) */
    int fds[npipes][2];
    for (int k = 0; k < npipes; ++k) {
        if (pipe(fds[k]) < 0) return 1;
    }

    /* Fork para cada segmento */
    pid_t children[nseg];
    for (int s = 0; s < nseg; ++s) {
        pid_t c = fork();
        if (c < 0) {
            /* falha no fork: termina todos e sai */
            for (int kk = 0; kk < s; ++kk) waitpid(children[kk], NULL, 0);
            return 1;
        }
        if (c == 0) {
            /* Filho do segmento s */
            /* Ligar stdin/stdout aos pipes apropriados */
            if (s > 0) {
                /* ler do pipe anterior */
                if (dup2(fds[s-1][0], STDIN_FILENO) < 0) return 1;
            }
            if (s < nseg - 1) {
                /* escrever para o pipe seguinte */
                if (dup2(fds[s][1], STDOUT_FILENO) < 0) return 1;
            }

            /* fechar todas os descritores dos pipes (pai/filho) */
            for (int kk = 0; kk < npipes; ++kk) {
                close(fds[kk][0]);
                close(fds[kk][1]);
            }

            /* aplicar redirects só no segmento atual */
            if (apply_redirects(seg_ptrs[s]) < 0) return 1;
            if (seg_ptrs[s][0] == NULL) return 1;
            execvp(seg_ptrs[s][0], seg_ptrs[s]);
            return 127;   /* só retorna se execvp falhar */
        }
        /* pai guarda pid */
        children[s] = c;
    }

    /* Pai fecha todos os descritores dos pipes */
    for (int k = 0; k < npipes; ++k) {
        close(fds[k][0]);
        close(fds[k][1]);
    }

    /* Espera pelos filhos do pipeline */
    for (int s = 0; s < nseg; ++s) {
        int st;
        while (waitpid(children[s], &st, 0) < 0) {
            if (errno != EINTR) break;
        }
    }

    return 0;   /* sucesso */
}
