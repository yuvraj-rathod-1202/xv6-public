// schedtest: correctness tests for the schedulers and runtime switching.
//
//   1. switch  - switch RR/FIFO/CFS repeatedly while CPU-bound processes run;
//                every switch must take effect and every process must finish.
//   2. cfsfair - a process that slept through a CFS period and woke under RR
//                must not monopolise the CPU when CFS is switched back on
//                (stale vruntime). Both processes should get similar shares.
//   3. fifo    - under FIFO, processes are first dispatched in the order they
//                became runnable, and are never preempted once running.
//   4. nice    - renice/getpinfo/setnice validation and nice inheritance.
//
// Test 2 is only discriminating with one CPU: make qemu-nox CPUS=1
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"

static volatile uint sink;
static uint loops_per_tick;
static struct pinfo procs[64];
static int failures;

static void
burn(uint n)
{
  uint i;

  for(i = 0; i < n; i++)
    sink += i;
}

// How many burn() iterations fit in one tick (see schedbench.c).
static void
calibrate(void)
{
  uint n = 1000000, t0, t;

  for(;;){
    t0 = uptime();
    while(uptime() == t0)
      ;
    t0 = uptime();
    burn(n);
    t = uptime() - t0;
    if(t >= 10 || n >= 0x40000000){
      loops_per_tick = t ? n / t : n;
      return;
    }
    n *= 2;
  }
}

static void
burn_ticks(int t)
{
  while(t-- > 0)
    burn(loops_per_tick);
}

static void
check(int ok, char *what)
{
  printf(1, "  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if(!ok)
    failures++;
}

static struct pinfo*
find(int pid)
{
  int i, n = getpinfo(procs, 64);

  for(i = 0; i < n; i++)
    if(procs[i].pid == pid)
      return &procs[i];
  return 0;
}

static void
test_switch(void)
{
  int seq[] = { SCHED_CFS, SCHED_FCFS, SCHED_RR, SCHED_CFS,
                SCHED_RR, SCHED_FCFS, SCHED_CFS, SCHED_RR };
  int nseq = sizeof(seq) / sizeof(seq[0]);
  int i, pid, reaped = 0, switched = 1;
  struct waitinfo wi;

  printf(1, "test 1: switching schedulers under load\n");
  setscheduler(SCHED_RR);
  for(i = 0; i < 4; i++){
    if(fork() == 0){
      burn_ticks(40);
      exit();
    }
  }
  for(i = 0; i < nseq; i++){
    sleep(8);
    if(setscheduler(seq[i]) < 0 || getscheduler() != seq[i])
      switched = 0;
  }
  while((pid = waitx(&wi)) > 0){
    reaped++;
    if(wi.rtime == 0)
      printf(1, "  pid %d never ran?\n", pid);
  }
  check(switched, "every setscheduler took effect");
  check(reaped == 4, "all 4 processes ran to completion");
  check(setscheduler(-1) < 0 && setscheduler(3) < 0,
        "invalid scheduler ids rejected");
}

static void
test_cfsfair(void)
{
  int fd[2], a, b, ok;
  uint stop, da, db, a0, b0;
  struct pinfo *p;
  char c = 'x';

  printf(1, "test 2: fairness after switching back into CFS\n");
  setscheduler(SCHED_CFS);
  pipe(fd);
  stop = uptime() + 240;

  // B blocks right away, so it misses the whole first CFS period.
  b = fork();
  if(b == 0){
    close(fd[1]);
    read(fd[0], &c, 1);
    while(uptime() < stop)
      burn(1000);
    exit();
  }
  // A runs alone under CFS and builds up a large vruntime.
  a = fork();
  if(a == 0){
    while(uptime() < stop)
      burn(1000);
    exit();
  }
  close(fd[0]);
  sleep(100);

  // Wake B while RR is active, then switch back to CFS.
  setscheduler(SCHED_RR);
  write(fd[1], &c, 1);
  close(fd[1]);
  sleep(5);
  setscheduler(SCHED_CFS);

  p = find(a); a0 = p ? p->rtime : 0;
  p = find(b); b0 = p ? p->rtime : 0;
  sleep(100);
  p = find(a); da = (p ? p->rtime : 0) - a0;
  p = find(b); db = (p ? p->rtime : 0) - b0;
  wait();
  wait();

  printf(1, "  over 100 ticks of CFS: A ran %d ticks, B ran %d ticks\n", da, db);
  ok = da + db > 0 && da * 100 >= (da + db) * 30 && db * 100 >= (da + db) * 30;
  check(ok, "each process got at least 30% (no stale-vruntime monopoly)");
}

static void
test_fifo(void)
{
  int pids[4], i, k, pid, ordered = 1, nopreempt = 1;
  struct waitinfo w[4], wi;

  printf(1, "test 3: FIFO order and non-preemption\n");
  setscheduler(SCHED_FCFS);
  for(i = 0; i < 4; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      burn_ticks(10);
      exit();
    }
  }
  for(k = 0; k < 4; k++){
    pid = waitx(&wi);
    for(i = 0; i < 4; i++)
      if(pids[i] == pid)
        w[i] = wi;
  }
  for(i = 0; i < 4; i++){
    int before = w[i].first_run - w[i].ctime;
    printf(1, "  child %d: created %d, first run %d, exit %d, waited %d\n",
           i, w[i].ctime, w[i].first_run, w[i].etime, w[i].wtime);
    if(i > 0 && w[i].first_run < w[i-1].first_run)
      ordered = 0;
    // Non-preemptive and never sleeps: all of its waiting happens before
    // its first run (allow 1 tick of sampling slack).
    if(w[i].wtime > before + 1)
      nopreempt = 0;
  }
  check(ordered, "dispatched in the order they became runnable");
  check(nopreempt, "no process was preempted once running");
  setscheduler(SCHED_RR);
}

static void
test_nice(void)
{
  int pid, up[2], down[2], v = 99;
  struct pinfo *p;
  char c;

  printf(1, "test 4: nice values\n");
  check(setnice(NICE_MAX + 1) < 0 && setnice(NICE_MIN - 1) < 0,
        "setnice rejects out-of-range values");
  setnice(3);
  pipe(up);     // child -> parent
  pipe(down);   // parent -> child
  pid = fork();
  if(pid == 0){
    v = getnice();
    write(up[1], &v, sizeof(v));
    read(down[0], &c, 1);   // wait until the parent has checked us
    exit();
  }
  read(up[0], &v, sizeof(v));
  check(v == 3, "child inherits parent's nice value");
  check(renice(pid, -7) == 0, "renice another process");
  p = find(pid);
  check(p && p->nice == -7, "getpinfo reports the new nice value");
  check(renice(pid, 20) < 0, "renice rejects out-of-range value");
  check(renice(99999, 0) < 0, "renice rejects unknown pid");
  write(down[1], "x", 1);
  close(up[0]); close(up[1]);
  close(down[0]); close(down[1]);
  wait();
  setnice(0);
}

int
main(void)
{
  int orig = getscheduler();

  calibrate();
  test_switch();
  test_cfsfair();
  test_fifo();
  test_nice();
  setscheduler(orig);

  if(failures == 0)
    printf(1, "schedtest: ALL TESTS PASSED\n");
  else
    printf(1, "schedtest: %d CHECK(S) FAILED\n", failures);
  exit();
}
