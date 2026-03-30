#ifndef DEFINES_H
#define DEFINES_H
/* --------------------------------------------------------------------------
 * Kernel Space 
 * -------------------------------------------------------------------------- */
#define STRUCT_STR_LEN 256
#define DEVICE_NAME "container_monitor"

//Structs
typedef struct monitor_request{
	pid_t pid;
	unsigned long int HARD_MIB;
	unsigned long int SOFT_MIB;
	char container_id[STRUCT_STR_LEN];
}monitor_request;

//IOCTL Commands
#define MAGIC_NUMBER 'M'
#define KERNEL_READ __IOW(MAGIC_NUMBER, 0, monitor_request)
#define KERNEL_WRITE __IOR(MAGIC_NUMBER, 1, monitor_request)

#endif