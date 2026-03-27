#ifndef DEFINES_H
#define DEFINES_H

//Definitions
#define true 1
#define false 0

#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

#define SUPERVISOR_SOCKET_PATH "/tmp/supervisor.sock"
#define CONTROL_SOCKET_PATH_PREFIX "/tmp/cli_runtime-" //!! This will be suffixed with the PID of CLI for unique socket per CLI instance

#define STRUCT_STR_LEN 256
#define STACK_SIZE (1024 * 1024) //8 MiB stack for each container

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
    char id[STRUCT_STR_LEN];
    char container_rootfs[STRUCT_STR_LEN];
    char prog[STRUCT_STR_LEN]; //Command to run for RUN command, not used for START command
    int nice;     //-20 to 19
    int slave_fd; //Used for redirecting container's stdout and stderr to CLI, not used for START command
    int await_fd[2]; //Used for synchronizing between supervisor and child process during startup, not used for RUN command
    ipc_command_type cmd;
    unsigned long int soft_mib; //in MiB
    unsigned long int hard_mib; //in MiB
}ipc_payload_client;

typedef enum{
    RUNNING,
    STOPPED,
    KILLED,
    EXITED
}container_state;

typedef struct container_info{
    char id[STRUCT_STR_LEN];
    char rootfs[STRUCT_STR_LEN];
    char run_cli_socket_path[STRUCT_STR_LEN]; 
    container_state state;
    unsigned long int soft_mib; //in MiB
    unsigned long int hard_mib; //in MiB
    int exit_code; //used for PS command response
    int exit_signal; //used for PS command response
    pid_t host_pid;
    UT_hash_handle hh;

}container_info;

//Global variables
volatile sig_atomic_t stop_signal_emmited = false;
_Atomic int server_running = true; //This will be set 
char child_stack[STACK_SIZE];
int CHILD_FLAGS = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD; //Namespace flags for clone
char BASE_ROOTFS[STRUCT_STR_LEN]; //This will be set by supervisor command line argument
struct container_info * containers_list = NULL; //Hash table to store container info, keyed by container ID

//Locks
pthread_mutex_t containers_list_mutex = PTHREAD_MUTEX_INITIALIZER; //Mutex to protect access to the containers hash table


#endif