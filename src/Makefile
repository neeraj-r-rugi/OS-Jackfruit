CC = gcc

# User-space source files (exclude monitor.c)
src = $(filter-out monitor.c,$(wildcard *.c))
target = $(src:.c=)

# Kernel module
obj-m += monitor.o

# Kernel build directory
KDIR = /lib/modules/$(shell uname -r)/build
PWD  = $(shell pwd)

all: user module

# Build normal user-space executables
user:
	$(CC) -o $(target) $(src) -lpthread

# Build kernel module -> monitor.ko
module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -f $(target)
	$(MAKE) -C $(KDIR) M=$(PWD) clean
