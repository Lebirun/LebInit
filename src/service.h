#ifndef LEBINIT_SERVICE_H
#define LEBINIT_SERVICE_H

#define SHELL_PATH "/bin/lsh"
#define GETTY_PATH "/sbin/getty"
#define MAX_CONSOLES 12
#define MAX_SERVICES 16
#define SERVICES_DIR "/etc/lebinit/services"

#define SVC_TYPE_ONESHOT  0
#define SVC_TYPE_DAEMON   1
#define SVC_TYPE_FORKING  2

#define SVC_RESTART_NO        0
#define SVC_RESTART_ALWAYS    1
#define SVC_RESTART_ONFAIL    2

#define SVC_INSTANCES_NONE  0
#define SVC_INSTANCES_AUTO  1

typedef struct {
    char name[32];
    char description[64];
    char exec_start[128];
    char exec_stop[64];
    char after[64];
    char start_msg[64];
    char ok_msg[64];
    int type;
    int restart;
    int restart_delay;
    int instances;
    int instance_pids[MAX_CONSOLES];
    int pid;
    int loaded;
    int started;
    int failed;
} lservice_t;

int spawn_shell(void);
int spawn_getty(int console_num);
int get_num_consoles(void);

int services_load(lservice_t *svcs, int max);
int service_start(lservice_t *svc);
void service_stop(lservice_t *svc);
void services_start_all(lservice_t *svcs, int count);
void services_stop_all(lservice_t *svcs, int count);
int service_check_respawn(lservice_t *svcs, int count, int dead_pid);

#endif
