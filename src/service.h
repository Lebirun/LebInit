#ifndef LEBINIT_SERVICE_H
#define LEBINIT_SERVICE_H

#define SHELL_PATH "/bin/lsh"
#define GETTY_PATH "/sbin/getty"
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
    char *name;
    char *description;
    char *exec_start;
    char *exec_stop;
    char *after;
    char *before;
    char *start_msg;
    char *ok_msg;
    int type;
    int restart;
    int restart_delay;
    int instances;
    int *instance_pids;
    int *start_order;
    int instance_count;
    int pid;
    int loaded;
    int started;
    int failed;
    int silent;
    int console;
} lservice_t;

int get_num_consoles(void);

int services_load(lservice_t **out_svcs);
void services_free(lservice_t *svcs, int count);
int service_start(lservice_t *svc);
void service_stop(lservice_t *svc);
void services_start_all(lservice_t *svcs, int count);
void services_stop_all(lservice_t *svcs, int count);
int service_check_respawn(lservice_t *svcs, int count, int dead_pid);
void services_ensure_instances(lservice_t *svcs, int count);

#endif
