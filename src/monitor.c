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

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Kernel Level Process Monitor for Container Management");
MODULE_AUTHOR("Neeraj");
MODULE_AUTHOR("Nitn");
static int __init monitor_init(void)
{
    printk(KERN_INFO "monitor module loaded\n");
    return 0;
}

static void __exit monitor_exit(void)
{
    printk(KERN_INFO "monitor module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);