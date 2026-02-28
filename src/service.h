#ifndef LEBINIT_SERVICE_H
#define LEBINIT_SERVICE_H

#define SHELL_PATH "/bin/lsh"
#define GETTY_PATH "/sbin/getty"
#define MAX_CONSOLES 12

int spawn_shell(void);
int spawn_getty(int console_num);
int get_num_consoles(void);

#endif
