#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "kernel_defines.h"

static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Kernel Level Process Monitor for Container Management");
MODULE_AUTHOR("Neeraj");
MODULE_AUTHOR("Nitn");
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    return 0; //TODO: Implement ioctl handler logic and replace 0
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl 
};

static int __init monitor_init(void)
{
    printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO]: monitor module loaded\n");

    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
        printk(KERN_ERR "[KERNEL MEMORY MONITOR >>> ERROR]: Failed to allocate character device region\n");
        return -1;
    }

    cl = class_create(DEVICE_NAME);
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "[KERNEL MEMORY MONITOR >>> ERROR]: Failed to create device class\n");
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "[KERNEL MEMORY MONITOR >>> ERROR]: Failed to create device\n");
        return -1;
    }

    cdev_init(&c_dev, &fops);

    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "[KERNEL MEMORY MONITOR >>> ERROR]: Failed to add character device\n");
        return -1;
    }

    printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO]: Device /dev/%s created successfully\n", DEVICE_NAME);

    return 0;
}

static void __exit monitor_exit(void)
{
    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);
    printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO]: monitor module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);