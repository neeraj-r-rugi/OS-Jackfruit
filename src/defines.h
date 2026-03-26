#ifndef DEFINES_H
#define DEFINES_H

//Definitions
#define true 1
#define false 0

#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

#define SUPERVISOR_SOCKET_PATH "/tmp/supervisor.sock"
#define CONTROL_SOCKET_PATH_PREFIX "/tmp/cli_runtime-" //!! This will be suffixed with the PID of CLI for unique socket per CLI instance

//PANIC macros for supervisor and client
#define PANIC(...)                                                      \
    do {                                                                \
        fprintf(stderr, "\033[01;31mSUPERVISOR PANIC:\033[0m \n");        \
        perror(__VA_ARGS__);                                            \
        exit(1);                                                        \
    } while (0)

#define PANIC_CLIENT(...)                                                      \
    do {                                                                \
        fprintf(stderr, "\033[01;31mCLI PANIC:\033[0m \n");        \
        perror(__VA_ARGS__);                                            \
        exit(1);                                                        \
    } while (0)

//structs
typedef enum{
    CLI_SUPERVISOR,
    CLI_START,
    CLI_RUN,
    CLI_PS,
    CLI_LOGS,
    CLI_STOP
}cli_command_type;

typedef enum{
    START,
    RUN,
    PS,
    LOGS,
    STOP,
    ERROR,
    SUCCESS
}ipc_command_type;

typedef struct{
    char id[256];
    char container_rootfs[256];
    int nice;     //-20 to 19
    ipc_command_type cmd;
    unsigned long int soft_mib; //in MiB
    unsigned long int hard_mib; //in MiB
}ipc_payload_client;

//Global variables
volatile sig_atomic_t stop_signal_emmited = false;
_Atomic int server_running = true; //This will be set 


#endif