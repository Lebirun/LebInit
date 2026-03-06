#include <unistd.h>
#include <string.h>
#include "log.h"

#define COLOR_RESET  "\033[0m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED    "\033[31m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_BOLD   "\033[1m"

#define WRITE_LIT(fd, lit) write((fd), (lit), sizeof(lit) - 1)

static void log_write(const char *prefix, const char *color, const char *msg)
{
    char buf[64];
    size_t clen;
    size_t plen;
    size_t blen;
    size_t pos;

    clen = strlen(color);
    plen = strlen(prefix);
    blen = sizeof(COLOR_BOLD) - 1;
    pos = 0;
    memcpy(buf + pos, COLOR_BOLD, blen);
    pos += blen;
    memcpy(buf + pos, color, clen);
    pos += clen;
    memcpy(buf + pos, prefix, plen);
    pos += plen;
    write(1, buf, pos);
    WRITE_LIT(1, COLOR_RESET " ");
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}

void log_info(const char *msg)
{
    log_write("[ INFO ]", COLOR_BLUE, msg);
}

void log_ok(const char *msg)
{
    log_write("[  OK  ]", COLOR_GREEN, msg);
}

void log_warn(const char *msg)
{
    log_write("[ WARN ]", COLOR_YELLOW, msg);
}

void log_fail(const char *msg)
{
    log_write("[ FAIL ]", COLOR_RED, msg);
}
