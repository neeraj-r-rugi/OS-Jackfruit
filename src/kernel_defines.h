#ifndef KERNEL_DEFINES_H
#define KERNEL_DEFINES_H

/* --------------------------------------------------------------------------
 * Shared between userspace (engine.c) and kernel (monitor.c)
 * -------------------------------------------------------------------------- */

#define STRUCT_STR_LEN 256
#define DEVICE_NAME "container_monitor"
#define MAIN_CHECK_INTERVAL_SEC 2
#define ESCALATED_CHECK_INTERVAL_SEC 1

/* ioctl structs — must be visible to both sides */
typedef struct monitor_request {
    pid_t         pid;
    unsigned long HARD_MIB;
    unsigned long SOFT_MIB;
} monitor_request;

/* IOCTL commands */
#define MAGIC_NUMBER 'M'

#ifdef __KERNEL__
  #include <linux/ioctl.h>
  /* pid_list is kernel-only — uses kernel's struct list_head */
  typedef struct pid_list {
      pid_t         pid;
      unsigned long SOFT_MIB;
      unsigned long HARD_MIB;
      struct list_head list;   /* kernel's list_head from <linux/list.h> */
  } pid_list;
  #define KERNEL_ADD    _IOW(MAGIC_NUMBER, 0, monitor_request)
  #define KERNEL_DELETE _IOW(MAGIC_NUMBER, 1, monitor_request)
#else
  #include <sys/ioctl.h>
  #define KERNEL_ADD    _IOW(MAGIC_NUMBER, 0, monitor_request)
  #define KERNEL_DELETE _IOW(MAGIC_NUMBER, 1, monitor_request)
#endif

#endif /* KERNEL_DEFINES_H */