#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define CONTAINER_MONITOR_DEVICE "/dev/container_monitor"

/* ioctl command to register a container process with the kernel monitor */
#define IOCTL_REGISTER_CONTAINER _IOW('C', 1, struct container_monitor_cmd)
#define IOCTL_UNREGISTER_CONTAINER _IOW('C', 2, struct container_monitor_cmd)
#define IOCTL_SET_MEMORY_LIMIT _IOW('C', 3, struct container_monitor_cmd)

struct container_monitor_cmd {
    pid_t pid;           /* Host PID of container process */
    unsigned long soft_limit_bytes;  /* Soft memory limit */
    unsigned long hard_limit_bytes;  /* Hard memory limit */
};

#endif /* MONITOR_IOCTL_H */
