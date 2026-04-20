#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/ioctl.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CHIRANTH.R (PES1UG24AM354) | Deeptanshu Kumar (PES1UG24AM357)");
MODULE_DESCRIPTION("Kernel memory monitor for containers — Multi-Container Runtime");

#define DEVICE_NAME      "container_monitor"
#define MAX_MONITORED    64
#define CHECK_INTERVAL_MS 100

typedef struct {
    pid_t          pid;
    unsigned long  soft_limit;
    unsigned long  hard_limit;
    int            active;
    int            warned_soft;
} monitored_t;

static int                  major_num;
static struct class        *monitor_class;
static struct cdev          monitor_cdev;
static struct device       *monitor_device;

static monitored_t          monitored_processes[MAX_MONITORED];
static int                  monitored_count = 0;
static DEFINE_SPINLOCK(monitored_lock);
static monitored_t          monitor_snapshot[MAX_MONITORED];

static struct task_struct  *monitor_thread;
static volatile int         monitor_shutdown = 0;

/* ── helpers ────────────────────────────────────────────────────────────── */

static unsigned long get_process_rss(struct task_struct *task) {
    struct mm_struct *mm;
    unsigned long rss = 0;

    if (!task) return 0;
    mm = get_task_mm(task);
    if (!mm) return 0;
    rss = get_mm_rss(mm) * PAGE_SIZE;
    mmput(mm);
    return rss;
}

static struct task_struct *get_task_by_pid(pid_t pid) {
    struct pid *p = find_get_pid(pid);
    struct task_struct *task = NULL;
    if (p) {
        task = pid_task(p, PIDTYPE_PID);
        put_pid(p);
    }
    return task;
}

/* ── monitor kthread ────────────────────────────────────────────────────── */

static int monitor_thread_func(void *data) {
    (void)data;

    while (!kthread_should_stop() && !monitor_shutdown) {
        unsigned long flags;
        int i;

        msleep_interruptible(CHECK_INTERVAL_MS);
        if (kthread_should_stop() || monitor_shutdown) break;

        /*
         * Snapshot PID list under spinlock, then do the expensive
         * memory lookups outside the lock to avoid sleeping while
         * holding a spinlock.
         */
        int count;

        spin_lock_irqsave(&monitored_lock, flags);
        count = monitored_count;
        memcpy(monitor_snapshot, monitored_processes, count * sizeof(monitored_t));
        spin_unlock_irqrestore(&monitored_lock, flags);

        for (i = 0; i < count; i++) {
            if (!monitor_snapshot[i].active) continue;

            struct task_struct *task = get_task_by_pid(monitor_snapshot[i].pid);
            if (!task) {
                /* Process gone – mark inactive */
                spin_lock_irqsave(&monitored_lock, flags);
                for (int j = 0; j < monitored_count; j++) {
                    if (monitored_processes[j].pid == monitor_snapshot[i].pid) {
                        monitored_processes[j].active = 0;
                        break;
                    }
                }
                spin_unlock_irqrestore(&monitored_lock, flags);
                continue;
            }

            unsigned long rss = get_process_rss(task);

            if (rss > monitor_snapshot[i].hard_limit) {
                printk(KERN_WARNING
                       "container_monitor: PID %d exceeded hard limit "
                       "(%lu > %lu), killing\n",
                       monitor_snapshot[i].pid, rss, monitor_snapshot[i].hard_limit);
                send_sig(SIGKILL, task, 0);
                spin_lock_irqsave(&monitored_lock, flags);
                for (int j = 0; j < monitored_count; j++) {
                    if (monitored_processes[j].pid == monitor_snapshot[i].pid) {
                        monitored_processes[j].active = 0;
                        break;
                    }
                }
                spin_unlock_irqrestore(&monitored_lock, flags);
                continue;
            }

            if (rss > monitor_snapshot[i].soft_limit && !monitor_snapshot[i].warned_soft) {
                printk(KERN_WARNING
                       "container_monitor: PID %d exceeded soft limit "
                       "(%lu > %lu)\n",
                       monitor_snapshot[i].pid, rss, monitor_snapshot[i].soft_limit);
                spin_lock_irqsave(&monitored_lock, flags);
                for (int j = 0; j < monitored_count; j++) {
                    if (monitored_processes[j].pid == monitor_snapshot[i].pid) {
                        monitored_processes[j].warned_soft = 1;
                        break;
                    }
                }
                spin_unlock_irqrestore(&monitored_lock, flags);
            }
        }
    }
    return 0;
}

/* ── ioctl handlers ─────────────────────────────────────────────────────── */

static int register_container(pid_t pid,
                               unsigned long soft_limit,
                               unsigned long hard_limit) {
    unsigned long flags;
    int i;

    spin_lock_irqsave(&monitored_lock, flags);

    /* Update existing entry if present */
    for (i = 0; i < monitored_count; i++) {
        if (monitored_processes[i].pid == pid) {
            monitored_processes[i].soft_limit  = soft_limit;
            monitored_processes[i].hard_limit  = hard_limit;
            monitored_processes[i].active      = 1;
            monitored_processes[i].warned_soft = 0;
            spin_unlock_irqrestore(&monitored_lock, flags);
            return 0;
        }
    }

    if (monitored_count >= MAX_MONITORED) {
        spin_unlock_irqrestore(&monitored_lock, flags);
        return -ENOMEM;
    }

    monitored_processes[monitored_count].pid        = pid;
    monitored_processes[monitored_count].soft_limit = soft_limit;
    monitored_processes[monitored_count].hard_limit = hard_limit;
    monitored_processes[monitored_count].active     = 1;
    monitored_processes[monitored_count].warned_soft= 0;
    monitored_count++;
    spin_unlock_irqrestore(&monitored_lock, flags);

    printk(KERN_INFO "container_monitor: registered PID %d "
                     "(soft=%lu, hard=%lu)\n", pid, soft_limit, hard_limit);
    return 0;
}

static int unregister_container(pid_t pid) {
    unsigned long flags;
    int i;

    spin_lock_irqsave(&monitored_lock, flags);
    for (i = 0; i < monitored_count; i++) {
        if (monitored_processes[i].pid == pid) {
            monitored_processes[i].active = 0;
            spin_unlock_irqrestore(&monitored_lock, flags);
            printk(KERN_INFO "container_monitor: unregistered PID %d\n", pid);
            return 0;
        }
    }
    spin_unlock_irqrestore(&monitored_lock, flags);
    return -ENOENT;
}

/* ── file operations ────────────────────────────────────────────────────── */

static int device_open(struct inode *inode, struct file *file) { return 0; }
static int device_close(struct inode *inode, struct file *file) { return 0; }

static long device_ioctl(struct file *file,
                          unsigned int cmd, unsigned long arg) {
    struct container_monitor_cmd mc;

    if (copy_from_user(&mc, (void __user *)arg, sizeof(mc)))
        return -EFAULT;

    switch (cmd) {
    case IOCTL_REGISTER_CONTAINER:
    case IOCTL_SET_MEMORY_LIMIT:
        return register_container(mc.pid,
                                  mc.soft_limit_bytes,
                                  mc.hard_limit_bytes);
    case IOCTL_UNREGISTER_CONTAINER:
        return unregister_container(mc.pid);
    default:
        return -EINVAL;
    }
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = device_open,
    .release        = device_close,
    .unlocked_ioctl = device_ioctl,
};

/* ── module init / exit ─────────────────────────────────────────────────── */

static int __init monitor_init(void) {
    dev_t dev_num;
    int   ret;

    printk(KERN_INFO "container_monitor: loading\n");

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "container_monitor: alloc_chrdev_region failed\n");
        return ret;
    }
    major_num = MAJOR(dev_num);

    /* class_create() signature changed in kernel 6.4 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    monitor_class = class_create(DEVICE_NAME);
#else
    monitor_class = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(monitor_class)) {
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "container_monitor: class_create failed\n");
        return PTR_ERR(monitor_class);
    }

    monitor_device = device_create(monitor_class, NULL, dev_num,
                                   NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        class_destroy(monitor_class);
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "container_monitor: device_create failed\n");
        return PTR_ERR(monitor_device);
    }

    cdev_init(&monitor_cdev, &fops);
    monitor_cdev.owner = THIS_MODULE;
    ret = cdev_add(&monitor_cdev, dev_num, 1);
    if (ret < 0) {
        device_destroy(monitor_class, dev_num);
        class_destroy(monitor_class);
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "container_monitor: cdev_add failed\n");
        return ret;
    }

    monitor_shutdown = 0;
    monitor_thread   = kthread_run(monitor_thread_func, NULL, "container_monitor");
    if (IS_ERR(monitor_thread)) {
        cdev_del(&monitor_cdev);
        device_destroy(monitor_class, dev_num);
        class_destroy(monitor_class);
        unregister_chrdev_region(dev_num, 1);
        printk(KERN_ERR "container_monitor: kthread_run failed\n");
        return PTR_ERR(monitor_thread);
    }

    printk(KERN_INFO "container_monitor: /dev/%s ready (major %d)\n",
           DEVICE_NAME, major_num);
    return 0;
}

static void __exit monitor_exit(void) {
    dev_t dev_num = MKDEV(major_num, 0);

    printk(KERN_INFO "container_monitor: unloading\n");

    monitor_shutdown = 1;
    if (monitor_thread && !IS_ERR(monitor_thread))
        kthread_stop(monitor_thread);

    cdev_del(&monitor_cdev);
    device_destroy(monitor_class, dev_num);
    class_destroy(monitor_class);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
