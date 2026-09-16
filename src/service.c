#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <lebirun.h>
#include <dirent.h>
#include "service.h"
#include "log.h"

#define AUTH_WARM_TRIES 5
#define SERVICE_BOOTSTRAP_SIZE 2048

typedef struct {
    size_t mapping_size;
} service_storage_t;

static union {
    uint64_t alignment;
    unsigned char data[SERVICE_BOOTSTRAP_SIZE];
} service_bootstrap;
static int service_bootstrap_used;

static int size_add_checked(size_t left, size_t right, size_t *result)
{
    if (right > (size_t)-1 - left)
        return -1;
    *result = left + right;
    return 0;
}

static int size_mul_checked(size_t left, size_t right, size_t *result)
{
    if (left && right > (size_t)-1 / left)
        return -1;
    *result = left * right;
    return 0;
}

static void service_write_number(int fd, int value)
{
    char buffer[16];
    unsigned int number;
    int position;

    position = sizeof(buffer);
    number = value < 0 ? (unsigned int)(-(value + 1)) + 1 :
             (unsigned int)value;
    do {
        buffer[--position] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    if (value < 0)
        buffer[--position] = '-';
    write(fd, buffer + position, sizeof(buffer) - (size_t)position);
}

static int service_make_path(char *buffer, size_t capacity,
                             const char *name)
{
    size_t base_length;
    size_t name_length;

    base_length = sizeof(SERVICES_DIR) - 1;
    name_length = strlen(name);
    if (name_length > capacity || base_length + 2 > capacity - name_length)
        return -1;
    memcpy(buffer, SERVICES_DIR, base_length);
    buffer[base_length] = '/';
    memcpy(buffer + base_length + 1, name, name_length + 1);
    return 0;
}

static void service_storage_release(lservice_t *svcs)
{
    service_storage_t *storage;

    if (!svcs)
        return;
    storage = (service_storage_t *)(void *)svcs - 1;
    if (storage->mapping_size) {
        munmap(storage, storage->mapping_size);
    } else {
        service_bootstrap_used = 0;
    }
}

static void service_exec_error(const char *path)
{
    int error;

    error = errno;
    write(2, "LebInit: exec ", sizeof("LebInit: exec ") - 1);
    write(2, path, strlen(path));
    write(2, " failed with errno ", sizeof(" failed with errno ") - 1);
    service_write_number(2, error);
    write(2, "\n", 1);
}

static void service_console_error(int console_num)
{
    write(2, "LebInit: console_setid(",
          sizeof("LebInit: console_setid(") - 1);
    service_write_number(2, console_num);
    write(2, ") failed\n", sizeof(") failed\n") - 1);
}

static int service_exec_available(const char *path)
{
    unsigned char header[4];
    int fd;
    int size;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size = read(fd, header, sizeof(header));
    close(fd);
    if (size != (int)sizeof(header))
        return -1;
    if (header[0] != 0x7F || header[1] != 'E' ||
        header[2] != 'L' || header[3] != 'F')
        return -1;
    return 0;
}

static int service_console_available(int console_num)
{
    char digits[sizeof(unsigned int) * 3];
    char path[sizeof("/dev/tty") + sizeof(digits)];
    struct vt_mode mode;
    unsigned int number;
    int digit_count;
    int position;
    int display_mode;
    int fd;
    int i;

    memcpy(path, "/dev/tty", 8);
    number = (unsigned int)console_num;
    digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    position = 8;
    for (i = digit_count - 1; i >= 0; i--)
        path[position++] = digits[i];
    path[position] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 1;
    display_mode = KD_TEXT;
    memset(&mode, 0, sizeof(mode));
    if (ioctl(fd, KDGETMODE, &display_mode) < 0 ||
        ioctl(fd, VT_GETMODE, &mode) < 0) {
        close(fd);
        return 1;
    }
    close(fd);
    return display_mode == KD_TEXT && mode.mode != VT_PROCESS;
}

static int warm_auth_file(const char *path)
{
    int fd;
    char buf[512];
    int n;
    int total;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    total = 0;
    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    close(fd);

    if (total > 0)
        return 0;
    return -1;
}

static int warm_auth_files(void)
{
    int i;

    for (i = 0; i < AUTH_WARM_TRIES; i++) {
        if (warm_auth_file("/etc/passwd") == 0 &&
            warm_auth_file("/etc/shadow") == 0)
            return 0;
        sleep(1);
    }
    return -1;
}

int get_num_consoles(void)
{
    int fd;
    char buf[256];
    int n;
    char *p;
    int val;

    val = 2;

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return val;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return val;
    buf[n] = '\0';

    p = strstr(buf, "consoles=");
    if (p) {
        p += 9;
        val = atoi(p);
        if (val < 1)
            val = 1;
        if (val > 16)
            val = 16;
    }
    return val;
}

static void service_clear(lservice_t *svc)
{
    if (!svc)
        return;
    memset(svc, 0, sizeof(*svc));
}

static int service_alloc_instances(lservice_t *svc, int count,
                                   int *instance_pids)
{
    int i;

    if (!instance_pids || count < 1)
        return -1;
    svc->instance_pids = instance_pids;
    svc->instance_count = count;
    for (i = 0; i < count; i++)
        svc->instance_pids[i] = -1;
    return 0;
}

static int service_spawn_instance(lservice_t *svc, int instance)
{
    int pid;
    char num_str[4];
    char *argv[3];
    char *envp[2];

    if (instance >= 10) {
        num_str[0] = '0' + (instance / 10);
        num_str[1] = '0' + (instance % 10);
        num_str[2] = '\0';
    } else {
        num_str[0] = '0' + instance;
        num_str[1] = '\0';
    }

    pid = fork();
    if (pid < 0) {
        svc->instance_pids[instance] = -1;
        return -1;
    }
    if (pid == 0) {
        if (strcmp(svc->exec_start, GETTY_PATH) != 0) {
            setsid();
            if (console_setid(instance) < 0) {
                service_console_error(instance);
                _exit(126);
            }
            ioctl(0, TIOCSCTTY, 0);
            tcsetpgrp(0, getpid());
        }
        argv[0] = svc->exec_start;
        argv[1] = num_str;
        argv[2] = (char *)0;
        envp[0] = "TERM=linux";
        envp[1] = (char *)0;
        execve(svc->exec_start, argv, envp);
        service_exec_error(svc->exec_start);
        _exit(127);
    }
    svc->instance_pids[instance] = pid;
    return pid;
}

static void strip_trailing(char *s)
{
    int len;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
           s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

static char *skip_space(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int parse_lservice(const char *path, lservice_t *svc,
                          char *config_data, size_t config_capacity,
                          int *instance_pids,
                          int console_count)
{
    int fd;
    ssize_t n;
    char *line;
    char *next;
    char *eq;
    char *key;
    char *val;
    int in_about;
    int in_exec;
    int in_health;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (config_capacity < 2) {
        close(fd);
        return -1;
    }
    n = read(fd, config_data, config_capacity - 1);
    close(fd);
    if (n <= 0)
        return -1;
    config_data[n] = '\0';

    memset(svc, 0, sizeof(*svc));
    svc->name = "";
    svc->description = "";
    svc->exec_start = "";
    svc->exec_stop = "";
    svc->after = "";
    svc->before = "";
    svc->start_msg = "";
    svc->ok_msg = "";
    svc->type = SVC_TYPE_DAEMON;
    svc->restart = SVC_RESTART_NO;
    svc->restart_delay = 3;
    svc->instances = SVC_INSTANCES_NONE;
    svc->pid = -1;
    svc->silent = 0;
    svc->console = -1;

    in_about = 0;
    in_exec = 0;
    in_health = 0;

    line = config_data;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }

        strip_trailing(line);
        key = skip_space(line);

        if (*key == '#' || *key == ';' || *key == '\0') {
            line = next;
            continue;
        }

        if (strcmp(key, "[About]") == 0) {
            in_about = 1;
            in_exec = 0;
            in_health = 0;
            line = next;
            continue;
        }
        if (strcmp(key, "[Exec]") == 0) {
            in_about = 0;
            in_exec = 1;
            in_health = 0;
            line = next;
            continue;
        }
        if (strcmp(key, "[Health]") == 0) {
            in_about = 0;
            in_exec = 0;
            in_health = 1;
            line = next;
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) {
            line = next;
            continue;
        }

        *eq = '\0';
        strip_trailing(line);
        key = skip_space(line);
        val = skip_space(eq + 1);

        if (in_about) {
            if (strcmp(key, "Name") == 0)
                svc->name = val;
            else if (strcmp(key, "Description") == 0)
                svc->description = val;
            else if (strcmp(key, "StartMsg") == 0)
                svc->start_msg = val;
            else if (strcmp(key, "OkMsg") == 0)
                svc->ok_msg = val;
            else if (strcmp(key, "Silent") == 0)
                svc->silent = (strcmp(val, "yes") == 0 || strcmp(val, "1") == 0);
        } else if (in_exec) {
            if (strcmp(key, "Start") == 0)
                svc->exec_start = val;
            else if (strcmp(key, "Stop") == 0)
                svc->exec_stop = val;
            else if (strcmp(key, "Type") == 0) {
                if (strcmp(val, "oneshot") == 0)
                    svc->type = SVC_TYPE_ONESHOT;
                else if (strcmp(val, "forking") == 0)
                    svc->type = SVC_TYPE_FORKING;
                else
                    svc->type = SVC_TYPE_DAEMON;
            } else if (strcmp(key, "Instances") == 0) {
                if (strcmp(val, "auto") == 0)
                    svc->instances = SVC_INSTANCES_AUTO;
            } else if (strcmp(key, "Console") == 0) {
                svc->console = atoi(val);
                if (svc->console < 0) svc->console = 0;
            }
        } else if (in_health) {
            if (strcmp(key, "Restart") == 0) {
                if (strcmp(val, "always") == 0)
                    svc->restart = SVC_RESTART_ALWAYS;
                else if (strcmp(val, "on-failure") == 0)
                    svc->restart = SVC_RESTART_ONFAIL;
                else
                    svc->restart = SVC_RESTART_NO;
            } else if (strcmp(key, "RestartDelay") == 0) {
                svc->restart_delay = atoi(val);
                if (svc->restart_delay < 0)
                    svc->restart_delay = 0;
            } else if (strcmp(key, "After") == 0) {
                svc->after = val;
            } else if (strcmp(key, "Before") == 0) {
                svc->before = val;
            }
        }

        line = next;
    }

    if (svc->name[0] == '\0' || svc->exec_start[0] == '\0') {
        service_clear(svc);
        return -1;
    }

    if (svc->instances == SVC_INSTANCES_AUTO &&
        service_alloc_instances(svc, console_count, instance_pids) < 0) {
        service_clear(svc);
        return -1;
    }

    svc->loaded = 1;
    return 0;
}

static int service_filename_valid(const char *name, unsigned int type)
{
    int nlen;

    if (type != 1)
        return 0;
    nlen = strlen(name);
    if (nlen < 9)
        return 0;
    return strcmp(name + nlen - 9, ".lservice") == 0;
}

static size_t service_config_capacity(const char *path)
{
    char buffer[256];
    int fd;
    ssize_t size;
    size_t total;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 1;
    total = 1;
    for (;;) {
        size = read(fd, buffer, sizeof(buffer));
        if (size <= 0)
            break;
        if ((size_t)size > (size_t)-1 - total) {
            close(fd);
            return 0;
        }
        total += (size_t)size;
    }
    close(fd);
    return total;
}

static int services_measure(size_t *config_size)
{
    int dirfd;
    char name[64];
    char path[256];
    unsigned int type;
    unsigned int idx;
    int count;
    size_t capacity;

    *config_size = 0;
    dirfd = vfs_open(SERVICES_DIR, 0);
    if (dirfd < 0)
        return 0;
    idx = 0;
    count = 0;
    while (vfs_readdir(dirfd, name, &type, idx) >= 0) {
        idx++;
        if (service_filename_valid(name, type)) {
            if (service_make_path(path, sizeof(path), name) != 0)
                continue;
            capacity = service_config_capacity(path);
            if (capacity > (size_t)-1 - *config_size) {
                vfs_close_fd(dirfd);
                return 0;
            }
            *config_size += capacity;
            count++;
        }
    }
    vfs_close_fd(dirfd);
    return count;
}

int services_load(lservice_t **out_svcs)
{
    int dirfd;
    char name[64];
    unsigned int type;
    unsigned int idx;
    int count;
    int capacity;
    int console_count;
    char path[256];
    lservice_t *svcs;
    char *config_storage;
    int *instance_storage;
    size_t records_size;
    size_t config_size;
    size_t config_offset;
    size_t config_capacity;
    size_t config_aligned_size;
    size_t instances_size;
    size_t workspace_size;
    size_t allocation_size;
    size_t mapping_size;
    int *workspace;
    service_storage_t *storage;

    if (!out_svcs)
        return 0;
    *out_svcs = (lservice_t *)0;
    capacity = services_measure(&config_size);
    if (capacity <= 0)
        return 0;
    console_count = get_num_consoles();
    if (size_mul_checked((size_t)capacity, sizeof(lservice_t),
                         &records_size) < 0)
        return 0;
    if (size_add_checked(config_size, sizeof(int) - 1,
                         &config_aligned_size) < 0)
        return 0;
    config_aligned_size &= ~(sizeof(int) - 1);
    if (size_mul_checked((size_t)capacity, (size_t)console_count,
                         &instances_size) < 0 ||
        size_mul_checked(instances_size, sizeof(int), &instances_size) < 0 ||
        size_mul_checked((size_t)capacity, sizeof(int), &workspace_size) < 0)
        return 0;
    allocation_size = sizeof(service_storage_t);
    if (size_add_checked(allocation_size, records_size, &allocation_size) < 0 ||
        size_add_checked(allocation_size, config_aligned_size,
                         &allocation_size) < 0 ||
        size_add_checked(allocation_size, instances_size,
                         &allocation_size) < 0 ||
        size_add_checked(allocation_size, workspace_size,
                         &allocation_size) < 0 ||
        size_add_checked(allocation_size, 4095, &mapping_size) < 0)
        return 0;
    mapping_size &= ~(size_t)4095;
    if (allocation_size <= sizeof(service_bootstrap.data) &&
        !service_bootstrap_used) {
        storage = (service_storage_t *)(void *)service_bootstrap.data;
        memset(storage, 0, allocation_size);
        storage->mapping_size = 0;
        service_bootstrap_used = 1;
    } else {
        storage = mmap((void *)0, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (storage == MAP_FAILED)
            return 0;
        storage->mapping_size = mapping_size;
    }
    svcs = (lservice_t *)(void *)(storage + 1);
    config_storage = (char *)svcs + records_size;
    instance_storage = (int *)(void *)(config_storage + config_aligned_size);
    workspace = (int *)(void *)((char *)instance_storage + instances_size);
    dirfd = vfs_open(SERVICES_DIR, 0);
    if (dirfd < 0)
        goto fail;

    count = 0;
    config_offset = 0;
    idx = 0;
    for (;;) {
        if (vfs_readdir(dirfd, name, &type, idx) < 0)
            break;
        idx++;
        if (!service_filename_valid(name, type))
            continue;

        if (service_make_path(path, sizeof(path), name) != 0)
            continue;
        if (count >= capacity)
            break;
        config_capacity = service_config_capacity(path);
        if (config_offset > config_size) break;
        if (config_capacity > config_size - config_offset)
            config_capacity = config_size - config_offset;
        if (parse_lservice(path, &svcs[count],
                           config_storage + config_offset, config_capacity,
                           instance_storage + (size_t)count *
                           (size_t)console_count, console_count) == 0)
            count++;
        config_offset += config_capacity;
    }
    vfs_close_fd(dirfd);
    if (count > 0) {
        svcs[0].start_order = workspace;
    }
    *out_svcs = svcs;
    if (count == 0) {
        service_storage_release(svcs);
        *out_svcs = (lservice_t *)0;
    }
    return count;

fail:
    service_storage_release(svcs);
    return 0;
}

void services_free(lservice_t *svcs, int count)
{
    (void)count;
    service_storage_release(svcs);
}

static int service_start_internal(lservice_t *svc)
{
    int pid;
    char *argv[4];
    char *envp[2];
    int num_cons;
    int i;
    int started_count;
    int active_console;

    if (!svc->loaded || svc->exec_start[0] == '\0')
        return -1;

    if (svc->instances == SVC_INSTANCES_AUTO) {
        num_cons = svc->instance_count;
        started_count = 0;
        svc->started = 1;
        if (strcmp(svc->exec_start, GETTY_PATH) == 0) {
            active_console = console_getcur();
            if (active_console < 0 || active_console >= num_cons)
                active_console = 0;
            if (service_console_available(active_console) &&
                service_spawn_instance(svc, active_console) > 0)
                started_count++;
        } else {
            for (i = 0; i < num_cons; i++) {
                if (service_spawn_instance(svc, i) > 0)
                    started_count++;
            }
        }
        if (started_count == 0)
            return -1;
        return 0;
    }

    pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        setsid();
        if (svc->console >= 0) {
            if (console_setid(svc->console) < 0) {
                service_console_error(svc->console);
                _exit(126);
            }
            ioctl(0, TIOCSCTTY, 0);
            tcsetpgrp(0, getpid());
        }
        argv[0] = "/bin/sh";
        argv[1] = "-c";
        argv[2] = svc->exec_start;
        argv[3] = (char *)0;
        envp[0] = "TERM=linux";
        envp[1] = (char *)0;
        execve("/bin/sh", argv, envp);
        service_exec_error("/bin/sh");
        _exit(127);
    }

    svc->pid = pid;
    svc->started = 1;

    if (svc->type == SVC_TYPE_ONESHOT) {
        int st;
        waitpid(pid, &st, 0);
        svc->pid = -1;
    }

    return pid;
}

int service_start(lservice_t *svc)
{
    return service_start_internal(svc);
}

void service_stop(lservice_t *svc)
{
    int i;

    if (svc->instances == SVC_INSTANCES_AUTO) {
        for (i = 0; i < svc->instance_count; i++) {
            if (svc->instance_pids[i] > 0) {
                kill(svc->instance_pids[i], SIGTERM);
                svc->instance_pids[i] = -1;
            }
        }
        svc->started = 0;
        return;
    }

    if (svc->pid > 0) {
        if (svc->exec_stop[0] != '\0') {
            int pid;
            char *argv[4];
            char *envp[1];
            int st;

            pid = fork();
            if (pid == 0) {
                argv[0] = "/bin/sh";
                argv[1] = "-c";
                argv[2] = svc->exec_stop;
                argv[3] = (char *)0;
                envp[0] = (char *)0;
                execve("/bin/sh", argv, envp);
                service_exec_error("/bin/sh");
                _exit(127);
            }
            if (pid > 0)
                waitpid(pid, &st, 0);
        }
        kill(svc->pid, SIGTERM);
        svc->pid = -1;
        svc->started = 0;
    }
}

static int svc_find_by_name(lservice_t *svcs, int count, const char *name)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(svcs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int svc_order_contains(int *order, int count, int service)
{
    int i;

    for (i = 0; i < count; i++) {
        if (order[i] == service)
            return 1;
    }
    return 0;
}

static int svc_sort(lservice_t *svcs, int count, int *order)
{
    int out;
    int i;
    int dep;
    int changed;
    int pass;
    int my_pos;
    int dep_pos;
    int j;
    int tmp;

    if (!order)
        return -1;

    for (i = 0; i < count; i++)
        order[i] = -1;
    out = 0;

    for (pass = 0; pass < count; pass++) {
        changed = 0;
        for (i = 0; i < count; i++) {
            if (svc_order_contains(order, out, i))
                continue;

            if (svcs[i].after[0] != '\0') {
                dep = svc_find_by_name(svcs, count, svcs[i].after);
                if (dep >= 0 && !svc_order_contains(order, out, dep))
                    continue;
            }

            order[out++] = i;
            changed = 1;
        }
        if (!changed)
            break;
    }

    for (i = 0; i < count; i++) {
        if (!svc_order_contains(order, out, i))
            order[out++] = i;
    }

    for (i = 0; i < count; i++) {
        if (svcs[i].before[0] != '\0') {
            dep = svc_find_by_name(svcs, count, svcs[i].before);
            if (dep >= 0) {
                my_pos = -1;
                dep_pos = -1;
                for (j = 0; j < count; j++) {
                    if (order[j] == i)
                        my_pos = j;
                    if (order[j] == dep)
                        dep_pos = j;
                }
                if (my_pos >= 0 && dep_pos >= 0 && my_pos > dep_pos) {
                    tmp = order[my_pos];
                    for (j = my_pos; j > dep_pos; j--)
                        order[j] = order[j - 1];
                    order[dep_pos] = tmp;
                }
            }
        }
    }
    return 0;
}

void services_start_all(lservice_t *svcs, int count)
{
    int i;
    int idx;
    const char *msg;
    int *order;

    if (count <= 0)
        return;

    order = svcs[0].start_order;
    if (!order)
        return;

    if (svc_sort(svcs, count, order) < 0) {
        return;
    }

    for (i = 0; i < count; i++) {
        idx = order[i];
        if (!svcs[idx].silent) {
            if (svcs[idx].start_msg[0] != '\0')
                msg = svcs[idx].start_msg;
            else if (svcs[idx].description[0] != '\0')
                msg = svcs[idx].description;
            else
                msg = svcs[idx].name;
            log_info(msg);
        }

        if (svcs[idx].instances == SVC_INSTANCES_AUTO &&
            strcmp(svcs[idx].exec_start, GETTY_PATH) == 0) {
            warm_auth_files();
            if (service_exec_available(svcs[idx].exec_start) < 0) {
                if (!svcs[idx].silent)
                    log_fail(svcs[idx].exec_start);
                continue;
            }
            if (!svcs[idx].silent) {
                if (svcs[idx].ok_msg[0] != '\0')
                    log_ok(svcs[idx].ok_msg);
                else
                    log_ok(svcs[idx].name);
            }
            if (service_start_internal(&svcs[idx]) < 0 &&
                !svcs[idx].silent)
                log_fail(svcs[idx].name);
            continue;
        }

        if (service_start_internal(&svcs[idx]) >= 0) {
            if (!svcs[idx].silent) {
                if (svcs[idx].ok_msg[0] != '\0')
                    log_ok(svcs[idx].ok_msg);
                else
                    log_ok(svcs[idx].name);
            }
        } else {
            if (!svcs[idx].silent)
                log_fail(svcs[idx].name);
        }
    }
}

void services_stop_all(lservice_t *svcs, int count)
{
    int i;

    for (i = count - 1; i >= 0; i--) {
        if (svcs[i].started)
            service_stop(&svcs[i]);
    }
}

int __attribute__((section(".text.hot.user"))) service_check_respawn(
    lservice_t *svcs, int count, int dead_pid)
{
    int i;
    int j;

    for (i = 0; i < count; i++) {
        if (svcs[i].instances == SVC_INSTANCES_AUTO) {
            for (j = 0; j < svcs[i].instance_count; j++) {
                if (svcs[i].instance_pids[j] != dead_pid)
                    continue;
                svcs[i].instance_pids[j] = -1;
                if (svcs[i].restart == SVC_RESTART_ALWAYS ||
                    svcs[i].restart == SVC_RESTART_ONFAIL) {
                    if (strcmp(svcs[i].exec_start, GETTY_PATH) == 0 &&
                        !service_console_available(j))
                        return 1;
                    if (svcs[i].restart_delay > 0)
                        sleep(svcs[i].restart_delay);
                    if (service_spawn_instance(&svcs[i], j) < 0)
                        return 0;
                    return 1;
                }
                return 0;
            }
            continue;
        }
        if (svcs[i].pid == dead_pid) {
            svcs[i].pid = -1;
            if (svcs[i].restart == SVC_RESTART_ALWAYS ||
                svcs[i].restart == SVC_RESTART_ONFAIL) {
                if (svcs[i].restart_delay > 0)
                    sleep(svcs[i].restart_delay);
                service_start(&svcs[i]);
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

void __attribute__((section(".text.hot.user"))) services_ensure_instances(
    lservice_t *svcs, int count)
{
    int i;
    int j;
    int active_console;

    active_console = console_getcur();
    for (i = 0; i < count; i++) {
        if (!svcs[i].started ||
            svcs[i].instances != SVC_INSTANCES_AUTO)
            continue;
        if (svcs[i].restart != SVC_RESTART_ALWAYS &&
            svcs[i].restart != SVC_RESTART_ONFAIL)
            continue;
        if (strcmp(svcs[i].exec_start, GETTY_PATH) == 0) {
            if (active_console < 0 ||
                active_console >= svcs[i].instance_count)
                continue;
            if (svcs[i].instance_pids[active_console] <= 0 &&
                service_console_available(active_console))
                service_spawn_instance(&svcs[i], active_console);
        } else {
            for (j = 0; j < svcs[i].instance_count; j++) {
                if (svcs[i].instance_pids[j] <= 0)
                    service_spawn_instance(&svcs[i], j);
            }
        }
    }
}
