#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <search.h>
#include <sys/resource.h>
#include <pty.h>
#include "uthash.h"
#include "defines.h"

/*
! --- TODO: 1 --- Implement a semaphore for the producers, that the consumer checks before exiting to ensure that
!                 all produced logs are consumed before the consumer thread exits.
! --- TODO: 2 --- Implement the remaining CLI commands and their corresponding logic in the supervisor.
! --- TODO: 3 --- Implement dynamic stack allocation for `clone()` to prevent corruption on concurrent start/run commands.
! --- TODO: 4 --- Implement the recevie side logic for all the commands.
*/

// File Specific Global Variables
volatile sig_atomic_t stop_signal_emmited = false;
_Atomic int server_running = true; //This will be set 
char child_stack[STACK_SIZE];
int CHILD_FLAGS = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD; //Namespace flags for clone
char BASE_ROOTFS[STRUCT_STR_LEN]; //This will be set by supervisor command line argument
struct container_info * containers_list = NULL; //Hash table to store container info, keyed by container ID
bounded_buffer_queue logs_queue;
pthread_mutex_t containers_list_mutex;

//Semaphore
_Atomic int producer_count = 0; //This will keep track of the number of active producer threads, used for graceful shutdown of consumer thread



/* --------------------------------------------------------------------------
 * Helper Functions
 * -------------------------------------------------------------------------- */
static void usage(const char *prog_name) {
  fprintf(stderr,
          "Usage:\n"
          "  %s supervisor <base-rootfs>\n"
          "  %s start <id> <container-rootfs> <command> [--soft-mib N] "
          "[--hard-mib N] [--nice N]\n"
          "  %s run <id> <container-rootfs> <command> [--soft-mib N] "
          "[--hard-mib N] [--nice N]\n"
          "  %s ps\n"
          "  %s logs <id>\n"
          "  %s stop <id>\n",
          prog_name, prog_name, prog_name, prog_name, prog_name, prog_name);
}

cli_command_type parse_command(const char *cmd_str, const char *prog_name) {

  cli_command_type cmd = CLI_SUPERVISOR;
  if (strcmp(cmd_str, "supervisor") == 0) {
    cmd = CLI_SUPERVISOR;
    return cmd;
  }
  if (strcmp(cmd_str, "start") == 0) {
    cmd = CLI_START;
    return cmd;
  }
  if (strcmp(cmd_str, "run") == 0) {
    cmd = CLI_RUN;
    return cmd;
  }
  if (strcmp(cmd_str, "ps") == 0) {
    cmd = CLI_PS;
    return cmd;
  }
  if (strcmp(cmd_str, "logs") == 0) {
    cmd = CLI_LOGS;
    return cmd;
  }
  if (strcmp(cmd_str, "stop") == 0) {
    cmd = CLI_STOP;
    return cmd;
  }
  usage(prog_name);
  exit(EXIT_FAILURE);
}
static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *target_bytes) {
  char *end = NULL;
  unsigned long mib;

  errno = 0;
  mib = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
    return -1;
  }

  if (mib > ULONG_MAX / (1UL << 20)) {
    fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
    return -1;
  }

  *target_bytes = mib * (1UL << 20);
  return 0;
}
static int parse_optional_flags(ipc_payload_client *req, int argc, char *argv[],
                                int start_index) {
  int i;

  for (i = start_index; i < argc; i += 2) {
    char *end = NULL;
    long nice_value;

    if (i + 1 >= argc) {
      fprintf(stderr, "Missing value for option: %s\n", argv[i]);
      return -1;
    }

    if (strcmp(argv[i], "--soft-mib") == 0) {
      if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_mib) != 0)
        return -1;
      continue;
    }

    if (strcmp(argv[i], "--hard-mib") == 0) {
      if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_mib) != 0)
        return -1;
      continue;
    }

    if (strcmp(argv[i], "--nice") == 0) {
      errno = 0;
      nice_value = strtol(argv[i + 1], &end, 10);
      if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
          nice_value < -20 || nice_value > 19) {
        fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n",
                argv[i + 1]);
        return -1;
      }
      req->nice = (int)nice_value;
      continue;
    }

    fprintf(stderr, "Unknown option: %s\n", argv[i]);
    return -1;
  }

  if (req->soft_mib > req->hard_mib) {
    fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
    return -1;
  }

  return 0;
}
void write_file(char *path, char *data) {
    int fd = open(path, O_WRONLY);
    write(fd, data, strlen(data));
    close(fd);
}
void register_child_proc_with_kernel(pid_t child_pid, ipc_payload_client *payload){
    close(payload->await_fd[0]);
    char path[100], map[100];
    sprintf(path, "/proc/%d/setgroups", child_pid);
    write_file(path, "deny");

    sprintf(path, "/proc/%d/uid_map", child_pid);
    sprintf(map, "0 %d 1", getuid());
    write_file(path, map);

    sprintf(path, "/proc/%d/gid_map", child_pid);
    sprintf(map, "0 %d 1", getgid());
    write_file(path, map);
    write(payload->await_fd[1], "x", 1); //Signal the child that it can proceed with execution after setting up user namespace mappings
}


/* --------------------------------------------------------------------------
 * Bounded Buffer Functions
 * -------------------------------------------------------------------------- */
int is_buffer_empty(){
  pthread_mutex_lock(&logs_queue.bounded_buffer_mutex);
  int empty = (logs_queue.count == 0)?true : false;
  pthread_mutex_unlock(&logs_queue.bounded_buffer_mutex);
  return empty;
}

void manipulate_bounded_buffer_queue(producer_data * data, int add_to_buffer){
  pthread_mutex_lock(&logs_queue.bounded_buffer_mutex);
  //Crtical Section, combined producer and consumer logic so that we dont kill ourselves debugging.
  if(add_to_buffer){
    while(logs_queue.count == BOUNDED_BUFFER_QUEUE_SIZE){
      pthread_cond_wait(&logs_queue.not_full, &logs_queue.bounded_buffer_mutex);
    }
      producer_data * slot = malloc(sizeof(producer_data));
      memcpy(slot, data, sizeof(producer_data));
      logs_queue.buffer[logs_queue.tail] = slot;
      logs_queue.tail = (logs_queue.tail + 1) % BOUNDED_BUFFER_QUEUE_SIZE;
      logs_queue.count++;
      pthread_cond_signal(&logs_queue.not_empty);
  }else{
    while(logs_queue.count == 0){
      pthread_cond_wait(&logs_queue.not_empty, &logs_queue.bounded_buffer_mutex);
    }
    producer_data * slot = logs_queue.buffer[logs_queue.head];
    char id [STRUCT_STR_LEN];
    char produced_data[PRODUCER_DATA_LEN];
    memcpy(id, slot->container_id, sizeof(id));
    memcpy(produced_data, slot->produced_data, sizeof(produced_data));
    int n = snprintf(NULL, 0, "/tmp/%s.log", id) + 1;
    char path[n];
    snprintf(path, n, "/tmp/%s.log", id);
    FILE *f = fopen(path, "a");
    fprintf(f, "%s", produced_data);
    fclose(f);
    free(slot);
    logs_queue.head = (logs_queue.head + 1) % BOUNDED_BUFFER_QUEUE_SIZE;
    logs_queue.count--;
    pthread_cond_signal(&logs_queue.not_full);
  }
  pthread_mutex_unlock(&logs_queue.bounded_buffer_mutex);
}

void * init_producer_thread(void * arg){
    atomic_fetch_add(&producer_count, 1); //Increment the producer count when a producer thread is created
    producer_thread_arg * info = (producer_thread_arg *)arg;
    ssize_t n = 0;
    producer_data data;
    data.pid = info->pid;
    strncpy(data.container_id, info->container_id, sizeof(data.container_id) - 1);
    memset(data.produced_data, 0, sizeof(data.produced_data));
    close(info->producer_read_fd[1]); //Close the write end of the pipe in the producer thread, only the container process will write to it
    while((n = read(info->producer_read_fd[0], data.produced_data, sizeof(data.produced_data))) > 0 ){
      manipulate_bounded_buffer_queue(&data, true);
      memset(data.produced_data, 0, sizeof(data.produced_data)); // clear AFTER enqueue
    }
    printf("Producer thread for container ID: %s has exited\n", info->container_id);
    atomic_fetch_sub(&producer_count, 1); //Decrement the producer count when a producer thread is exiting, this will signal the consumer thread to exit if there are no more producers and the buffer is empty
    free(info);

}

void * init_consumer_thread(){
  //quite simple consumer actually no nitin
  while((atomic_load(&server_running) == true) || (atomic_load(&producer_count) > 0) ){
    if(is_buffer_empty()){
      sleep(1);
      if(is_buffer_empty() == false){manipulate_bounded_buffer_queue(NULL, false);} 
      continue;
    }
    manipulate_bounded_buffer_queue(NULL, false);
  }
}

/* --------------------------------------------------------------------------
 * Supervisor Functions
 * -------------------------------------------------------------------------- */
void traverse_hashtable(){
    pthread_mutex_lock(&containers_list_mutex);
    FILE *f = fopen(PS_LOGS_PATH, "w");
    container_info *curr, *tmp;
    if(containers_list == NULL){
        fprintf(f, "No active containers\n");
    }
    HASH_ITER(hh, containers_list, curr, tmp) {
      if(curr->stopped == STOPPED)
        fprintf(f,"Container ID: %s, Host PID: %d, State: %d, STOPPED\n", curr->id, curr->host_pid, curr->state);
      else
        fprintf(f,"Container ID: %s, Host PID: %d, State: %d\n", curr->id, curr->host_pid, curr->state);
    }
    fclose(f);
    pthread_mutex_unlock(&containers_list_mutex);
}

 void fire_response_payload(supervisor_response *response, struct sockaddr_un *client_addr, socklen_t client_addr_len){
    int unix_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sendto(unix_socket, response, sizeof(*response), 0,
               (struct sockaddr *)client_addr, client_addr_len) < 0) {
      PANIC("Failed to send response to client");
    }
    close(unix_socket);
 }

void sigint_handler_func(int signum) { stop_signal_emmited = true; }
void add_container_info(container_info *info){
    pthread_mutex_lock(&containers_list_mutex);
    HASH_ADD_STR(containers_list, id, info);
    pthread_mutex_unlock(&containers_list_mutex);
}

int init_start(void *arg){
    ipc_payload_client *payload = (ipc_payload_client *)arg;
    printf("%15s>>>[CHILD INIT]: Initializing START command for container ID: %s\n", "", payload->id);
    char _buf;
    close(payload->await_fd[1]); //Close the write end of the pipe in the child, only the supervisor will write to it
    read(payload->await_fd[0], &_buf, 1); //Wait for the supervisor to signal that it's ready for the child to proceed with execution
    setsid();
    container_info * info = malloc(sizeof(container_info));
    strcpy(info->id, payload->id);
    strcpy(info->rootfs, payload->container_rootfs);
    info->host_pid = getpid();
    info->exit_code = -1; //Default value indicating not exited
    info->exit_signal = -1; //Default value indicating not exited by signal
    info->soft_mib = payload->soft_mib;
    info->hard_mib = payload->hard_mib;
    info->state = RUNNING;
    info->producer_write_fd = payload->producer_write_fd; //The supervisor will read logs from the read end of the pipe
    info->producer_thread = payload->producer_thread;
    
    dup2(info->producer_write_fd, STDOUT_FILENO);
    dup2(info->producer_write_fd, STDERR_FILENO);
    close(info->producer_write_fd); //As the STDOUT and STDERR of the container process are now redirected to the write end of the pipe, we can close the original write end file descriptor
    sethostname(info->id, strlen(info->id));
    if(mount(info->rootfs, info->rootfs, "bind", MS_BIND | MS_REC, NULL) < 0){
        perror("Failed to bind mount rootfs");
        return -1;
    }
    if(chroot(info->rootfs) < 0){
        perror("Failed to chroot to rootfs");
        return -1;
    }
    if(chdir("/") < 0){
        perror("Failed to change directory to /");
        return -1;
    }
    if(setpriority(PRIO_PROCESS, 0, payload->nice) < 0){
        perror("Failed to set process priority");
        return -1;
    }
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", MS_RDONLY, NULL);
    mount("tmpfs", "/dev", "tmpfs", 0, "mode=755");
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    int fd = open("/dev/null", O_RDONLY);
    dup2(fd, STDIN_FILENO); //Redirect stdin to /dev/null for the start command since it doesn't have an attached terminal
    close(fd);
    char *argv[] = { payload->prog, NULL };
    execv(payload->prog, argv);
    perror("Failed to exec command");
    return -1;
}
int init_run(void *arg){
    ipc_payload_client *payload = (ipc_payload_client *)arg;
    char _buf;
    close(payload->await_fd[1]); //Close the write end of the pipe in the child, only the supervisor will write to it
    read(payload->await_fd[0], &_buf, 1); //Wait for the supervisor to signal that it's ready for the child to proceed with execution
    printf("%15s>>>[CHILD INIT]: Initializing RUN command for container ID: %s\n", "",payload->id);
    setsid();
    ioctl(payload->slave_fd, TIOCSCTTY, 0);
    dup2(payload->slave_fd, STDIN_FILENO);
    dup2(payload->slave_fd, STDOUT_FILENO);
    dup2(payload->slave_fd, STDERR_FILENO);
    container_info * info = malloc(sizeof(container_info));
    strcpy(info->id, payload->id);
    strcpy(info->rootfs, payload->container_rootfs);
    info->host_pid = getpid();
    info->exit_code = -1; //Default value indicating not exited
    info->exit_signal = -1; //Default value indicating not exited by signal
    info->soft_mib = payload->soft_mib;
    info->hard_mib = payload->hard_mib;
    info->state = RUNNING;
    sethostname(info->id, strlen(info->id));
    if(mount(info->rootfs, info->rootfs, "bind", MS_BIND | MS_REC, NULL) < 0){
        perror("Failed to bind mount rootfs");
        return -1;
    }
    if(chroot(info->rootfs) < 0){
        perror("Failed to chroot to rootfs");
        return -1;
    }
    if(chdir("/") < 0){
        perror("Failed to change directory to /");
        return -1;
    }
    if(setpriority(PRIO_PROCESS, 0, payload->nice) < 0){
        perror("Failed to set process priority");
        return -1;
    }
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", MS_RDONLY, NULL);
    mount("tmpfs", "/dev", "tmpfs", 0, "mode=755");
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    char *argv[] = { payload->prog, NULL };
    execv(payload->prog, argv);
    perror("Failed to exec command");
    return -1;
}
void init_stop_handler(struct sockaddr_un client_addr, socklen_t client_addr_len, ipc_payload_client payload){
    pthread_mutex_lock(&containers_list_mutex);
    supervisor_response response;
    container_info * info;
    HASH_FIND_STR(containers_list, payload.id, info);
    if(info == NULL){
      printf("[STOP ERROR]: Container with ID: %s not found\n", payload.id);
      pthread_mutex_unlock(&containers_list_mutex);
      response = (supervisor_response){.type = NACK, .state = FAILED};
      fire_response_payload(&response, &client_addr, client_addr_len);
      return;
    }
    if(kill(info->host_pid, SIGKILL) < 0){
      printf("[STOP ERROR]: Failed to send SIGTERM to container with ID: %s\n", payload.id);
      pthread_mutex_unlock(&containers_list_mutex);
      response = (supervisor_response){.type = NACK, .state = FAILED};
      fire_response_payload(&response, &client_addr, client_addr_len);
      return;
    }
    info->stopped = STOPPED;              
    fire_response_payload(&(supervisor_response){.type = ACK}, &client_addr, client_addr_len);
    pthread_mutex_unlock(&containers_list_mutex);
}
void init_log_handler(struct sockaddr_un client_addr, socklen_t client_addr_len, ipc_payload_client payload){
    pthread_mutex_lock(&containers_list_mutex);
    supervisor_response response;
    container_info * info;
    HASH_FIND_STR(containers_list, payload.id, info);
    if(info == NULL){
      printf("[LOGS ERROR]: Container with ID: %s not found\n", payload.id);
      pthread_mutex_unlock(&containers_list_mutex);
      response = (supervisor_response){.type = NACK};
      fire_response_payload(&response, &client_addr, client_addr_len);
      return;
    }
    int n = snprintf(NULL, 0, "/tmp/%s.log", payload.id) + 1;
    char path[n];
    snprintf(path, n, "/tmp/%s.log", payload.id);
    response = (supervisor_response){.type = FILE_LOC};
    strncpy(response.data, path, sizeof(response.data) - 1);
    fire_response_payload(&response, &client_addr, client_addr_len);
    pthread_mutex_unlock(&containers_list_mutex);
}

void * init_repear(void * arg){
  (void)arg;
  sigset_t sigchld_mask;
  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);
  while(atomic_load(&server_running)){
    siginfo_t signal_info; //This will be used to get the PID of the child process that exited and its exit status
    int signal = sigwaitinfo(&sigchld_mask, &signal_info);
    if(signal < 0){
      if(errno == EINTR) continue; // Interrupted by signal, check server_running and continue
      perror("[REPEAR ERROR]: sigwaitinfo failed");
      break;
    }

    int status;
    pid_t exited_pid;
    while((exited_pid = waitpid(-1, &status, WNOHANG)) > 0){
      pthread_mutex_lock(&containers_list_mutex);
      container_info * entry = NULL;
      container_info * curr, * tmp;
      HASH_ITER(hh, containers_list, curr, tmp){
        if(curr->host_pid == exited_pid){
          entry = curr;
          break;
        }
      }
      if(entry == NULL){
        printf("[REPEAR WARNING] NOT SYNCING: Received SIGCHLD for unknown child process with PID: %d\n", exited_pid);
        printf("[REPEAR WARNING]: Termination of supervisor recommeded\n");
        pthread_mutex_unlock(&containers_list_mutex);
        continue;
      }
      if(WIFEXITED(status)){
        entry->exit_code = WEXITSTATUS(status);
        entry->state = EXITED;
        printf("[REPEAR]: Container with ID: %s and PID: %d exited with exit code: %d\n", entry->id, exited_pid, entry->exit_code);
      
    }
    if(WIFSIGNALED(status)){
      entry->exit_signal = WTERMSIG(status);
      entry->state = KILLED;
      printf("[REPEAR]: Container with ID: %s and PID: %d killed by signal: %d\n", entry->id, exited_pid, entry->exit_signal); 
    }
      pthread_mutex_unlock(&containers_list_mutex);
    }
  }
  return NULL;
}

void init_supervisor(const char *base_rootfs) {

    //Check if the base rootfs exists and is a directory
    struct stat rootfs_st;
    if (stat(base_rootfs, &rootfs_st) < 0) {
        errno = ENOENT;
        PANIC("Base rootfs does not exist");
    }strncpy(BASE_ROOTFS, base_rootfs, sizeof(BASE_ROOTFS) - 1);
    // Open a UNIX domain socket for communication with the CLI
    unlink(SUPERVISOR_SOCKET_PATH); // Ensure old socket is removed
    unlink(PS_LOGS_PATH); // Ensure old ps logs file is removed
    int unix_socket = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (bind(unix_socket,
            (struct sockaddr *)&(struct sockaddr_un){
                .sun_family = AF_UNIX, .sun_path = SUPERVISOR_SOCKET_PATH},
            sizeof(struct sockaddr_un)) < 0) {
        PANIC("Failed to bind UNIX socket");
    }

    //SIGCHILD Handler to reap child processes and update their state in the hashtable
    // Block SIGCHLD before spawning ANY threads so all threads inherit the mask
    sigset_t sigchld_mask;
    sigemptyset(&sigchld_mask);
    sigaddset(&sigchld_mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &sigchld_mask, NULL);

    // Register signal handlers for graceful shutdown
    struct sigaction sigint_handler;
    memset(&sigint_handler, 0, sizeof(sigint_handler));
    sigint_handler.sa_handler =
        sigint_handler_func; //! Uncomment this line to enable graceful shutdown
                            //! on SIGINT and SIGTERM
    sigemptyset(&sigint_handler.sa_mask);
    sigint_handler.sa_flags = 0;
    sigaction(SIGINT, &sigint_handler, NULL);
    sigaction(SIGTERM, &sigint_handler, NULL);

    printf("> Supervisor initialized with base rootfs: %s\n", base_rootfs);

    ipc_payload_client payload;
    struct sockaddr_un client_addr;

    //Invoke the consumer thread for the logs queue
    pthread_t consumer_thread;
    pthread_create(&consumer_thread, NULL, init_consumer_thread, NULL);

    //Invoke the reaper thread for handling SIGCHLD and updating container states
    pthread_t reaper_thread;
    pthread_create(&reaper_thread, NULL, init_repear, NULL);

    container_info * info;
    while (atomic_load(&server_running)) {
        // Wait for commands from the CLI and handle them accordingly
        memset(&client_addr, 0, sizeof(client_addr));
        memset(&payload, 0, sizeof(payload));
        socklen_t client_addr_len = sizeof(client_addr);
        int bytes = recvfrom(unix_socket, &payload, sizeof(payload), 0,
                            (struct sockaddr *)&client_addr, &client_addr_len);
        if (bytes < 0) {
        if (errno == EINTR) {
            if(stop_signal_emmited){
                atomic_store(&server_running, false);
                }
                continue; // Interrupted by signal, check supervisor_running and
                        // continue
            }
        PANIC("Failed to receive data from socket");
        }
        printf("%10s>>Received command: %d for container ID: %s\n", "",payload.cmd,
            payload.id);
        if(payload.cmd == RUN){
          struct msghdr msg = {0};
          char m_buffer[1] = {0};
          struct iovec io = {
              .iov_base = m_buffer,
              .iov_len = sizeof(m_buffer)
          };
          char c_buffer[CMSG_SPACE(sizeof(int))];
          msg.msg_iov = &io;
          msg.msg_iovlen = 1;
          msg.msg_control = c_buffer;
          msg.msg_controllen = sizeof(c_buffer);
          if (recvmsg(unix_socket, &msg, 0) < 0){
            if (errno == EINTR) {
                if(stop_signal_emmited){
                    atomic_store(&server_running, false);
                }
                continue; // Interrupted by signal, check supervisor_running and
                        // continue
            }
            PANIC("Failed to receive file descriptor");
          }
          struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
          int fd;
          memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
          payload.slave_fd = fd;
        }
        pid_t child_pid;
        pipe(payload.await_fd); //Create a pipe for synchronizing between supervisor and child during startup
        
        switch(payload.cmd){
            case START:
                producer_thread_arg *  producer_arg = malloc(sizeof(producer_thread_arg));
                pipe(producer_arg->producer_read_fd); //Create a pipe for the container process to write logs to and supervisor to read logs from
                payload.producer_write_fd = producer_arg->producer_read_fd[1]; //The child will write logs to the write end of the pipe
                child_pid = clone(&init_start, child_stack + STACK_SIZE, CHILD_FLAGS, &payload);
                if(child_pid < 0){
                  //Send Error response to client
                  supervisor_response response = {.type = NACK, .state = FAILED};
                fire_response_payload(&response, &client_addr, client_addr_len);
                break;
                }
                //send success response to client
                close(producer_arg->producer_read_fd[1]); //Close the write end of the pipe in the producer thread, only the container process will write to it
                producer_arg->pid = child_pid;
                strncpy(producer_arg->container_id, payload.id, sizeof(producer_arg->container_id) - 1);
                pthread_create(&payload.producer_thread, NULL, init_producer_thread, producer_arg);
                register_child_proc_with_kernel(child_pid, &payload);
                supervisor_response response = {.type = ACK, .state = RUNNING};
                fire_response_payload(&response, &client_addr, client_addr_len);
                //Hashing Container Info
                info = malloc(sizeof(container_info));
                strcpy(info->id, payload.id);
                strcpy(info->rootfs, payload.container_rootfs);
                info->host_pid = child_pid;
                info->exit_code = -1; //Default value indicating not exited
                info->exit_signal = -1; //Default value indicating not exited by signal
                info->soft_mib = payload.soft_mib;
                info->hard_mib = payload.hard_mib;
                info->state = RUNNING;
                add_container_info(info);
                break;
            case RUN:
                child_pid = clone(&init_run, child_stack + STACK_SIZE, CHILD_FLAGS, &payload);
                if(child_pid < 0){
                    //Send Error response to client
                    printf("%15s>>>Failed to clone child process for RUN command\n", "");
                    break;
                }
                //send success response to client 
                printf("%15s>>>Cloned child process with PID: %d for RUN command\n", "", child_pid);
                close(payload.slave_fd); //Close the slave fd in the supervisor after passing it to the child
                register_child_proc_with_kernel(child_pid, &payload);
                //Hashing Container Info
                info = malloc(sizeof(container_info));
                strcpy(info->id, payload.id);
                strcpy(info->rootfs, payload.container_rootfs);
                info->host_pid = child_pid;
                info->exit_code = -1; //Default value indicating not exited
                info->exit_signal = -1; //Default value indicating not exited by signal
                info->soft_mib = payload.soft_mib;
                info->hard_mib = payload.hard_mib;
                info->state = RUNNING;
                add_container_info(info);
                break;
            case PS:
                traverse_hashtable();
                supervisor_response ps_response = {.type = FILE_LOC, .data = PS_LOGS_PATH};
                fire_response_payload(&ps_response, &client_addr, client_addr_len);
                break;
              case STOP:
                init_stop_handler(client_addr, client_addr_len, payload);
                break;
              case LOGS:
              init_log_handler(client_addr, client_addr_len, payload);
              break;
            default:
                break;

        }
        if(stop_signal_emmited){
            atomic_store(&server_running, false);
        }
    }
    printf("\n>Supervisor shutting down, waiting for consumer thread to finish processing logs...\n");
    atomic_store(&server_running, false);
    pthread_cond_broadcast(&logs_queue.not_empty); // wake the consumer so it can check the exit condition  
    pthread_kill(reaper_thread, SIGCHLD);
    pthread_join(reaper_thread, NULL); 
    pthread_join(consumer_thread, NULL);
    unlink(SUPERVISOR_SOCKET_PATH);
    printf(">Supervisor exiting gracefully\n");
}

/* --------------------------------------------------------------------------
 * Client Functions
 * -------------------------------------------------------------------------- */

static supervisor_response fire_command_payload(ipc_payload_client *payload){
  size_t needed =
      snprintf(NULL, 0, "%s%d.sock", CONTROL_SOCKET_PATH_PREFIX, getpid()) + 1;

  char *control_socket_path = malloc(needed);
  if (control_socket_path == NULL) {
    PANIC_CLIENT("Failed to allocate memory");
  }

  snprintf(control_socket_path, needed, "%s%d.sock", CONTROL_SOCKET_PATH_PREFIX,
           getpid());
  int unix_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un client_addr = {
      .sun_family = AF_UNIX,
  };
  strcpy(client_addr.sun_path, control_socket_path);
  if (bind(unix_socket, (struct sockaddr *)&client_addr,
           sizeof(struct sockaddr_un)) < 0) {
    unlink(control_socket_path);
    free(control_socket_path);
    PANIC_CLIENT("Failed to bind client UNIX socket");
  }

  int server_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un server_addr = {.sun_family = AF_UNIX,
                                    .sun_path = SUPERVISOR_SOCKET_PATH};
  socklen_t server_addr_len = sizeof(server_addr);
  if (sendto(unix_socket, payload, sizeof(*payload), 0,
             (struct sockaddr *)&server_addr, server_addr_len) < 0) {
    unlink(control_socket_path);
    free(control_socket_path);
    PANIC_CLIENT("Failed to send data to supervisor");
  }
  if(payload->cmd == RUN){
    struct msghdr msg = {0};
    char m_buffer[1] = {0};
    struct iovec io = {
        .iov_base = m_buffer,
        .iov_len  = sizeof(m_buffer)
    };
    char c_buffer[CMSG_SPACE(sizeof(int))];

    msg.msg_name    = &server_addr;           // destination: the supervisor
    msg.msg_namelen = server_addr_len;

    msg.msg_iov        = &io;
    msg.msg_iovlen     = 1;
    msg.msg_control    = c_buffer;
    msg.msg_controllen = sizeof(c_buffer);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &payload->slave_fd, sizeof(int));

    if (sendmsg(unix_socket, &msg, 0) < 0) {
        unlink(control_socket_path);
        free(control_socket_path);
        PANIC_CLIENT("Failed to send file descriptor to supervisor");
    }
    unlink(control_socket_path);
    return; //For RUN command, we don't wait for a response from the supervisor
  }
  // if(payload->cmd == START){return 0;}
  supervisor_response response;
  ssize_t bytes_received =
      recvfrom(unix_socket, &response, sizeof(supervisor_response), 0, NULL, NULL);
  if (bytes_received < 0) {
    unlink(control_socket_path);
    free(control_socket_path);
    PANIC_CLIENT("Failed to receive response from supervisor");
  }
  printf("Received response from supervisor\n");

  unlink(control_socket_path);
  free(control_socket_path);
  return response;
}

static void init_cmd_start(int argc, char *argv[]) {
  if (argc < 5) {
    fprintf(stderr,
            "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] "
            "[--hard-mib N] [--nice N]\n",
            argv[0]);
    errno = EINVAL;
    PANIC_CLIENT("Invalid arguments for start command");
  }
  ipc_payload_client payload;
  memset(&payload, 0, sizeof(payload));
  payload.cmd = START;
  strncpy(payload.id, argv[2], sizeof(payload.id) - 1);
  strncpy(payload.container_rootfs, argv[3],
          sizeof(payload.container_rootfs) - 1);
   strncpy(payload.prog, argv[4], sizeof(payload.prog) - 1);
  payload.soft_mib = DEFAULT_SOFT_LIMIT;
  payload.hard_mib = DEFAULT_HARD_LIMIT;
  payload.nice = 0; //Default nice value
  if (parse_optional_flags(&payload, argc, argv, 5) != 0) {
    errno = EINVAL;
    PANIC_CLIENT("Failed to parse optional flags for start command");
  }
  supervisor_response response = fire_command_payload(&payload);
  if(response.type == ACK){
    printf("Start command acknowledged by supervisor, container is starting...\n");
  }else{
    PANIC_CLIENT("Supervisor responded with NACK for start command");
  }
}
static void init_cmd_run(int argc, char *argv[]) {
  if (argc < 5) {
    fprintf(stderr,
            "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] "
            "[--hard-mib N] [--nice N]\n",
            argv[0]);
    errno = EINVAL;
    PANIC_CLIENT("Invalid arguments for start command");
  }
  ipc_payload_client payload;
  memset(&payload, 0, sizeof(payload));
  payload.cmd = RUN;
  strncpy(payload.id, argv[2], sizeof(payload.id) - 1);
  strncpy(payload.container_rootfs, argv[3],
          sizeof(payload.container_rootfs) - 1);
  strncpy(payload.prog, argv[4], sizeof(payload.prog) - 1);
  payload.soft_mib = DEFAULT_SOFT_LIMIT;
  payload.hard_mib = DEFAULT_HARD_LIMIT;
  payload.nice = 0; //Default nice value
  if (parse_optional_flags(&payload, argc, argv, 5) != 0) {
    errno = EINVAL;
    PANIC_CLIENT("Failed to parse optional flags for start command");
  }
  int master_fd, slave_fd;
  openpty(&master_fd, &slave_fd, NULL, NULL, NULL);
  payload.slave_fd = slave_fd;
  fire_command_payload(&payload);
  close(slave_fd);
  struct termios orig, raw;
  tcgetattr(STDIN_FILENO, &orig);
  raw = orig;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  fd_set fds;
  char buf[256];
  while (1) {
      FD_ZERO(&fds);
      FD_SET(STDIN_FILENO, &fds);
      FD_SET(master_fd, &fds);
      select(master_fd + 1, &fds, NULL, NULL, NULL);
      if (FD_ISSET(STDIN_FILENO, &fds)) {
          int n = read(STDIN_FILENO, buf, sizeof(buf));
          if(n <= 0) break; // EOF or error
          write(master_fd, buf, n);
      }
      if (FD_ISSET(master_fd, &fds)) {
          int n = read(master_fd, buf, sizeof(buf));
          if (n <= 0) break;   // EOF or error
          write(STDOUT_FILENO, buf, n);
      }
  }
  tcsetattr(STDIN_FILENO, TCSANOW, &orig);  // restore terminal to cooked mode
  close(master_fd);
}
static void init_cmd_ps(void){
  ipc_payload_client payload;
  memset(&payload, 0, sizeof(payload));
  payload.cmd = PS;
  supervisor_response respone = fire_command_payload(&payload);
  int fd = open(respone.data, O_RDONLY);
  if(fd < 0){
    PANIC_CLIENT("Failed to open ps logs file");
  }
  char buf[256];
  ssize_t n;
  printf("All containers:\n");
  while((n = read(fd, buf, sizeof(buf))) > 0){
    write(STDOUT_FILENO, buf, n);
  }
  close(fd);
}
static void init_cmd_logs(int argc, char * argv[]){
  if (argc < 3) {
      fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
      errno = EINVAL;
      PANIC_CLIENT("Not Enough Arguments");
  }
  ipc_payload_client payload;
  memset(&payload, 0, sizeof(payload));
  payload.cmd = LOGS;
  strncpy(payload.id, argv[2], sizeof(payload.id) - 1);
  supervisor_response response = fire_command_payload(&payload);
  if(response.type == FILE_LOC){
    printf("Logs command acknowledged by supervisor, fetching logs...\n");
    int fd = open(response.data, O_RDONLY);
    if(fd < 0){
      PANIC_CLIENT("Failed to open logs file");
    }
    char buf[256];
    ssize_t n;
    while((n = read(fd, buf, sizeof(buf))) > 0){
      write(STDOUT_FILENO, buf, n);
    }
    close(fd);
  }else{
    PANIC_CLIENT("Supervisor responded with NACK for logs command, No such container with ID\n");
  }
}
static void init_cmd_stop(int argc, char * argv[]){
	if(argc < 3) {
	  fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
	  errno = EINVAL;
	  PANIC_CLIENT("Not Enough Arguments");
  }
  ipc_payload_client payload;
  memset(&payload, 0, sizeof(payload));
  payload.cmd = STOP;
  strncpy(payload.id, argv[2], sizeof(payload.id) - 1);
  supervisor_response response = fire_command_payload(&payload);
  if(response.type == ACK){
    printf("Stop command acknowledged by supervisor, container is stopping...\n");
  }else{
    PANIC_CLIENT("Supervisor responded with NACK for stop command, No such container with ID\n");
  }

}
/* --------------------------------------------------------------------------
 * Main Function
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (strcmp(argv[1], "supervisor") == 0) {
    if (argc < 3) {
      fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
      return 1;
    }
  }
  //Initialize the logs queue
  logs_queue.head = 0;
  logs_queue.tail = 0;
  logs_queue.count = 0;
  memset(logs_queue.buffer, 0, sizeof(logs_queue.buffer));
  pthread_mutex_init(&logs_queue.bounded_buffer_mutex, NULL);
  pthread_cond_init(&logs_queue.not_full, NULL);
  pthread_cond_init(&logs_queue.not_empty, NULL);

  //Initialize the mutex for the containers list
  pthread_mutex_init(&containers_list_mutex, NULL);

  printf("PID: %d\n", getpid());
  switch (parse_command(argv[1], argv[0])) {
  case CLI_SUPERVISOR:
    init_supervisor(argv[2]);
    break;
  case CLI_START:
    init_cmd_start(argc, argv);
    break;
  case CLI_RUN:
    init_cmd_run(argc, argv);
    break;
  case CLI_PS:
    init_cmd_ps();
    break;
  case CLI_LOGS:
    init_cmd_logs(argc, argv);
    break;
  case CLI_STOP:
    init_cmd_stop(argc, argv);
    break;
  }
  return 0;
}
