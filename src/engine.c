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
/* --------------------------------------------------------------------------
 * Supervisor Functions
 * -------------------------------------------------------------------------- */

void sigint_handler_func(int signum) { stop_signal_emmited = true; }

void init_supervisor(const char *base_rootfs) {
  // Open a UNIX domain socket for communication with the CLI
  unlink(SUPERVISOR_SOCKET_PATH); // Ensure old socket is removed
  int unix_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
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
    sendto(unix_socket, "Command received", strlen("Command received"), 0,
           (struct sockaddr *)&client_addr, client_addr_len);
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
  payload.soft_mib = DEFAULT_SOFT_LIMIT;
  payload.hard_mib = DEFAULT_HARD_LIMIT;
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
  payload.soft_mib = DEFAULT_SOFT_LIMIT;
  payload.hard_mib = DEFAULT_HARD_LIMIT;
  if (parse_optional_flags(&payload, argc, argv, 5) != 0) {
    errno = EINVAL;
    PANIC_CLIENT("Failed to parse optional flags for start command");
  }
  fire_command_payload(&payload);
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
