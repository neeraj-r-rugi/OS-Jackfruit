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
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/jiffies.h>
#include "kernel_defines.h"

static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Kernel Level Process Monitor for Container Management");
MODULE_AUTHOR("Neeraj");
MODULE_AUTHOR("Nitn");

static LIST_HEAD(main_list);
static LIST_HEAD(escalated_list);

static DEFINE_MUTEX(main_list_lock);
static DEFINE_MUTEX(escalated_list_lock);

static struct timer_list main_list_timer;
static struct timer_list escalated_list_timer;

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }

    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }

    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

static void kill_process(pid_t pid, unsigned long hard_mib, unsigned long rss_mib)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[KERNEL MEMORY MONITOR >>> KILL] pid=%d rss=%lu MiB hard=%lu MiB\n",
           pid, rss_mib, hard_mib);
}

static bool pid_exists_in_list(struct list_head *head, pid_t pid)
{
    pid_list *entry;

    list_for_each_entry(entry, head, list) {
        if (entry->pid == pid)
            return true;
    }

    return false;
}

static void escalated_list_timer_callback(struct timer_list *t)
{
    pid_list *entry, *tmp;
    (void)t;
    
    mutex_lock(&escalated_list_lock);

    list_for_each_entry_safe(entry, tmp, &escalated_list, list) {
        long rss_bytes;
        unsigned long rss_mib;

        rss_bytes = get_rss_bytes(entry->pid);

        if (rss_bytes < 0) {
            printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO] Removing dead PID %d from escalated list\n", entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        rss_mib = rss_bytes;

        if (rss_mib > entry->HARD_MIB) {
            kill_process(entry->pid, entry->HARD_MIB, rss_mib);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (rss_mib < entry->SOFT_MIB) {
            mutex_lock(&main_list_lock);

            list_move(&entry->list, &main_list);

            mutex_unlock(&main_list_lock);

            printk(KERN_INFO
                   "[KERNEL MEMORY MONITOR >>> INFO] PID %d dropped below SOFT (%lu MiB), back to main list\n",
                   entry->pid, rss_mib);
        }
    }

    mutex_unlock(&escalated_list_lock);

    mod_timer(&escalated_list_timer,
              jiffies + (ESCALATED_CHECK_INTERVAL_SEC * HZ));
}

static void main_list_timer_callback(struct timer_list *t)
{
    pid_list *entry, *tmp;
    (void)t;
    
    mutex_lock(&main_list_lock);

    list_for_each_entry_safe(entry, tmp, &main_list, list) {
        long rss_bytes;
        unsigned long rss_mib;

        rss_bytes = get_rss_bytes(entry->pid);

        if (rss_bytes < 0) {
            printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO] Removing dead PID %d from main list\n", entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        rss_mib = rss_bytes;

        if (rss_mib > entry->SOFT_MIB) {
            mutex_lock(&escalated_list_lock);

            list_move(&entry->list, &escalated_list);

            mutex_unlock(&escalated_list_lock);

            printk(KERN_WARNING
                   "[KERNEL MEMORY MONITOR >>> ESCALATION] PID %d exceeded SOFT LIMIT (%lu MiB)\n",
                   entry->pid, rss_mib);
        }
    }

    mutex_unlock(&main_list_lock);

    mod_timer(&main_list_timer,
              jiffies + (MAIN_CHECK_INTERVAL_SEC * HZ));
}

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if(cmd == KERNEL_ADD)
        printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> IOCTL] ADD COMMAND RECEIEVED\n");
    if(cmd == KERNEL_DELETE)
        printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> IOCTL] DELETE COMMAND RECEIEVED\n");
    monitor_request req;
    pid_list *entry, *tmp;
    (void)f;

    if (copy_from_user(&req, (monitor_request *)arg, sizeof(req)))
        return -EFAULT;

    if (req.SOFT_MIB >= req.HARD_MIB)
        return -EINVAL;

    if (cmd == KERNEL_ADD) {
        pid_list *new_entry;

        mutex_lock(&main_list_lock);

        if (pid_exists_in_list(&main_list, req.pid)) {
            mutex_unlock(&main_list_lock);
            return -EEXIST;
        }

        mutex_unlock(&main_list_lock);

        new_entry = kmalloc(sizeof(pid_list), GFP_KERNEL);
        if (!new_entry)
            return -ENOMEM;

        new_entry->pid = req.pid;
        new_entry->SOFT_MIB = req.SOFT_MIB;
        new_entry->HARD_MIB = req.HARD_MIB;

        mutex_lock(&main_list_lock);
        list_add(&new_entry->list, &main_list);
        mutex_unlock(&main_list_lock);

        printk(KERN_INFO
               "[KERNEL MEMORY MONITOR >>> INFO] Added pid=%d soft=%lu hard=%lu\n",
               req.pid, req.SOFT_MIB, req.HARD_MIB);

        return 0;
    }

    if (cmd == KERNEL_DELETE) {
        mutex_lock(&main_list_lock);

        list_for_each_entry_safe(entry, tmp, &main_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&main_list_lock);
                printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO] Deleted from Main List PID= %d\n", entry->pid);
                return 0;
            }
        }

        mutex_unlock(&main_list_lock);

        mutex_lock(&escalated_list_lock);

        list_for_each_entry_safe(entry, tmp, &escalated_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&escalated_list_lock);
                printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO] Deleted from Escalated List PID= %d\n", entry->pid);
                return 0;
            }
        }

        mutex_unlock(&escalated_list_lock);

        return 0;//This is because, the kernel module might have already kicked it out of the list when it detected that the process has exited.
    }

    return -EINVAL;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl
};

static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

    cl = class_create(DEVICE_NAME);
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);

    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&main_list_timer, main_list_timer_callback, 0);
    timer_setup(&escalated_list_timer, escalated_list_timer_callback, 0);

    mod_timer(&main_list_timer, jiffies + (MAIN_CHECK_INTERVAL_SEC * HZ));
    mod_timer(&escalated_list_timer, jiffies + (ESCALATED_CHECK_INTERVAL_SEC * HZ));

    printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO] Loaded successfully\n");

    return 0;
}

static void __exit monitor_exit(void)
{
    timer_delete_sync(&main_list_timer);
    timer_delete_sync(&escalated_list_timer);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[KERNEL MEMORY MONITOR >>> INFO] Unloaded successfully\n");
}

module_init(monitor_init);
module_exit(monitor_exit);