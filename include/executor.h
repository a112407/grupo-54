#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "common.h"

/* Separa a string cmd em argumentos, preenche args[] e devolve o número */
int parse_command(char *cmd, char *args[], int max_args);

/* Processa redirects no array args (>, <, 2>) e aplica dup2 */
int apply_redirects(char *args[]);

/* Executa um comando (potencialmente com pipes e redirects) */
int execute_command(const char *cmd_str);

#endif /* EXECUTOR_H */
