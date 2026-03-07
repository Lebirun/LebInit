#include <unistd.h>
#include <string.h>
#include "log.h"

#define COLOR_RESET  "\033[0m"

#define WRITE_LIT(fd, lit) write((fd), (lit), sizeof(lit) - 1)

void log_info(const char *msg)
{
    WRITE_LIT(1, "\033[1;34m");
    WRITE_LIT(1, "[ INFO ]");
    WRITE_LIT(1, COLOR_RESET " ");
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}

void log_ok(const char *msg)
{
    WRITE_LIT(1, "\033[1;32m");
    WRITE_LIT(1, "[  OK  ]");
    WRITE_LIT(1, COLOR_RESET " ");
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}

void log_warn(const char *msg)
{
    WRITE_LIT(1, "\033[1;33m");
    WRITE_LIT(1, "[ WARN ]");
    WRITE_LIT(1, COLOR_RESET " ");
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}

void log_fail(const char *msg)
{
    WRITE_LIT(1, "\033[1;31m");
    WRITE_LIT(1, "[ FAIL ]");
    WRITE_LIT(1, COLOR_RESET " ");
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}
