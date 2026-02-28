#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <lebirun.h>
#include "service.h"
#include "log.h"

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
        _exit(127);
    }
    return pid;
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
        if (console_setid(console_num) < 0)
            _exit(126);
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

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return 4;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 4;
    buf[n] = '\0';

    p = strstr(buf, "consoles=");
    if (!p)
        return 4;
    p += 9;
    val = atoi(p);
    if (val < 1)
        val = 1;
    if (val > MAX_CONSOLES)
        val = MAX_CONSOLES;
    return val;
}
