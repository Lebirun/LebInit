#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <lebirun.h>
#include <dirent.h>
#include "service.h"
#include "log.h"

#define AUTH_WARM_TRIES 5

static void service_exec_error(const char *path)
{
    char buf[192];
    int error;
    int len;

    error = errno;
    len = snprintf(buf, sizeof(buf), "LebInit: exec %s failed with errno %d\n",
                   path, error);
    if (len <= 0)
        return;
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    write(2, buf, len);
}

static void service_console_error(int console_num)
{
    char buf[96];
    int len;

    len = snprintf(buf, sizeof(buf),
                   "LebInit: console_setid(%d) failed\n", console_num);
    if (len <= 0)
        return;
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    write(2, buf, len);
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

int spawn_shell(void)
{
    int pid;
    char *argv[2];
    char *envp[1];

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        if (console_setid(1) < 0)
            console_setid(0);
        ioctl(0, TIOCSCTTY, 0);
        tcsetpgrp(0, getpid());
        argv[0] = SHELL_PATH;
        argv[1] = (char *)0;
        envp[0] = (char *)0;
        execve(SHELL_PATH, argv, envp);
        service_exec_error(SHELL_PATH);
        _exit(127);
    }
    return pid;
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

int spawn_getty(int console_num)
{
    int pid;
    char *argv[3];
    char *envp[2];
    char num_str[4];

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        if (console_setid(console_num) < 0) {
            service_console_error(console_num);
            _exit(126);
        }
        ioctl(0, TIOCSCTTY, 0);
        tcsetpgrp(0, getpid());
        if (console_num >= 10) {
            num_str[0] = '0' + (console_num / 10);
            num_str[1] = '0' + (console_num % 10);
            num_str[2] = '\0';
        } else {
            num_str[0] = '0' + console_num;
            num_str[1] = '\0';
        }
        argv[0] = GETTY_PATH;
        argv[1] = num_str;
        argv[2] = (char *)0;
        envp[0] = "TERM=linux";
        envp[1] = (char *)0;
        execve(GETTY_PATH, argv, envp);
        service_exec_error(GETTY_PATH);
        _exit(127);
    }
    return pid;
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
    }
    return val;
}

static void service_clear(lservice_t *svc)
{
    if (!svc)
        return;
    if (svc->instance_pids)
        free(svc->instance_pids);
    memset(svc, 0, sizeof(*svc));
}

static int service_alloc_instances(lservice_t *svc, int count)
{
    int i;

    svc->instance_pids = (int *)malloc(sizeof(int) * count);
    if (!svc->instance_pids)
        return -1;
    svc->instance_count = count;
    for (i = 0; i < count; i++)
        svc->instance_pids[i] = -1;
    return 0;
}

static void strip_trailing(char *s)
{
    int len;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
           s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int parse_lservice(const char *path, lservice_t *svc)
{
    int fd;
    char buf[1024];
    int n;
    char *line;
    char *next;
    char *eq;
    const char *key;
    const char *val;
    int in_about;
    int in_exec;
    int in_health;
    int consoles;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    memset(svc, 0, sizeof(*svc));
    consoles = get_num_consoles();
    if (service_alloc_instances(svc, consoles) < 0)
        return -1;
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

    line = buf;
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
                strncpy(svc->name, val, sizeof(svc->name) - 1);
            else if (strcmp(key, "Description") == 0)
                strncpy(svc->description, val, sizeof(svc->description) - 1);
            else if (strcmp(key, "StartMsg") == 0)
                strncpy(svc->start_msg, val, sizeof(svc->start_msg) - 1);
            else if (strcmp(key, "OkMsg") == 0)
                strncpy(svc->ok_msg, val, sizeof(svc->ok_msg) - 1);
            else if (strcmp(key, "Silent") == 0)
                svc->silent = (strcmp(val, "yes") == 0 || strcmp(val, "1") == 0);
        } else if (in_exec) {
            if (strcmp(key, "Start") == 0)
                strncpy(svc->exec_start, val, sizeof(svc->exec_start) - 1);
            else if (strcmp(key, "Stop") == 0)
                strncpy(svc->exec_stop, val, sizeof(svc->exec_stop) - 1);
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
                strncpy(svc->after, val, sizeof(svc->after) - 1);
            } else if (strcmp(key, "Before") == 0) {
                strncpy(svc->before, val, sizeof(svc->before) - 1);
            }
        }

        line = next;
    }

    if (svc->name[0] == '\0' || svc->exec_start[0] == '\0') {
        service_clear(svc);
        return -1;
    }

    svc->loaded = 1;
    return 0;
}

int services_load(lservice_t **out_svcs)
{
    int dirfd;
    char name[64];
    unsigned int type;
    unsigned int idx;
    int count;
    int cap;
    char path[256];
    int nlen;
    lservice_t *svcs;
    lservice_t *new_svcs;

    if (!out_svcs)
        return 0;
    *out_svcs = (lservice_t *)0;
    dirfd = vfs_open(SERVICES_DIR, 0);
    if (dirfd < 0)
        return 0;

    count = 0;
    cap = 0;
    svcs = (lservice_t *)0;
    idx = 0;
    for (;;) {
        if (vfs_readdir(dirfd, name, &type, idx) < 0)
            break;
        idx++;
        if (type != 1)
            continue;
        nlen = strlen(name);
        if (nlen < 9 || strcmp(name + nlen - 9, ".lservice") != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", SERVICES_DIR, name);
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            new_svcs = (lservice_t *)realloc(svcs, sizeof(lservice_t) * cap);
            if (!new_svcs)
                break;
            svcs = new_svcs;
        }
        memset(&svcs[count], 0, sizeof(lservice_t));
        if (parse_lservice(path, &svcs[count]) == 0)
            count++;
    }
    vfs_close_fd(dirfd);
    *out_svcs = svcs;
    if (count == 0) {
        free(svcs);
        *out_svcs = (lservice_t *)0;
    }
    return count;
}

void services_free(lservice_t *svcs, int count)
{
    int i;

    if (!svcs)
        return;
    for (i = 0; i < count; i++)
        service_clear(&svcs[i]);
    free(svcs);
}

static int service_start_internal(lservice_t *svc)
{
    int pid;
    char *argv[4];
    char *envp[2];
    int num_cons;
    int i;
    char num_str[4];
    int started_count;

    if (!svc->loaded || svc->exec_start[0] == '\0')
        return -1;

    if (svc->instances == SVC_INSTANCES_AUTO) {
        num_cons = svc->instance_count;
        started_count = 0;
        for (i = 0; i < num_cons; i++) {
            if (i >= 10) {
                num_str[0] = '0' + (i / 10);
                num_str[1] = '0' + (i % 10);
                num_str[2] = '\0';
            } else {
                num_str[0] = '0' + i;
                num_str[1] = '\0';
            }

            pid = fork();
            if (pid < 0) {
                svc->instance_pids[i] = -1;
                continue;
            }
            if (pid == 0) {
                setsid();
                if (console_setid(i) < 0) {
                    service_console_error(i);
                    _exit(126);
                }
                ioctl(0, TIOCSCTTY, 0);
                tcsetpgrp(0, getpid());
                argv[0] = svc->exec_start;
                argv[1] = num_str;
                argv[2] = (char *)0;
                envp[0] = "TERM=linux";
                envp[1] = (char *)0;
                execve(svc->exec_start, argv, envp);
                service_exec_error(svc->exec_start);
                _exit(127);
            }
            svc->instance_pids[i] = pid;
            started_count++;
        }
        if (started_count == 0)
            return -1;
        svc->started = 1;
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

static int svc_sort(lservice_t *svcs, int count, int *order)
{
    int *placed;
    int out;
    int i;
    int dep;
    int changed;
    int pass;

    placed = (int *)malloc(sizeof(int) * count);
    if (!placed)
        return -1;

    for (i = 0; i < count; i++)
        placed[i] = 0;
    out = 0;

    for (pass = 0; pass < count; pass++) {
        changed = 0;
        for (i = 0; i < count; i++) {
            if (placed[i])
                continue;

            if (svcs[i].after[0] != '\0') {
                dep = svc_find_by_name(svcs, count, svcs[i].after);
                if (dep >= 0 && !placed[dep])
                    continue;
            }

            placed[i] = 1;
            order[out++] = i;
            changed = 1;
        }
        if (!changed)
            break;
    }

    for (i = 0; i < count; i++) {
        if (!placed[i])
            order[out++] = i;
    }

    for (i = 0; i < count; i++) {
        if (svcs[i].before[0] != '\0') {
            dep = svc_find_by_name(svcs, count, svcs[i].before);
            if (dep >= 0) {
                int my_pos;
                int dep_pos;
                int j;
                int tmp;

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
    free(placed);
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

    order = (int *)malloc(sizeof(int) * count);
    if (!order)
        return;

    if (svc_sort(svcs, count, order) < 0) {
        free(order);
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
    free(order);
}

void services_stop_all(lservice_t *svcs, int count)
{
    int i;

    for (i = count - 1; i >= 0; i--) {
        if (svcs[i].started)
            service_stop(&svcs[i]);
    }
}

int service_check_respawn(lservice_t *svcs, int count, int dead_pid)
{
    int i;
    int j;
    int pid;
    char num_str[4];
    char *argv[3];
    char *envp[2];

    for (i = 0; i < count; i++) {
        if (svcs[i].instances == SVC_INSTANCES_AUTO) {
            for (j = 0; j < svcs[i].instance_count; j++) {
                if (svcs[i].instance_pids[j] != dead_pid)
                    continue;
                svcs[i].instance_pids[j] = -1;
                if (svcs[i].restart == SVC_RESTART_ALWAYS ||
                    svcs[i].restart == SVC_RESTART_ONFAIL) {
                    if (svcs[i].restart_delay > 0)
                        sleep(svcs[i].restart_delay);
                    if (j >= 10) {
                        num_str[0] = '0' + (j / 10);
                        num_str[1] = '0' + (j % 10);
                        num_str[2] = '\0';
                    } else {
                        num_str[0] = '0' + j;
                        num_str[1] = '\0';
                    }
                    pid = fork();
                    if (pid < 0)
                        return 0;
                    if (pid == 0) {
                        setsid();
                        if (console_setid(j) < 0) {
                            service_console_error(j);
                            _exit(126);
                        }
                        ioctl(0, TIOCSCTTY, 0);
                        tcsetpgrp(0, getpid());
                        argv[0] = svcs[i].exec_start;
                        argv[1] = num_str;
                        argv[2] = (char *)0;
                        envp[0] = "TERM=linux";
                        envp[1] = (char *)0;
                        execve(svcs[i].exec_start, argv, envp);
                        service_exec_error(svcs[i].exec_start);
                        _exit(127);
                    }
                    svcs[i].instance_pids[j] = pid;
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
