#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_status;
    uint32_t payload_len;
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

struct supervisor_ctx;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int wait_status;
    int stop_requested;
    int monitor_registered;
    int producer_started;
    int producer_joined;
    char final_reason[32];
    char log_path[PATH_MAX];
    void *child_stack;
    child_config_t *child_cfg;
    pthread_t producer_thread;
    pthread_cond_t state_cond;
    struct container_record *next;
} container_record_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} string_builder_t;

typedef struct {
    struct supervisor_ctx *ctx;
    container_record_t *record;
    int read_fd;
} producer_arg_t;

typedef struct {
    struct supervisor_ctx *ctx;
    int client_fd;
} client_handler_arg_t;

typedef struct supervisor_ctx {
    int server_fd;
    int monitor_fd;
    volatile sig_atomic_t should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    char base_rootfs[PATH_MAX];
} supervisor_ctx_t;

static volatile sig_atomic_t g_sigchld_seen = 0;
static volatile sig_atomic_t g_stop_supervisor = 0;
static volatile sig_atomic_t g_client_stop_requested = 0;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int mkdir_if_missing(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    return -1;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
    const char *cursor = buf;
    size_t total = 0;

    while (total < len) {
        ssize_t written = write(fd, cursor + total, len - total);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0)
            break;
        total += (size_t)written;
    }

    return (ssize_t)total;
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
    char *cursor = buf;
    size_t total = 0;

    while (total < len) {
        ssize_t read_bytes = read(fd, cursor + total, len - total);
        if (read_bytes < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (read_bytes == 0)
            return (ssize_t)total;
        total += (size_t)read_bytes;
    }

    return (ssize_t)total;
}

static int string_builder_init(string_builder_t *builder)
{
    builder->cap = 1024;
    builder->len = 0;
    builder->data = malloc(builder->cap);
    if (!builder->data)
        return -1;
    builder->data[0] = '\0';
    return 0;
}

static void string_builder_destroy(string_builder_t *builder)
{
    free(builder->data);
    builder->data = NULL;
    builder->len = 0;
    builder->cap = 0;
}

static int string_builder_reserve(string_builder_t *builder, size_t extra)
{
    if (builder->len + extra + 1 <= builder->cap)
        return 0;

    while (builder->len + extra + 1 > builder->cap)
        builder->cap *= 2;

    char *new_data = realloc(builder->data, builder->cap);
    if (!new_data)
        return -1;

    builder->data = new_data;
    return 0;
}

static int string_builder_appendf(string_builder_t *builder, const char *fmt, ...)
{
    va_list ap;
    va_list ap_copy;
    int needed;

    va_start(ap, fmt);
    va_copy(ap_copy, ap);
    needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }

    if (string_builder_reserve(builder, (size_t)needed) != 0) {
        va_end(ap);
        return -1;
    }

    vsnprintf(builder->data + builder->len,
              builder->cap - builder->len,
              fmt,
              ap);
    va_end(ap);
    builder->len += (size_t)needed;
    return 0;
}

static int read_file_to_builder(const char *path, string_builder_t *builder)
{
    int fd;
    char buf[4096];

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    for (;;) {
        ssize_t read_bytes = read(fd, buf, sizeof(buf));
        if (read_bytes < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (read_bytes == 0)
            break;
        if (string_builder_reserve(builder, (size_t)read_bytes) != 0) {
            close(fd);
            return -1;
        }
        memcpy(builder->data + builder->len, buf, (size_t)read_bytes);
        builder->len += (size_t)read_bytes;
        builder->data[builder->len] = '\0';
    }

    close(fd);
    return 0;
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
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

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (!buffer->shutting_down && buffer->count == LOG_BUFFER_CAPACITY)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static void format_started_at(time_t started_at, char *buf, size_t buf_len)
{
    struct tm tm_now;

    if (localtime_r(&started_at, &tm_now) == NULL) {
        snprintf(buf, buf_len, "n/a");
        return;
    }

    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        int fd;

        if (item.length == 0)
            continue;

        if (mkdir_if_missing(LOG_DIR, 0755) != 0) {
            perror("mkdir logs");
            continue;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0) {
            perror("open log file");
            continue;
        }

        if (write_full(fd, item.data, item.length) < 0)
            perror("write log file");
        close(fd);

        fprintf(stderr,
                "[logger] flushed container=%s bytes=%zu\n",
                item.container_id,
                item.length);
    }

    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    if (cfg->log_write_fd >= 0) {
        if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0)
            perror("dup2 stdout");
        if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0)
            perror("dup2 stderr");
        if (cfg->log_write_fd > STDERR_FILENO)
            close(cfg->log_write_fd);
    }

    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount private");

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    perror("exec");
    return 127;
}

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static int path_matches(const char *left, const char *right)
{
    char left_real[PATH_MAX];
    char right_real[PATH_MAX];

    if (realpath(left, left_real) != NULL && realpath(right, right_real) != NULL)
        return strcmp(left_real, right_real) == 0;

    return strcmp(left, right) == 0;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *container_id)
{
    container_record_t *cursor = ctx->containers;

    while (cursor) {
        if (strcmp(cursor->id, container_id) == 0)
            return cursor;
        cursor = cursor->next;
    }

    return NULL;
}

static int container_state_is_live(container_state_t state)
{
    return state == CONTAINER_STARTING || state == CONTAINER_RUNNING;
}

static void finalize_container_locked(container_record_t *record, int status)
{
    record->wait_status = status;

    if (WIFEXITED(status)) {
        record->exit_code = WEXITSTATUS(status);
        record->exit_signal = 0;
        if (record->stop_requested) {
            record->state = CONTAINER_STOPPED;
            snprintf(record->final_reason,
                     sizeof(record->final_reason),
                     "stopped");
        } else {
            record->state = CONTAINER_EXITED;
            snprintf(record->final_reason,
                     sizeof(record->final_reason),
                     "exited");
        }
        return;
    }

    if (WIFSIGNALED(status)) {
        record->exit_signal = WTERMSIG(status);
        record->exit_code = 128 + record->exit_signal;

        if (record->stop_requested) {
            record->state = CONTAINER_STOPPED;
            snprintf(record->final_reason,
                     sizeof(record->final_reason),
                     "stopped");
        } else if (record->exit_signal == SIGKILL) {
            record->state = CONTAINER_KILLED;
            snprintf(record->final_reason,
                     sizeof(record->final_reason),
                     "hard_limit_killed");
        } else {
            record->state = CONTAINER_KILLED;
            snprintf(record->final_reason,
                     sizeof(record->final_reason),
                     "signaled");
        }
        return;
    }

    record->state = CONTAINER_EXITED;
    record->exit_code = 0;
    record->exit_signal = 0;
    snprintf(record->final_reason, sizeof(record->final_reason), "unknown");
}

static void join_container_producer(container_record_t *record)
{
    if (record->producer_started && !record->producer_joined) {
        pthread_join(record->producer_thread, NULL);
        record->producer_joined = 1;
    }
}

static void reap_children(supervisor_ctx_t *ctx)
{
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid <= 0)
            break;

        pthread_t producer_thread = 0;
        int join_needed = 0;
        int monitor_registered = 0;
        char container_id[CONTAINER_ID_LEN] = {0};

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *cursor = ctx->containers;
        while (cursor) {
            if (cursor->host_pid == pid)
                break;
            cursor = cursor->next;
        }

        if (cursor) {
            finalize_container_locked(cursor, status);
            pthread_cond_broadcast(&cursor->state_cond);
            monitor_registered = cursor->monitor_registered;
            cursor->monitor_registered = 0;
            strncpy(container_id, cursor->id, sizeof(container_id) - 1);
            if (cursor->producer_started && !cursor->producer_joined) {
                producer_thread = cursor->producer_thread;
                cursor->producer_joined = 1;
                join_needed = 1;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (monitor_registered &&
            unregister_from_monitor(ctx->monitor_fd, container_id, pid) < 0) {
            perror("unregister_from_monitor");
        }

        if (join_needed)
            pthread_join(producer_thread, NULL);
    }
}

static void signal_handler(int signo)
{
    if (signo == SIGCHLD)
        g_sigchld_seen = 1;
    if (signo == SIGINT || signo == SIGTERM)
        g_stop_supervisor = 1;
}

static void client_signal_handler(int signo)
{
    (void)signo;
    g_client_stop_requested = 1;
}

static int install_supervisor_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    return 0;
}

static int install_client_signal_handlers(struct sigaction *old_int,
                                          struct sigaction *old_term)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = client_signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, old_int) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, old_term) < 0)
        return -1;

    return 0;
}

static void restore_client_signal_handlers(const struct sigaction *old_int,
                                           const struct sigaction *old_term)
{
    sigaction(SIGINT, old_int, NULL);
    sigaction(SIGTERM, old_term, NULL);
}

static int send_response(int fd, int status, int exit_status, const char *payload)
{
    control_response_t response;
    size_t payload_len = payload ? strlen(payload) : 0;

    response.status = status;
    response.exit_status = exit_status;
    response.payload_len = (uint32_t)payload_len;

    if (write_full(fd, &response, sizeof(response)) != (ssize_t)sizeof(response))
        return -1;

    if (payload_len > 0 &&
        write_full(fd, payload, payload_len) != (ssize_t)payload_len)
        return -1;

    return 0;
}

static int receive_response(int fd,
                            int *status_out,
                            int *exit_status_out,
                            char **payload_out)
{
    control_response_t response;
    char *payload = NULL;

    if (read_full(fd, &response, sizeof(response)) != (ssize_t)sizeof(response))
        return -1;

    if (response.payload_len > 0) {
        payload = malloc((size_t)response.payload_len + 1);
        if (!payload)
            return -1;
        if (read_full(fd, payload, response.payload_len) != response.payload_len) {
            free(payload);
            return -1;
        }
        payload[response.payload_len] = '\0';
    } else {
        payload = strdup("");
        if (!payload)
            return -1;
    }

    *status_out = response.status;
    *exit_status_out = response.exit_status;
    *payload_out = payload;
    return 0;
}

static void free_record(container_record_t *record)
{
    if (!record)
        return;

    if (record->producer_started && !record->producer_joined)
        join_container_producer(record);
    if (record->child_cfg)
        free(record->child_cfg);
    if (record->child_stack)
        free(record->child_stack);
    pthread_cond_destroy(&record->state_cond);
    free(record);
}

static void stop_all_live_containers(supervisor_ctx_t *ctx)
{
    container_record_t *cursor;

    pthread_mutex_lock(&ctx->metadata_lock);
    cursor = ctx->containers;
    while (cursor) {
        if (container_state_is_live(cursor->state) && cursor->host_pid > 0) {
            cursor->stop_requested = 1;
            kill(cursor->host_pid, SIGTERM);
        }
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int live_container_count(supervisor_ctx_t *ctx)
{
    int count = 0;
    container_record_t *cursor;

    pthread_mutex_lock(&ctx->metadata_lock);
    cursor = ctx->containers;
    while (cursor) {
        if (container_state_is_live(cursor->state))
            count++;
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    return count;
}

static void destroy_all_records(supervisor_ctx_t *ctx)
{
    container_record_t *cursor;
    container_record_t *next;

    pthread_mutex_lock(&ctx->metadata_lock);
    cursor = ctx->containers;
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (cursor) {
        next = cursor->next;
        free_record(cursor);
        cursor = next;
    }
}

static void *producer_thread_main(void *arg)
{
    producer_arg_t *producer = arg;
    supervisor_ctx_t *ctx = producer->ctx;
    container_record_t *record = producer->record;
    int fd = producer->read_fd;

    free(producer);

    for (;;) {
        log_item_t item;
        ssize_t read_bytes = read(fd, item.data, sizeof(item.data));

        if (read_bytes < 0) {
            if (errno == EINTR)
                continue;
            perror("read container log pipe");
            break;
        }

        if (read_bytes == 0)
            break;

        memset(&item.container_id, 0, sizeof(item.container_id));
        strncpy(item.container_id, record->id, sizeof(item.container_id) - 1);
        item.length = (size_t)read_bytes;

        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0)
            break;

        fprintf(stderr,
                "[producer] queued container=%s bytes=%zu\n",
                record->id,
                item.length);
    }

    close(fd);
    return NULL;
}

static int create_control_socket(void)
{
    int server_fd;
    struct sockaddr_un addr;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
        return -1;

    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 16) < 0) {
        close(server_fd);
        unlink(CONTROL_PATH);
        return -1;
    }

    return server_fd;
}

static int validate_start_request_locked(supervisor_ctx_t *ctx,
                                         const control_request_t *req,
                                         char *message,
                                         size_t message_len)
{
    container_record_t *cursor = ctx->containers;

    while (cursor) {
        if (strcmp(cursor->id, req->container_id) == 0) {
            snprintf(message,
                     message_len,
                     "container id '%s' already exists",
                     req->container_id);
            return -1;
        }
        if (container_state_is_live(cursor->state) &&
            path_matches(cursor->rootfs, req->rootfs)) {
            snprintf(message,
                     message_len,
                     "rootfs '%s' is already in use by running container '%s'",
                     req->rootfs,
                     cursor->id);
            return -1;
        }
        cursor = cursor->next;
    }

    if (path_matches(ctx->base_rootfs, req->rootfs)) {
        snprintf(message,
                 message_len,
                 "container rootfs must be a writable copy, not the base rootfs");
        return -1;
    }

    return 0;
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           container_record_t **record_out,
                           char *message,
                           size_t message_len)
{
    container_record_t *record = NULL;
    child_config_t *cfg = NULL;
    producer_arg_t *producer_arg = NULL;
    void *stack = NULL;
    int pipefd[2] = {-1, -1};
    pid_t pid = -1;
    int monitor_registered = 0;

    if (mkdir_if_missing(LOG_DIR, 0755) != 0) {
        snprintf(message, message_len, "failed to create log directory");
        return -1;
    }

    if (access(req->rootfs, F_OK) != 0) {
        snprintf(message, message_len, "container rootfs does not exist: %s", req->rootfs);
        return -1;
    }

    if (pipe(pipefd) < 0) {
        snprintf(message, message_len, "pipe failed: %s", strerror(errno));
        return -1;
    }

    record = calloc(1, sizeof(*record));
    cfg = calloc(1, sizeof(*cfg));
    producer_arg = calloc(1, sizeof(*producer_arg));
    stack = malloc(STACK_SIZE);

    if (!record || !cfg || !producer_arg || !stack) {
        snprintf(message, message_len, "out of memory while creating container");
        goto fail;
    }

    memset(record, 0, sizeof(*record));
    memset(cfg, 0, sizeof(*cfg));

    strncpy(record->id, req->container_id, sizeof(record->id) - 1);
    strncpy(record->rootfs, req->rootfs, sizeof(record->rootfs) - 1);
    snprintf(record->log_path, sizeof(record->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    record->state = CONTAINER_STARTING;
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    record->started_at = time(NULL);
    snprintf(record->final_reason, sizeof(record->final_reason), "starting");
    pthread_cond_init(&record->state_cond, NULL);
    record->child_stack = stack;
    record->child_cfg = cfg;

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    producer_arg->ctx = ctx;
    producer_arg->record = record;
    producer_arg->read_fd = pipefd[0];

    pthread_mutex_lock(&ctx->metadata_lock);
    if (validate_start_request_locked(ctx, req, message, message_len) != 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        goto fail;
    }

    pid = clone(child_fn,
                (char *)stack + STACK_SIZE,
                CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "clone failed: %s", strerror(errno));
        goto fail;
    }

    close(pipefd[1]);
    pipefd[1] = -1;

    record->host_pid = pid;
    record->state = CONTAINER_RUNNING;
    snprintf(record->final_reason, sizeof(record->final_reason), "running");
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pthread_create(&record->producer_thread, NULL, producer_thread_main, producer_arg) != 0) {
        kill(pid, SIGKILL);
        snprintf(message, message_len, "failed to create producer thread");
        return -1;
    }
    record->producer_started = 1;
    producer_arg = NULL;

    if (register_with_monitor(ctx->monitor_fd,
                              record->id,
                              pid,
                              record->soft_limit_bytes,
                              record->hard_limit_bytes) == 0) {
        monitor_registered = 1;
        record->monitor_registered = 1;
    }

    snprintf(message,
             message_len,
             "started container=%s pid=%d log=%s%s",
             record->id,
             record->host_pid,
             record->log_path,
             ctx->monitor_fd < 0 ? " (monitor unavailable on this run)" :
                                   (monitor_registered ? "" : " (monitor registration failed)"));

    *record_out = record;
    return 0;

fail:
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    free(producer_arg);
    if (cfg)
        free(cfg);
    if (stack)
        free(stack);
    if (record)
        free(record);
    return -1;
}

static int build_ps_output(supervisor_ctx_t *ctx, char **payload_out)
{
    container_record_t *cursor;
    string_builder_t builder;

    if (string_builder_init(&builder) != 0)
        return -1;

    if (string_builder_appendf(&builder,
                               "%-12s %-8s %-12s %-19s %-8s %-8s %-18s %s\n",
                               "ID",
                               "PID",
                               "STATE",
                               "STARTED",
                               "SOFT",
                               "HARD",
                               "REASON",
                               "LOG") != 0) {
        string_builder_destroy(&builder);
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    cursor = ctx->containers;
    while (cursor) {
        char started[32];
        format_started_at(cursor->started_at, started, sizeof(started));
        if (string_builder_appendf(&builder,
                                   "%-12s %-8d %-12s %-19s %-8lu %-8lu %-18s %s\n",
                                   cursor->id,
                                   cursor->host_pid,
                                   state_to_string(cursor->state),
                                   started,
                                   cursor->soft_limit_bytes >> 20,
                                   cursor->hard_limit_bytes >> 20,
                                   cursor->final_reason,
                                   cursor->log_path) != 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            string_builder_destroy(&builder);
            return -1;
        }
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    *payload_out = builder.data;
    return 0;
}

static int build_logs_output(supervisor_ctx_t *ctx, const char *container_id, char **payload_out)
{
    string_builder_t builder;
    container_record_t *record;
    char log_path[PATH_MAX];

    if (string_builder_init(&builder) != 0)
        return -1;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, container_id);
    if (record)
        strncpy(log_path, record->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!record) {
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, container_id);
    }

    if (read_file_to_builder(log_path, &builder) != 0) {
        string_builder_destroy(&builder);
        return -1;
    }

    *payload_out = builder.data;
    return 0;
}

static int handle_start_or_run(supervisor_ctx_t *ctx,
                               const control_request_t *req,
                               int client_fd,
                               int wait_for_exit)
{
    container_record_t *record = NULL;
    char message[CONTROL_MESSAGE_LEN];

    memset(message, 0, sizeof(message));

    if (start_container(ctx, req, &record, message, sizeof(message)) != 0)
        return send_response(client_fd, 1, 1, message);

    if (!wait_for_exit)
        return send_response(client_fd, 0, 0, message);

    pthread_mutex_lock(&ctx->metadata_lock);
    while (container_state_is_live(record->state))
        pthread_cond_wait(&record->state_cond, &ctx->metadata_lock);

    snprintf(message,
             sizeof(message),
             "container=%s state=%s reason=%s exit_status=%d signal=%d",
             record->id,
             state_to_string(record->state),
             record->final_reason,
             record->exit_code,
             record->exit_signal);
    int exit_status = record->exit_code;
    pthread_mutex_unlock(&ctx->metadata_lock);

    return send_response(client_fd, 0, exit_status, message);
}

static int handle_stop(supervisor_ctx_t *ctx,
                       const control_request_t *req,
                       int client_fd)
{
    container_record_t *record;
    char message[CONTROL_MESSAGE_LEN];

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, req->container_id);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, sizeof(message), "container '%s' not found", req->container_id);
        return send_response(client_fd, 1, 1, message);
    }

    if (!container_state_is_live(record->state)) {
        snprintf(message,
                 sizeof(message),
                 "container '%s' is already in state=%s",
                 req->container_id,
                 state_to_string(record->state));
        pthread_mutex_unlock(&ctx->metadata_lock);
        return send_response(client_fd, 0, 0, message);
    }

    record->stop_requested = 1;
    kill(record->host_pid, SIGTERM);
    snprintf(message,
             sizeof(message),
             "stop requested for container=%s pid=%d",
             record->id,
             record->host_pid);
    pthread_mutex_unlock(&ctx->metadata_lock);

    return send_response(client_fd, 0, 0, message);
}

static void *client_handler_main(void *arg)
{
    client_handler_arg_t *client_arg = arg;
    supervisor_ctx_t *ctx = client_arg->ctx;
    int client_fd = client_arg->client_fd;
    control_request_t req;

    free(client_arg);

    memset(&req, 0, sizeof(req));
    if (read_full(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        close(client_fd);
        return NULL;
    }

    switch (req.kind) {
    case CMD_START:
        handle_start_or_run(ctx, &req, client_fd, 0);
        break;
    case CMD_RUN:
        handle_start_or_run(ctx, &req, client_fd, 1);
        break;
    case CMD_PS: {
        char *payload = NULL;
        if (build_ps_output(ctx, &payload) != 0) {
            send_response(client_fd, 1, 1, "failed to render ps output");
        } else {
            send_response(client_fd, 0, 0, payload);
            free(payload);
        }
        break;
    }
    case CMD_LOGS: {
        char *payload = NULL;
        if (build_logs_output(ctx, req.container_id, &payload) != 0) {
            send_response(client_fd, 1, 1, "failed to read log file");
        } else {
            send_response(client_fd, 0, 0, payload);
            free(payload);
        }
        break;
    }
    case CMD_STOP:
        handle_stop(ctx, &req, client_fd);
        break;
    default:
        send_response(client_fd, 1, 1, "unsupported command");
        break;
    }

    close(client_fd);
    return NULL;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    strncpy(ctx.base_rootfs, rootfs, sizeof(ctx.base_rootfs) - 1);

    if (access(rootfs, F_OK) != 0) {
        perror("base rootfs");
        return 1;
    }

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (mkdir_if_missing(LOG_DIR, 0755) != 0)
        perror("mkdir logs");

    if (install_supervisor_signal_handlers() < 0) {
        perror("sigaction");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr,
                "[supervisor] monitor device unavailable, continuing without kernel enforcement: %s\n",
                strerror(errno));
    }

    ctx.server_fd = create_control_socket();
    if (ctx.server_fd < 0) {
        perror("create_control_socket");
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger_thread");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    fprintf(stderr,
            "[supervisor] listening on %s with base rootfs %s\n",
            CONTROL_PATH,
            rootfs);

    while (!ctx.should_stop) {
        struct pollfd pfd;
        int poll_rc;

        if (g_stop_supervisor) {
            ctx.should_stop = 1;
            stop_all_live_containers(&ctx);
        }

        if (g_sigchld_seen) {
            g_sigchld_seen = 0;
            reap_children(&ctx);
        }

        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        poll_rc = poll(&pfd, 1, 250);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (poll_rc == 0)
            continue;
        if (pfd.revents & POLLIN) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR)
                    continue;
                perror("accept");
                break;
            }

            client_handler_arg_t *client_arg = calloc(1, sizeof(*client_arg));
            pthread_t handler_thread;

            if (!client_arg) {
                close(client_fd);
                continue;
            }

            client_arg->ctx = &ctx;
            client_arg->client_fd = client_fd;

            if (pthread_create(&handler_thread, NULL, client_handler_main, client_arg) != 0) {
                perror("pthread_create client_handler");
                close(client_fd);
                free(client_arg);
                continue;
            }

            pthread_detach(handler_thread);
        }
    }

    stop_all_live_containers(&ctx);
    while (live_container_count(&ctx) > 0) {
        reap_children(&ctx);
        usleep(200000);
    }
    reap_children(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    destroy_all_records(&ctx);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "[supervisor] clean shutdown complete\n");
    return 0;
}

static int connect_control_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int send_stop_request(const char *container_id)
{
    control_request_t req;
    int fd;
    int status;
    int exit_status;
    char *payload = NULL;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    fd = connect_control_socket();
    if (fd < 0)
        return -1;

    if (write_full(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        close(fd);
        return -1;
    }

    if (receive_response(fd, &status, &exit_status, &payload) == 0) {
        fprintf(stderr, "%s\n", payload);
        free(payload);
    }

    close(fd);
    return 0;
}

static int wait_for_run_response(int fd, const char *container_id)
{
    struct sigaction old_int;
    struct sigaction old_term;
    int status;
    int exit_status;
    char *payload = NULL;

    g_client_stop_requested = 0;
    if (install_client_signal_handlers(&old_int, &old_term) < 0) {
        perror("sigaction");
        return 1;
    }

    for (;;) {
        struct pollfd pfd;
        int poll_rc;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        poll_rc = poll(&pfd, 1, 250);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            restore_client_signal_handlers(&old_int, &old_term);
            return 1;
        }

        if (g_client_stop_requested) {
            g_client_stop_requested = 0;
            if (send_stop_request(container_id) < 0)
                fprintf(stderr, "failed to forward stop request for %s\n", container_id);
        }

        if (poll_rc == 0)
            continue;

        if (receive_response(fd, &status, &exit_status, &payload) != 0) {
            restore_client_signal_handlers(&old_int, &old_term);
            fprintf(stderr, "failed to receive supervisor response\n");
            return 1;
        }
        break;
    }

    restore_client_signal_handlers(&old_int, &old_term);

    if (payload && *payload)
        printf("%s\n", payload);

    if (status != 0) {
        free(payload);
        return 1;
    }

    free(payload);
    return exit_status;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    int status;
    int exit_status;
    char *payload = NULL;

    fd = connect_control_socket();
    if (fd < 0) {
        perror("connect control socket");
        return 1;
    }

    if (write_full(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_RUN) {
        int rc = wait_for_run_response(fd, req->container_id);
        close(fd);
        return rc;
    }

    if (receive_response(fd, &status, &exit_status, &payload) != 0) {
        fprintf(stderr, "failed to receive supervisor response\n");
        close(fd);
        return 1;
    }

    if (payload && *payload) {
        if (status == 0)
            printf("%s\n", payload);
        else
            fprintf(stderr, "%s\n", payload);
    }

    free(payload);
    close(fd);
    return status == 0 ? 0 : exit_status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
