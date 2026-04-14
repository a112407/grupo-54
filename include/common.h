#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#define CONTROLLER_PIPE  "tmp/controller.pipe"
#define RUNNER_PIPE_FMT  "tmp/runner_%d.pipe"
#define LOG_FILE         "tmp/history.log"

#define MAX_CMD_LEN   1024
#define MAX_ARGS      64
#define MAX_USER_LEN  64
#define MAX_QUERY_BUF 4096

typedef enum {
    MSG_SUBMIT = 1,
    MSG_DONE = 2,
    MSG_QUERY = 3,
    MSG_SHUTDOWN = 4
} MessageType;

typedef enum {
    RESP_GO = 10,
    RESP_QUERY = 11,
    RESP_SHUTDOWN_ACK = 12
} ResponseType;

// mensagem runner->controller
typedef struct {
    MessageType type;              // tipo do pedido (MSG_*)        
    pid_t runner_pid;              // PID do runner                 
    int   cmd_id;                  // identificador único           
    char  user_id[MAX_USER_LEN];   // identificador do utilizador   
    char  cmd[MAX_CMD_LEN];        // comando a executar            
    struct timeval submit_time;    // timestamp de submissão        
} Message;

// resposta controller->runner
typedef struct {
    ResponseType type;           // tipo da resposta (RESP_*)        
    int  cmd_id;                 // identificador do comando         
    char body[MAX_QUERY_BUF];    // conteúdo extra (usado na query)  
} Response;

#endif /* COMMON_H */