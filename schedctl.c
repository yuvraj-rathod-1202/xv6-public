#include "types.h"
#include "stat.h"
#include "user.h"

#define SCHED_RR 0
#define SCHED_FCFS 1
#define SCHED_CFS 2

int
main(int argc, char *argv[])
{
    int scheduler;

    if(argc != 2) {
        printf(2, "Usage: schedctl get|rr|fcfs|cfs\n");
        exit();
    }

    if(strcmp(argv[1], "get") == 0) {
        scheduler = getscheduler();
        if(scheduler == SCHED_RR)
            printf(1, "rr\n");
        else if(scheduler == SCHED_FCFS)
            printf(1, "fcfs\n");
        else if(scheduler == SCHED_CFS)
            printf(1, "cfs\n");
        else
            printf(1, "unknown (%d)\n", scheduler);
        exit();
    } else if(strcmp(argv[1], "rr") == 0) {
        scheduler = SCHED_RR;
    } else if (strcmp(argv[1], "fcfs") == 0) {
        scheduler = SCHED_FCFS;
    } else if (strcmp(argv[1], "cfs") == 0) {
        scheduler = SCHED_CFS;
    } else {
        printf(2, "Invalid scheduler: %s\n", argv[1]);
        exit();
    }

    if(setscheduler(scheduler) < 0) {
        printf(2, "Failed to set scheduler\n");
        exit();
    }

    printf(1, "Scheduler set to %s\n", argv[1]);

    exit();
}