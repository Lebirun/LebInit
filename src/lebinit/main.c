#include <unistd.h>
#include <signal.h>
#include <string.h>

#define WRITE_LIT(fd, lit) write((fd), (lit), sizeof(lit) - 1)

static void usage(void)
{
	WRITE_LIT(2, "Usage: lebinit <command>\n");
	WRITE_LIT(2, "Commands:\n");
	WRITE_LIT(2, "  shutdown     Power off the system\n");
	WRITE_LIT(2, "  reboot       Reboot the system\n");
	WRITE_LIT(2, "  soft-reboot  Restart userspace only\n");
}

int main(int argc, char **argv)
{
	const char *cmd;
	int sig;

	if (argc < 2) {
		usage();
		return 1;
	}

	cmd = argv[1];

	if (strcmp(cmd, "shutdown") == 0) {
		sig = SIGUSR1;
	} else if (strcmp(cmd, "reboot") == 0) {
		sig = SIGUSR2;
	} else if (strcmp(cmd, "soft-reboot") == 0) {
		sig = SIGTERM;
	} else {
		WRITE_LIT(2, "lebinit: unknown command '");
		write(2, cmd, strlen(cmd));
		WRITE_LIT(2, "'\n");
		usage();
		return 1;
	}

	if (kill(1, sig) < 0) {
		WRITE_LIT(2, "lebinit: failed to signal init\n");
		return 1;
	}

	return 0;
}
