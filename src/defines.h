#ifndef DEFINES_H
#define DEFINES_H

//Definitions
#define true 1
#define false 0

#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

#define SUPERVISOR_SOCKET_PATH "/tmp/supervisor.sock"
#define CONTROL_SOCKET_PATH_PREFIX "/tmp/cli_runtime-" //!! This will be suffixed with the PID of CLI for unique socket per CLI instance
#define PS_LOGS_PATH "/tmp/supervisor_output.log"

#define STRUCT_STR_LEN 256
#define STACK_SIZE (1024 * 1024) //8 MiB stack for each container

#define BOUNDED_BUFFER_QUEUE_SIZE 256
#define PRODUCER_DATA_LEN 4096

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

//ENUMS
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


typedef enum{
    FAILED,
    RUNNING,
    STOPPED,
    KILLED,
    EXITED
}container_state;

typedef enum{
    ACK,
    NACK,
    FILE_LOC
}supervisor_response_type;

//STRUCTS
typedef struct{
    char id[STRUCT_STR_LEN];
    char container_rootfs[STRUCT_STR_LEN];
    char prog[STRUCT_STR_LEN]; //Command to run for RUN command, not used for START command
    int nice;     //-20 to 19
    int slave_fd; //Used for redirecting container's stdout and stderr to CLI, not used for START command
    int await_fd[2]; //Used for synchronizing between supervisor and child process during startup, not used for RUN command
    int producer_write_fd; //Used for writing logs from the container process to the producer thread, not used for RUN command
    ipc_command_type cmd;
    unsigned long int soft_mib; //in MiB
    unsigned long int hard_mib; //in MiB
    pthread_t producer_thread;
}ipc_payload_client;

typedef struct container_info{
    char id[STRUCT_STR_LEN]; //Main identifier for the hash table, should be unique for each container
    char rootfs[STRUCT_STR_LEN];
    char run_cli_socket_path[STRUCT_STR_LEN]; 
    container_state state;
    unsigned long int soft_mib; //in MiB
    unsigned long int hard_mib; //in MiB
    int exit_code; //used for PS command response
    int exit_signal; //used for PS command response
    int stopped;
    pid_t host_pid;
    int producer_write_fd; //Used for reading logs from the container process, supervisor will read from read_fd[0] and container process will write to read_fd[1]
    pthread_t producer_thread; 
    UT_hash_handle hh;

}container_info;


typedef struct producer_thread_arg{

    char container_id[STRUCT_STR_LEN];
    int producer_read_fd[2]; //Used for reading logs from the container process, supervisor will read from read_fd[0] and container process will write to read_fd[1]
    int pid;

}producer_thread_arg;

typedef struct producer_data{
    int pid;
    char container_id[STRUCT_STR_LEN];
    char produced_data[PRODUCER_DATA_LEN];
}producer_data;

typedef struct bounded_buffer_queue{
    producer_data * buffer[BOUNDED_BUFFER_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t bounded_buffer_mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
}bounded_buffer_queue;

typedef struct supervisor_response{
    supervisor_response_type type;
    container_state state; //State of the Container.
    char data[STRUCT_STR_LEN]; //This will be used for sending additional data for certain commands, like PS command response will send the container state, exit code and exit signal in this field.
}supervisor_response;

//Global variables
extern volatile sig_atomic_t stop_signal_emmited;
extern _Atomic int server_running;
extern char child_stack[STACK_SIZE];
extern int CHILD_FLAGS;
extern char BASE_ROOTFS[STRUCT_STR_LEN]; 
extern struct container_info * containers_list;
extern bounded_buffer_queue logs_queue;


//Locks
extern pthread_mutex_t containers_list_mutex; //Mutex to protect access to the containers hash table

//Semaphores
extern _Atomic int producer_count;


#endif