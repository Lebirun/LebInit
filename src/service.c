#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
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
