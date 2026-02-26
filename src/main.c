#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <lebirun.h>
#include "log.h"
#include "service.h"

#define COLOR_RESET  "\033[0m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_WHITE  "\033[37m"

#define WRITE_LIT(fd, lit) write((fd), (lit), sizeof(lit) - 1)

#define POWER_CMD_SHUTDOWN  0x4321
#define POWER_CMD_REBOOT    0x1234

static volatile sig_atomic_t g_shutdown;
static volatile sig_atomic_t g_reboot;
static volatile sig_atomic_t g_soft_reboot;

static void handle_sigusr1(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

static void handle_sigusr2(int sig)
{
    (void)sig;
    g_reboot = 1;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    g_soft_reboot = 1;
}

static void setup_signals(void)
{
    struct sigaction sa;

    sa.sa_handler = handle_sigusr1;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, (void *)0);

    sa.sa_handler = handle_sigusr2;
    sigaction(SIGUSR2, &sa, (void *)0);

    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, (void *)0);
}

static void kill_all_children(void)
{
    int status;

    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
}

static void do_shutdown(void)
{
    log_info("System is shutting down...");
    kill_all_children();
    log_info("Powering off");
    leb_syscall1(LEB_SYSCALL_REBOOT, POWER_CMD_SHUTDOWN);
    for (;;)
        sleep(1);
}

static void do_reboot(void)
{
    log_info("System is rebooting...");
    kill_all_children();
    log_info("Rebooting now");
    leb_syscall1(LEB_SYSCALL_REBOOT, POWER_CMD_REBOOT);
    for (;;)
        sleep(1);
}

static void print_banner(void)
{
    write(1, "\n", 1);
    WRITE_LIT(1, COLOR_BOLD COLOR_CYAN);
    WRITE_LIT(1, "  _          _     _____       _ _   \n");
    WRITE_LIT(1, " | |        | |   |_   _|     (_) |  \n");
    WRITE_LIT(1, " | |     ___| |__   | |  _ __  _| |_ \n");
    WRITE_LIT(1, " | |    / _ \\ '_ \\  | | | '_ \\| | __|\n");
    WRITE_LIT(1, " | |___|  __/ |_) |_| |_| | | | | |_ \n");
    WRITE_LIT(1, " |______\\___|_.__/|_____|_| |_|_|\\__|\n");
    WRITE_LIT(1, COLOR_RESET);
    write(1, "\n", 1);
    WRITE_LIT(1, COLOR_WHITE " Lebirun Init System" COLOR_RESET "\n\n");
}

int main(void)
{
    int shell_pid;
    int status;
    int wpid;
    char *argv[2];
    char *envp[1];

    (void)status;

    if (getpid() != 1) {
        WRITE_LIT(2, "LebInit: init is already running (PID 1)\n");
        return 1;
    }

    print_banner();
    setup_signals();

    log_info("LebInit starting as PID 1");

restart:
    g_shutdown = 0;
    g_reboot = 0;
    g_soft_reboot = 0;

    log_info("Launching shell: " SHELL_PATH);
    shell_pid = spawn_shell();
    if (shell_pid < 0) {
        log_fail("Could not launch shell");
        for (;;)
            sleep(1);
    }
    log_ok("Shell launched");

    for (;;) {
        if (g_shutdown) {
            do_shutdown();
        }
        if (g_reboot) {
            do_reboot();
        }
        if (g_soft_reboot) {
            log_info("Soft-reboot: restarting userspace...");
            kill_all_children();
            log_ok("Userspace stopped, restarting init");
            argv[0] = "/bin/init";
            argv[1] = (char *)0;
            envp[0] = (char *)0;
            execve("/bin/init", argv, envp);
            log_warn("execve failed, falling back to shell respawn");
            goto restart;
        }

        wpid = waitpid(-1, &status, WNOHANG);
        if (wpid == shell_pid) {
            log_warn("Shell exited, respawning...");
            sleep(1);
            shell_pid = spawn_shell();
            if (shell_pid < 0) {
                log_fail("Could not respawn shell");
            } else {
                log_ok("Shell respawned");
            }
        } else if (wpid <= 0) {
            sleep(1);
        }
    }

    return 0;
}
