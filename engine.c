#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

#include "monitor_ioctl.h"

#define MAX_CONTAINERS 64
#define MAX_CMD_LEN    1024
#define MAX_ID_LEN     32
#define MAX_PATH_LEN   256
#define SOCKET_PATH    "/tmp/engine_supervisor.sock"
#define LOG_BUFFER_SIZE 65536
#define LOG_ENTRY_SIZE  512
#define CHILD_STACK_SIZE 65536
#define RESPONSE_SIZE   8192

typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
} container_state_t;

typedef struct {
    char id[MAX_ID_LEN];
    pid_t host_pid;
    container_state_t state;
    unsigned long soft_limit_mib;
    unsigned long hard_limit_mib;
    int nice_value;
    char log_path[MAX_PATH_LEN];
    time_t start_time;
    int exit_status;
    int exit_signal;
    int stop_requested;
    int hard_limit_killed;
} container_t;

typedef struct {
    container_t containers[MAX_CONTAINERS];
    int count;
    pthread_mutex_t lock;
} container_metadata_t;

typedef struct {
    char data[LOG_ENTRY_SIZE];
    int length;
    char container_id[MAX_ID_LEN];
} log_entry_t;

typedef struct {
    log_entry_t entries[LOG_BUFFER_SIZE / LOG_ENTRY_SIZE];
    int head;
    int tail;
    int size;
    int capacity;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    int shutdown;
} bounded_buffer_t;

typedef struct {
    int pipe_fd;
    char container_id[MAX_ID_LEN];
} pipe_reader_t;

/* Args passed to container_main via clone() */
typedef struct {
    char rootfs[MAX_PATH_LEN];
    char cmd[MAX_CMD_LEN];
    int  pipe_write_fd;
    int  nice_val;
} container_args_t;

static container_metadata_t metadata;
static bounded_buffer_t     log_buffer;
static int                  supervisor_socket = -1;
static volatile sig_atomic_t should_shutdown  = 0;
static volatile sig_atomic_t child_exited     = 0;

/* ── bounded buffer ─────────────────────────────────────────────────────── */

static void init_supervisor(void) {
    pthread_mutex_init(&metadata.lock, NULL);
    pthread_mutex_init(&log_buffer.lock, NULL);
    pthread_cond_init(&log_buffer.not_full, NULL);
    pthread_cond_init(&log_buffer.not_empty, NULL);
    metadata.count       = 0;
    log_buffer.head      = 0;
    log_buffer.tail      = 0;
    log_buffer.size      = 0;
    log_buffer.capacity  = LOG_BUFFER_SIZE / LOG_ENTRY_SIZE;
    log_buffer.shutdown  = 0;
}

static int buffer_enqueue(const char *container_id, const char *data, int len) {
    if (len > LOG_ENTRY_SIZE - 1) len = LOG_ENTRY_SIZE - 1;

    pthread_mutex_lock(&log_buffer.lock);
    while (log_buffer.size >= log_buffer.capacity && !log_buffer.shutdown)
        pthread_cond_wait(&log_buffer.not_full, &log_buffer.lock);

    if (log_buffer.shutdown) {
        pthread_mutex_unlock(&log_buffer.lock);
        return -1;
    }

    log_entry_t *e = &log_buffer.entries[log_buffer.tail];
    strncpy(e->container_id, container_id, MAX_ID_LEN - 1);
    memcpy(e->data, data, len);
    e->data[len] = '\0';
    e->length    = len;

    log_buffer.tail = (log_buffer.tail + 1) % log_buffer.capacity;
    log_buffer.size++;
    pthread_cond_signal(&log_buffer.not_empty);
    pthread_mutex_unlock(&log_buffer.lock);
    return 0;
}

static int buffer_dequeue(log_entry_t *entry) {
    pthread_mutex_lock(&log_buffer.lock);
    while (log_buffer.size == 0 && !log_buffer.shutdown)
        pthread_cond_wait(&log_buffer.not_empty, &log_buffer.lock);

    if (log_buffer.size == 0) {
        pthread_mutex_unlock(&log_buffer.lock);
        return -1;
    }

    *entry = log_buffer.entries[log_buffer.head];
    log_buffer.head = (log_buffer.head + 1) % log_buffer.capacity;
    log_buffer.size--;
    pthread_cond_signal(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.lock);
    return 0;
}

/* ── logging threads ────────────────────────────────────────────────────── */

static void *logging_producer(void *arg) {
    pipe_reader_t *reader = (pipe_reader_t *)arg;
    char buf[512];
    int  nread;

    while ((nread = read(reader->pipe_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[nread] = '\0';
        buffer_enqueue(reader->container_id, buf, nread);
    }

    close(reader->pipe_fd);
    free(reader);
    return NULL;
}

static void *logging_consumer(void *arg) {
    (void)arg;
    log_entry_t entry;

    while (buffer_dequeue(&entry) == 0) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "/tmp/container-%s.log", entry.container_id);
        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(entry.data, 1, entry.length, f);
            fflush(f);
            fclose(f);
        }
    }
    return NULL;
}

/* ── arg parsing ────────────────────────────────────────────────────────── */

/* Split cmd into tokens, storing strdup'd pointers in argv_out. Caller frees. */
static int parse_args(const char *cmd, char **argv_out, int max_args) {
    char *copy  = strdup(cmd);
    char *token;
    int   count = 0;

    token = strtok(copy, " \t");
    while (token && count < max_args - 1) {
        argv_out[count++] = strdup(token);
        token = strtok(NULL, " \t");
    }
    argv_out[count] = NULL;
    free(copy);
    return count;
}

/* ── container entry point ──────────────────────────────────────────────── */

/* Runs in the child process created by clone(). */
static int container_main(void *arg) {
    container_args_t *a = (container_args_t *)arg;

    /* Redirect stdout + stderr to logging pipe */
    dup2(a->pipe_write_fd, STDOUT_FILENO);
    dup2(a->pipe_write_fd, STDERR_FILENO);
    close(a->pipe_write_fd);

    /* Apply scheduling priority */
    if (a->nice_val != 0)
        setpriority(PRIO_PROCESS, 0, a->nice_val);

    /* chroot into the container's rootfs */
    if (chroot(a->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/")        < 0) { perror("chdir");  return 1; }

    /* Mount /proc *after* chroot so it lands inside the container */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");   /* non-fatal */

    /* Parse and exec the command */
    char *argv[64];
    char  buf[MAX_CMD_LEN];
    strncpy(buf, a->cmd, MAX_CMD_LEN - 1);

    int   n   = 0;
    char *tok = strtok(buf, " \t");
    while (tok && n < 63) { argv[n++] = tok; tok = strtok(NULL, " \t"); }
    argv[n] = NULL;

    if (n == 0) { fprintf(stderr, "no command\n"); return 1; }

    execvp(argv[0], argv);
    perror("execvp");
    return 1;
}

/* ── kernel module integration ──────────────────────────────────────────── */

static void register_with_monitor(pid_t pid,
                                   unsigned long soft_mib,
                                   unsigned long hard_mib) {
    int fd = open(CONTAINER_MONITOR_DEVICE, O_RDWR);
    if (fd < 0) return;
    struct container_monitor_cmd c = {
        .pid              = pid,
        .soft_limit_bytes = soft_mib * 1024 * 1024,
        .hard_limit_bytes = hard_mib * 1024 * 1024,
    };
    ioctl(fd, IOCTL_REGISTER_CONTAINER, &c);
    close(fd);
}

static void unregister_from_monitor(pid_t pid) {
    int fd = open(CONTAINER_MONITOR_DEVICE, O_RDWR);
    if (fd < 0) return;
    struct container_monitor_cmd c = { .pid = pid };
    ioctl(fd, IOCTL_UNREGISTER_CONTAINER, &c);
    close(fd);
}

/* ── container lifecycle ────────────────────────────────────────────────── */

/* Returns child host PID on success, -1 on error. */
static pid_t start_container(const char *id, const char *rootfs, const char *cmd,
                              unsigned long soft_mib, unsigned long hard_mib,
                              int nice_val) {
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    /* pipefd[0] is read-only in the parent; close it in the exec'd child */
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);

    char *stack = malloc(CHILD_STACK_SIZE);
    if (!stack) { close(pipefd[0]); close(pipefd[1]); return -1; }

    container_args_t *args = malloc(sizeof(container_args_t));
    if (!args) { free(stack); close(pipefd[0]); close(pipefd[1]); return -1; }

    strncpy(args->rootfs, rootfs, MAX_PATH_LEN - 1);
    strncpy(args->cmd,    cmd,    MAX_CMD_LEN  - 1);
    args->pipe_write_fd = pipefd[1];
    args->nice_val      = nice_val;

    pid_t child = clone(container_main,
                        stack + CHILD_STACK_SIZE,
                        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                        args);

    if (child < 0) {
        perror("clone");
        free(stack); free(args);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    /* Parent closes write end; child has its own via dup2 */
    close(pipefd[1]);
    /* stack/args intentionally not freed: child uses them until execvp replaces image */

    /* Record metadata */
    pthread_mutex_lock(&metadata.lock);
    if (metadata.count >= MAX_CONTAINERS) {
        pthread_mutex_unlock(&metadata.lock);
        kill(child, SIGKILL);
        close(pipefd[0]);
        return -1;
    }
    container_t *c = &metadata.containers[metadata.count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, MAX_ID_LEN - 1);
    c->host_pid       = child;
    c->state          = STATE_RUNNING;
    c->soft_limit_mib = soft_mib;
    c->hard_limit_mib = hard_mib;
    c->nice_value     = nice_val;
    c->start_time     = time(NULL);
    snprintf(c->log_path, sizeof(c->log_path), "/tmp/container-%s.log", id);
    pthread_mutex_unlock(&metadata.lock);

    /* Register with kernel memory monitor (best-effort) */
    register_with_monitor(child, soft_mib, hard_mib);

    /* Logging producer thread reads from pipe and feeds bounded buffer */
    pipe_reader_t *reader = malloc(sizeof(pipe_reader_t));
    if (reader) {
        reader->pipe_fd = pipefd[0];
        strncpy(reader->container_id, id, MAX_ID_LEN - 1);
        pthread_t t;
        pthread_create(&t, NULL, logging_producer, reader);
        pthread_detach(t);
    } else {
        close(pipefd[0]);
    }

    return child;
}

/* Called from main thread only (not signal handler). */
static void reap_children(void) {
    int    status;
    pid_t  pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&metadata.lock);
        for (int i = 0; i < metadata.count; i++) {
            container_t *c = &metadata.containers[i];
            if (c->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                c->exit_status = WEXITSTATUS(status);
                c->exit_signal = 0;
                c->state       = STATE_STOPPED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                if (WTERMSIG(status) == SIGKILL && !c->stop_requested) {
                    c->hard_limit_killed = 1;
                    c->state = STATE_KILLED;
                } else {
                    c->state = STATE_STOPPED;
                }
            }
            unregister_from_monitor(pid);
            break;
        }
        pthread_mutex_unlock(&metadata.lock);
    }
}

static void stop_container(const char *id) {
    pthread_mutex_lock(&metadata.lock);
    for (int i = 0; i < metadata.count; i++) {
        container_t *c = &metadata.containers[i];
        if (strcmp(c->id, id) != 0) continue;
        c->stop_requested = 1;
        if (c->state == STATE_RUNNING) {
            kill(c->host_pid, SIGTERM);
            c->state = STATE_STOPPED;
        }
        break;
    }
    pthread_mutex_unlock(&metadata.lock);
}

/* ── signal handler ─────────────────────────────────────────────────────── */

/* Keep signal handler minimal: no mutex, no memory allocation. */
static void signal_handler(int sig) {
    if (sig == SIGCHLD)
        child_exited = 1;
    else if (sig == SIGTERM || sig == SIGINT)
        should_shutdown = 1;
}

/* ── CLI command handler ─────────────────────────────────────────────────── */

static void handle_command(const char *cmd_line, char *resp, int resp_size) {
    char  cmd_copy[MAX_CMD_LEN];
    char *argv[32];
    int   nargs, pos = 0;

    strncpy(cmd_copy, cmd_line, MAX_CMD_LEN - 1);
    nargs = parse_args(cmd_copy, argv, 32);

    if (nargs == 0) {
        snprintf(resp, resp_size, "ERROR: empty command\n");
        goto cleanup;
    }

    /* ── supervisor ── */
    if (strcmp(argv[0], "supervisor") == 0) {
        snprintf(resp, resp_size, "Supervisor running\n");

    /* ── start / run ── */
    } else if (strcmp(argv[0], "start") == 0 || strcmp(argv[0], "run") == 0) {
        int is_run = (strcmp(argv[0], "run") == 0);

        if (nargs < 4) {
            snprintf(resp, resp_size,
                     "ERROR: %s <id> <rootfs> <command> [opts]\n", argv[0]);
            goto cleanup;
        }

        char id[MAX_ID_LEN], rootfs[MAX_PATH_LEN], run_cmd[MAX_CMD_LEN];
        unsigned long soft_mib = 40, hard_mib = 64;
        int nice_val = 0;

        strncpy(id,      argv[1], MAX_ID_LEN  - 1);
        strncpy(rootfs,  argv[2], MAX_PATH_LEN - 1);
        strncpy(run_cmd, argv[3], MAX_CMD_LEN  - 1);

        /* Parse optional flags; non-flag tokens are additional command args */
        for (int i = 4; i < nargs; i++) {
            if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < nargs) {
                soft_mib = strtoul(argv[++i], NULL, 10);
            } else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < nargs) {
                hard_mib = strtoul(argv[++i], NULL, 10);
            } else if (strcmp(argv[i], "--nice") == 0 && i + 1 < nargs) {
                nice_val = atoi(argv[++i]);
            } else {
                strncat(run_cmd, " ",      MAX_CMD_LEN - strlen(run_cmd) - 1);
                strncat(run_cmd, argv[i],  MAX_CMD_LEN - strlen(run_cmd) - 1);
            }
        }

        if (is_run) {
            /* Block SIGCHLD so we can waitpid without racing the signal handler */
            sigset_t blk, old;
            sigemptyset(&blk);
            sigaddset(&blk, SIGCHLD);
            pthread_sigmask(SIG_BLOCK, &blk, &old);

            pid_t run_pid = start_container(id, rootfs, run_cmd,
                                            soft_mib, hard_mib, nice_val);
            if (run_pid < 0) {
                pthread_sigmask(SIG_SETMASK, &old, NULL);
                snprintf(resp, resp_size,
                         "ERROR: failed to create container %s\n", id);
                goto cleanup;
            }

            /* Block until this container exits */
            int stat = 0;
            waitpid(run_pid, &stat, 0);

            pthread_mutex_lock(&metadata.lock);
            for (int i = 0; i < metadata.count; i++) {
                container_t *c = &metadata.containers[i];
                if (c->host_pid != run_pid) continue;
                if (WIFEXITED(stat)) {
                    c->exit_status = WEXITSTATUS(stat);
                    c->state       = STATE_STOPPED;
                } else if (WIFSIGNALED(stat)) {
                    c->exit_signal = WTERMSIG(stat);
                    if (WTERMSIG(stat) == SIGKILL && !c->stop_requested)
                        c->hard_limit_killed = 1;
                    c->state = STATE_STOPPED;
                }
                break;
            }
            pthread_mutex_unlock(&metadata.lock);
            pthread_sigmask(SIG_SETMASK, &old, NULL);

            int code = WIFEXITED(stat) ? WEXITSTATUS(stat)
                                       : 128 + WTERMSIG(stat);
            snprintf(resp, resp_size,
                     "Container %s exited with code %d\n", id, code);

        } else {
            pid_t pid = start_container(id, rootfs, run_cmd,
                                        soft_mib, hard_mib, nice_val);
            if (pid < 0)
                snprintf(resp, resp_size,
                         "ERROR: failed to create container %s\n", id);
            else
                snprintf(resp, resp_size,
                         "Container %s started (pid %d)\n", id, (int)pid);
        }

    /* ── ps ── */
    } else if (strcmp(argv[0], "ps") == 0) {
        pos += snprintf(resp + pos, resp_size - pos,
                        "%-12s  %-7s  %-10s  %-10s  %-10s\n",
                        "ID", "PID", "State", "Soft(MiB)", "Hard(MiB)");
        pthread_mutex_lock(&metadata.lock);
        for (int i = 0; i < metadata.count && pos < resp_size - 128; i++) {
            container_t *c = &metadata.containers[i];
            const char  *st;
            switch (c->state) {
                case STATE_STARTING: st = "starting"; break;
                case STATE_RUNNING:  st = "running";  break;
                case STATE_STOPPED:  st = "stopped";  break;
                case STATE_KILLED:   st = "killed";   break;
                default:             st = "unknown";  break;
            }
            pos += snprintf(resp + pos, resp_size - pos,
                            "%-12s  %-7d  %-10s  %-10lu  %-10lu\n",
                            c->id, c->host_pid, st,
                            c->soft_limit_mib, c->hard_limit_mib);
        }
        pthread_mutex_unlock(&metadata.lock);

    /* ── logs ── */
    } else if (strcmp(argv[0], "logs") == 0) {
        if (nargs < 2) {
            snprintf(resp, resp_size, "ERROR: logs <id>\n");
            goto cleanup;
        }
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "/tmp/container-%s.log", argv[1]);
        FILE *f = fopen(path, "r");
        if (!f) {
            snprintf(resp, resp_size,
                     "No logs for container %s\n", argv[1]);
            goto cleanup;
        }
        char buf[512];
        while (fgets(buf, sizeof(buf), f) && pos < resp_size - 512)
            pos += snprintf(resp + pos, resp_size - pos, "%s", buf);
        fclose(f);
        if (pos == 0)
            snprintf(resp, resp_size, "(log is empty)\n");

    /* ── stop ── */
    } else if (strcmp(argv[0], "stop") == 0) {
        if (nargs < 2) {
            snprintf(resp, resp_size, "ERROR: stop <id>\n");
            goto cleanup;
        }
        stop_container(argv[1]);
        snprintf(resp, resp_size, "Container %s stopped\n", argv[1]);

    } else {
        snprintf(resp, resp_size, "ERROR: unknown command '%s'\n", argv[0]);
    }

cleanup:
    for (int i = 0; i < nargs; i++) free(argv[i]);
}

/* ── supervisor main loop ───────────────────────────────────────────────── */

static void accept_cli_connections(void) {
    char resp[RESPONSE_SIZE];
    char cmd_buf[MAX_CMD_LEN];

    while (!should_shutdown) {
        /* Reap any zombies from the main thread (safe to lock here) */
        if (child_exited) {
            child_exited = 0;
            reap_children();
        }

        /* Use select with 1-second timeout so we can check flags */
        fd_set rfds;
        struct timeval tv = {1, 0};
        FD_ZERO(&rfds);
        FD_SET(supervisor_socket, &rfds);
        int sel = select(supervisor_socket + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) { if (errno == EINTR) continue; break; }
        if (sel == 0) continue;

        int client = accept(supervisor_socket, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; break; }

        int n = read(client, cmd_buf, MAX_CMD_LEN - 1);
        if (n > 0) {
            cmd_buf[n] = '\0';
            memset(resp, 0, sizeof(resp));
            handle_command(cmd_buf, resp, sizeof(resp));
            if (resp[0] == '\0') snprintf(resp, sizeof(resp), "OK\n");
            write(client, resp, strlen(resp));
        }
        close(client);
    }
}

static int run_supervisor(const char *rootfs_base) {
    struct sockaddr_un addr;
    pthread_t consumer_thread;

    init_supervisor();

    supervisor_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (supervisor_socket < 0) { perror("socket"); return 1; }

    unlink(SOCKET_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(supervisor_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(supervisor_socket); return 1;
    }
    if (listen(supervisor_socket, 5) < 0) {
        perror("listen"); close(supervisor_socket); return 1;
    }

    pthread_create(&consumer_thread, NULL, logging_consumer, NULL);

    /* Use sigaction so SA_RESTART keeps accept() alive on SIGCHLD */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    printf("Supervisor started, listening on %s\n", SOCKET_PATH);
    printf("Base rootfs: %s\n", rootfs_base);
    fflush(stdout);

    accept_cli_connections();

    printf("Supervisor shutting down...\n");
    fflush(stdout);

    /* Ask running containers to exit */
    pthread_mutex_lock(&metadata.lock);
    for (int i = 0; i < metadata.count; i++) {
        if (metadata.containers[i].state == STATE_RUNNING)
            kill(metadata.containers[i].host_pid, SIGTERM);
    }
    pthread_mutex_unlock(&metadata.lock);

    /* Drain logging pipeline */
    pthread_mutex_lock(&log_buffer.lock);
    log_buffer.shutdown = 1;
    pthread_cond_broadcast(&log_buffer.not_empty);
    pthread_cond_broadcast(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.lock);
    pthread_join(consumer_thread, NULL);

    close(supervisor_socket);
    unlink(SOCKET_PATH);
    return 0;
}

/* ── CLI client ─────────────────────────────────────────────────────────── */

static int run_cli(int argc, char **argv) {
    struct sockaddr_un addr;
    char cmd_buf[MAX_CMD_LEN]  = {0};
    char resp_buf[RESPONSE_SIZE] = {0};

    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd_buf, " ",    MAX_CMD_LEN - strlen(cmd_buf) - 1);
        strncat(cmd_buf, argv[i], MAX_CMD_LEN - strlen(cmd_buf) - 1);
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERROR: cannot connect to supervisor. Is it running?\n");
        close(sock);
        return 1;
    }

    write(sock, cmd_buf, strlen(cmd_buf));

    int n = read(sock, resp_buf, sizeof(resp_buf) - 1);
    if (n > 0) { resp_buf[n] = '\0'; printf("%s", resp_buf); }

    close(sock);
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s supervisor <base-rootfs>\n", argv[0]);
        fprintf(stderr, "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        fprintf(stderr, "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        fprintf(stderr, "  %s ps\n", argv[0]);
        fprintf(stderr, "  %s logs <id>\n", argv[0]);
        fprintf(stderr, "  %s stop <id>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    return run_cli(argc, argv);
}
