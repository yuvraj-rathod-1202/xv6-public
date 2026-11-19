// schedctl: inspect or change the scheduler at runtime.
//   schedctl get                    active scheduler and CFS tunables
//   schedctl rr|fcfs|fifo|cfs       switch scheduler (fifo = fcfs)
//   schedctl tune <latency> <gran>  CFS sched_latency / min_granularity, ticks
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

static char *names[] = { [SCHED_RR] "rr", [SCHED_FCFS] "fcfs", [SCHED_CFS] "cfs" };

static void
usage(void)
{
  printf(2, "Usage: schedctl get | rr | fcfs | fifo | cfs | tune <latency> <min_gran>\n");
  exit();
}

int
main(int argc, char *argv[])
{
  struct schedstat st;
  int scheduler;

  if(argc < 2)
    usage();

  if(strcmp(argv[1], "get") == 0){
    scheduler = getscheduler();
    schedstat(SS_GET, &st);
    printf(1, "%s\n", scheduler >= 0 && scheduler <= SCHED_CFS ?
           names[scheduler] : "unknown");
    printf(1, "cfs: sched_latency %d ticks, min_granularity %d ticks, %d cpus\n",
           st.latency, st.min_granularity, st.ncpu);
    exit();
  }

  if(strcmp(argv[1], "tune") == 0){
    if(argc != 4)
      usage();
    if(schedtune(atoi(argv[2]), atoi(argv[3])) < 0){
      printf(2, "schedctl: need 1 <= min_gran <= latency <= %d\n",
             SCHED_LATENCY_MAX);
      exit();
    }
    schedstat(SS_GET, &st);
    printf(1, "cfs: sched_latency %d ticks, min_granularity %d ticks\n",
           st.latency, st.min_granularity);
    exit();
  }

  if(strcmp(argv[1], "rr") == 0)
    scheduler = SCHED_RR;
  else if(strcmp(argv[1], "fcfs") == 0 || strcmp(argv[1], "fifo") == 0)
    scheduler = SCHED_FCFS;
  else if(strcmp(argv[1], "cfs") == 0)
    scheduler = SCHED_CFS;
  else
    usage();

  if(setscheduler(scheduler) < 0){
    printf(2, "Failed to set scheduler\n");
    exit();
  }
  printf(1, "Scheduler set to %s\n", argv[1]);
  exit();
}
