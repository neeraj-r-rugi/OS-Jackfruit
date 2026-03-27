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

// File Specific Global Variables


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
/* --------------------------------------------------------------------------
 * Supervisor Functions
 * -------------------------------------------------------------------------- */

void sigint_handler_func(int signum) { stop_signal_emmited = true; }
void add_container_info(container_info *info){
    pthread_mutex_lock(&containers_list_mutex);
    HASH_ADD_STR(containers_list, id, info);
    pthread_mutex_unlock(&containers_list_mutex);
}

int init_start(void *arg){
    ipc_payload_client *payload = (ipc_payload_client *)arg;
    printf("Initializing start command for container ID: %s\n", payload->id);
    container_info * info = malloc(sizeof(container_info));
    strcpy(info->id, payload->id);
    strcpy(info->rootfs, payload->container_rootfs);
    info->host_pid = getpid();
    info->exit_code = -1; //Default value indicating not exited
    info->exit_signal = -1; //Default value indicating not exited by signal
    info->soft_mib = payload->soft_mib;
    info->hard_mib = payload->hard_mib;
    info->state = RUNNING;
    add_container_info(info);
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
    setsid();
    ioctl(payload->slave_fd, TIOCSCTTY, 0);
    dup2(payload->slave_fd, STDIN_FILENO);
    dup2(payload->slave_fd, STDOUT_FILENO);
    dup2(payload->slave_fd, STDERR_FILENO);
    printf("Initializing start command for container ID: %s\n", payload->id);
    container_info * info = malloc(sizeof(container_info));
    strcpy(info->id, payload->id);
    strcpy(info->rootfs, payload->container_rootfs);
    info->host_pid = getpid();
    info->exit_code = -1; //Default value indicating not exited
    info->exit_signal = -1; //Default value indicating not exited by signal
    info->soft_mib = payload->soft_mib;
    info->hard_mib = payload->hard_mib;
    info->state = RUNNING;
    add_container_info(info);
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
void init_supervisor(const char *base_rootfs) {

    //Check if the base rootfs exists and is a directory
    struct stat rootfs_st;
    if (stat(base_rootfs, &rootfs_st) < 0) {
        errno = ENOENT;
        PANIC("Base rootfs does not exist");
    }strncpy(BASE_ROOTFS, base_rootfs, sizeof(BASE_ROOTFS) - 1);
    // Open a UNIX domain socket for communication with the CLI
    unlink(SUPERVISOR_SOCKET_PATH); // Ensure old socket is removed
    int unix_socket = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (bind(unix_socket,
            (struct sockaddr *)&(struct sockaddr_un){
                .sun_family = AF_UNIX, .sun_path = SUPERVISOR_SOCKET_PATH},
            sizeof(struct sockaddr_un)) < 0) {
        PANIC("Failed to bind UNIX socket");
    }

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

    printf("Supervisor initialized with base rootfs: %s\n", base_rootfs);

    ipc_payload_client payload;
    struct sockaddr_un client_addr;
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
        printf("Received command: %d for container ID: %s\n", payload.cmd,
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
                child_pid = clone(&init_start, child_stack + STACK_SIZE, CHILD_FLAGS, &payload);
                if(child_pid < 0){
                    //Send Error response to client
                }
                break;
            case RUN:
                child_pid = clone(&init_run, child_stack + STACK_SIZE, CHILD_FLAGS, &payload);
                if(child_pid < 0){
                    //Send Error response to client
                    printf("Failed to clone child process for RUN command\n");
                }
                close(payload.await_fd[0]);
                close(payload.slave_fd); //Close the slave fd in the supervisor after passing it to the child
                printf("Cloned child process with PID: %d for RUN command\n", child_pid);
                char path[100], map[100];
                sprintf(path, "/proc/%d/setgroups", child_pid);
                write_file(path, "deny");

                sprintf(path, "/proc/%d/uid_map", child_pid);
                sprintf(map, "0 %d 1", getuid());
                write_file(path, map);

                sprintf(path, "/proc/%d/gid_map", child_pid);
                sprintf(map, "0 %d 1", getgid());
                write_file(path, map);
                write(payload.await_fd[1], "x", 1); //Signal the child that it can proceed with execution after setting up user namespace mappings
                break;
            default:
                break;

        }
        if(stop_signal_emmited){
            atomic_store(&server_running, false);
        }
    }
    unlink(SUPERVISOR_SOCKET_PATH);
}

/* --------------------------------------------------------------------------
 * Client Functions
 * -------------------------------------------------------------------------- */

static int fire_command_payload(ipc_payload_client *payload){
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
    return 0; //For RUN command, we don't wait for a response from the supervisor
  }
  char buffer[256];
  ssize_t bytes_received =
      recvfrom(unix_socket, buffer, sizeof(buffer), 0, NULL, NULL);
  if (bytes_received < 0) {
    unlink(control_socket_path);
    free(control_socket_path);
    PANIC_CLIENT("Failed to receive response from supervisor");
  }
  printf("Received response from supervisor: %.*s\n", (int)bytes_received,
         buffer);

  unlink(control_socket_path);
  free(control_socket_path);
  return 0;
}
/*
 * TODO: 1. ADD the remaining cli commands with the interface.
 * TODO: 2. Think of the data structure to store the running PIDs
 * TODO: 3. Begin actually executing the commands.
 * */

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
  fire_command_payload(&payload);
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
  fire_command_payload(&payload);

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
  fire_command_payload(&payload);
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
  fire_command_payload(&payload);

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
