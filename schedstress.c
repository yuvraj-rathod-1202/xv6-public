// schedstress: randomised stress test of the schedulers.
//
//   schedstress [ticks] [seed]        (default: 400 ticks, seed from uptime)
//
// For `ticks` timer ticks, two processes hammer the scheduler:
//   manager  - keeps up to MAXW workers alive, reaping each one with wait()
//              and replacing it.
//   chaos    - (this process) every few ticks does one of: switch scheduler
//              (RR/FIFO/CFS at random), change CFS tunables, renice a random
//              process, or kill a random worker.
// Each worker runs a random program of short CPU bursts, sleeps, nice
// changes, and forks of short-lived grandchildren that it waits for. Bursts
// are short, so even under the non-preemptive FIFO scheduler everything
// keeps moving.
//
// At the end it checks:
//   - every worker that was forked was reaped exactly once (none lost)
//   - no process this test created is still alive (no leaks / zombies)
//   - the CFS linear-scan cross-check never disagreed with the tree
//   - the kernel's run queue is consistent with the process table
// On a SCHED_DEBUG kernel the run-queue invariants are additionally checked
// inside the kernel on every scheduler operation (a violation panics).
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"
#include "param.h"

#define MAXW 24

static uint rng;
static uint
rnd(void)
{
  rng = rng * 1103515245 + 12345;
  return (rng >> 8) & 0xffffff;
}

static volatile uint sink;
static void
burn(uint n)
{
  uint i;
  for(i = 0; i < n; i++)
    sink += i;
}

static struct pinfo procs[NPROC];

static void
worker(void)
{
  int steps = 3 + rnd() % 10, i, pid;

  for(i = 0; i < steps; i++){
    switch(rnd() % 5){
    case 0:   // short CPU burst
    case 1:
      burn(20000 + rnd() % 400000);
      break;
    case 2:   // block for a while
      sleep(rnd() % 4);
      break;
    case 3:   // change own nice value
      setnice((int)(rnd() % 40) - 20);
      break;
    case 4:   // short-lived child
      pid = fork();
      if(pid == 0){
        burn(rnd() % 200000);
        exit();
      }
      if(pid > 0)
        wait();
      break;
    }
  }
  exit();
}

// Spawns and reaps workers until the deadline, then reports
// "spawned reaped forkfail" to fd.
static void
manager(uint deadline, int fd)
{
  int live = 0, stats[3] = { 0, 0, 0 }, pid;

  while(uptime() < deadline){
    while(live < MAXW && uptime() < deadline){
      rnd();
      pid = fork();
      if(pid == 0)
        worker();
      if(pid < 0){
        stats[2]++;
        break;
      }
      live++;
      stats[0]++;
    }
    if(wait() > 0){
      live--;
      stats[1]++;
    }
  }
  while(wait() > 0)
    stats[1]++;
  write(fd, stats, sizeof(stats));
  exit();
}

int
main(int argc, char *argv[])
{
  uint duration = argc > 1 ? atoi(argv[1]) : 400;
  uint deadline, switches = 0, tunes = 0, renices = 0, kills = 0;
  int fd[2], mgr, n, i, leftover, failures = 0, orig = getscheduler();
  int counts[3] = { 0, 0, 0 };
  struct schedstat st;
  char *names[] = { "rr", "fifo", "cfs" };

  rng = argc > 2 ? atoi(argv[2]) : uptime() * 2654435761u;
  printf(1, "schedstress: %d ticks, seed %d\n", duration, rng);

  schedstat(SS_SCAN_ON, &st);
  schedstat(SS_RESET, &st);
  pipe(fd);
  deadline = uptime() + duration;
  mgr = fork();
  if(mgr == 0){
    close(fd[0]);
    manager(deadline, fd[1]);
  }
  close(fd[1]);

  while(uptime() < deadline){
    sleep(1 + rnd() % 6);
    switch(rnd() % 6){
    case 0:
    case 1:
      setscheduler(rnd() % 3);
      switches++;
      break;
    case 2:
      schedtune(1 + rnd() % 12, 1);
      tunes++;
      break;
    case 3:
    case 4:
      n = getpinfo(procs, NPROC);
      if(n > 0 && renice(procs[rnd() % n].pid, (int)(rnd() % 40) - 20) == 0)
        renices++;
      break;
    case 5:
      // kill a random worker (a child of the manager)
      n = getpinfo(procs, NPROC);
      for(i = 0; i < n; i++){
        int j = (i + rnd()) % n;
        if(procs[j].ppid == mgr && procs[j].state != PSTATE_ZOMBIE){
          if(kill(procs[j].pid) == 0)
            kills++;
          break;
        }
      }
      break;
    }
  }

  setscheduler(SCHED_RR);   // everything left finishes promptly
  schedtune(SCHED_LATENCY_DEFAULT, SCHED_MIN_GRAN_DEFAULT);
  if(read(fd[0], counts, sizeof(counts)) != sizeof(counts)){
    printf(1, "  [FAIL] manager did not report\n");
    failures++;
  }
  wait();
  close(fd[0]);

  printf(1, "  workers spawned %d, reaped %d (fork failures %d)\n",
         counts[0], counts[1], counts[2]);
  printf(1, "  scheduler switches %d, tunable changes %d, renices %d, kills %d\n",
         switches, tunes, renices, kills);
  if(counts[0] == 0 || counts[0] != counts[1]){
    printf(1, "  [FAIL] spawned and reaped counts differ\n");
    failures++;
  }

  // Nothing we created may still exist.
  n = getpinfo(procs, NPROC);
  leftover = 0;
  for(i = 0; i < n; i++)
    if(procs[i].ppid == mgr || procs[i].pid == mgr ||
       (procs[i].ppid == getpid()))
      leftover++;
  if(leftover){
    printf(1, "  [FAIL] %d processes leaked\n", leftover);
    failures++;
  }

  schedstat(SS_GET, &st);
  printf(1, "  cfs picks cross-checked against a linear scan: %d, mismatches %d\n",
         st.scans, st.scan_mismatches);
  if(st.scan_mismatches){
    printf(1, "  [FAIL] CFS tree disagreed with linear scan\n");
    failures++;
  }
  // Only this process is runnable/running now; the queue must not hold
  // stale entries.
  if(st.nr_queued > n){
    printf(1, "  [FAIL] run queue holds %d entries, only %d processes exist\n",
           st.nr_queued, n);
    failures++;
  }
  schedstat(SS_SCAN_OFF, &st);
  setscheduler(orig);

  if(failures == 0)
    printf(1, "schedstress: PASS (ended under %s)\n", names[orig]);
  else
    printf(1, "schedstress: FAIL (%d)\n", failures);
  exit();
}
