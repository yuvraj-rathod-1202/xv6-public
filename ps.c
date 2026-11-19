// ps: list processes with their scheduling state.
//   RTIME/WTIME/STIME are ticks spent running / runnable / sleeping.
//   VRUNTIME is the CFS virtual runtime in units of 1024 weighted cycles.
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

#define MAXPROC 64

static struct pinfo procs[MAXPROC];   // too big for the 4KB user stack

static char *states[] = {
  [PSTATE_UNUSED]   "unused",
  [PSTATE_EMBRYO]   "embryo",
  [PSTATE_SLEEPING] "sleep",
  [PSTATE_RUNNABLE] "runble",
  [PSTATE_RUNNING]  "run",
  [PSTATE_ZOMBIE]   "zombie",
};

static char *scheds[] = {
  [SCHED_RR]   "rr",
  [SCHED_FCFS] "fifo",
  [SCHED_CFS]  "cfs",
};

static char*
utoa(uint v, char *buf)
{
  char tmp[12];
  int i = 0, j = 0;

  do {
    tmp[i++] = '0' + v % 10;
    v /= 10;
  } while(v);
  while(i)
    buf[j++] = tmp[--i];
  buf[j] = 0;
  return buf;
}

static char*
itoa(int v, char *buf)
{
  if(v < 0){
    buf[0] = '-';
    utoa(-v, buf + 1);
    return buf;
  }
  return utoa(v, buf);
}

// Print s left-aligned in a column of width w.
static void
col(char *s, int w)
{
  int n = strlen(s);

  printf(1, "%s", s);
  while(n++ < w)
    printf(1, " ");
}

int
main(void)
{
  char buf[16];
  int n, i, s;

  s = getscheduler();
  printf(1, "scheduler: %s\n", (s >= 0 && s <= SCHED_CFS) ? scheds[s] : "?");

  n = getpinfo(procs, MAXPROC);
  if(n < 0){
    printf(2, "ps: getpinfo failed\n");
    exit();
  }

  col("PID", 5); col("PPID", 5); col("STATE", 7); col("NICE", 5);
  col("VRUNTIME", 10); col("RTIME", 6); col("WTIME", 6); col("STIME", 6);
  printf(1, "NAME\n");
  for(i = 0; i < n; i++){
    col(itoa(procs[i].pid, buf), 5);
    col(itoa(procs[i].ppid, buf), 5);
    col(procs[i].state >= 0 && procs[i].state <= PSTATE_ZOMBIE ?
        states[procs[i].state] : "?", 7);
    col(itoa(procs[i].nice, buf), 5);
    col(utoa((uint)(procs[i].vruntime >> 10), buf), 10);
    col(utoa(procs[i].rtime, buf), 6);
    col(utoa(procs[i].wtime, buf), 6);
    col(utoa(procs[i].stime, buf), 6);
    printf(1, "%s\n", procs[i].name);
  }
  exit();
}
