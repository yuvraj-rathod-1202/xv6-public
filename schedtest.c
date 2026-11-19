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
//   5. slices  - CFS time slices follow sched_latency and min_granularity:
//                a longer latency means proportionally fewer preemptions,
//                and min_granularity puts a floor under the slice.
//   6. trace   - the scheduling trace is well formed: on every CPU, runs and
//                stops alternate, times never go backwards, and exits and
//                sleeps are recorded.
//   7. scan    - every CFS pick equals what a linear scan of the process
//                table would choose (the tree never picks the wrong process).
//
// Test 2 is only discriminating with one CPU: make qemu-nox CPUS=1
#include "types.h"
#include "stat.h"
#include "user.h"
#include "sched.h"
#include "param.h"

static volatile uint sink;
static uint loops_per_tick;
static struct pinfo procs[NPROC];
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
  int i, n = getpinfo(procs, NPROC);

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

// The CFS slice (ticks) for n equal-weight runnable processes, by the same
// formula as sched_cfs.c.
static uint
expected_slice(uint latency, uint gran, uint n)
{
  uint period = latency, slice;

  if(n * gran > period)
    period = n * gran;
  slice = (period + n / 2) / n;     // period * w / (n * w), rounded
  return slice < gran ? gran : slice;
}

// Run nproc CPU-bound processes under CFS with the given tunables for
// `window` ticks; return the number of timer preemptions. The parent blocks
// in wait() (not sleep(), which xv6 wakes on every tick and which would
// make the parent itself a runnable process every tick).
static uint
preemptions_with(uint latency, uint gran, int nproc, int window)
{
  struct schedstat st;
  int i;
  uint stop;

  schedtune(latency, gran);
  schedstat(SS_RESET, &st);
  stop = uptime() + window;
  for(i = 0; i < nproc; i++){
    if(fork() == 0){
      while(uptime() < stop)
        burn(1000);
      exit();
    }
  }
  for(i = 0; i < nproc; i++)
    wait();
  schedstat(SS_GET, &st);
  return st.preemptions;
}

// Measured preemptions must be within 35% (plus 2) of what the slice
// formula predicts: about window * ncpu / slice.
static void
check_slices(uint latency, uint gran, int n, int ncpu, int window)
{
  uint slice = expected_slice(latency, gran, n);
  uint want = window * ncpu / slice;
  uint got = preemptions_with(latency, gran, n, window);
  char msg[96];
  int ok = got * 100 <= want * 135 + 200 && got * 100 + 200 >= want * 65;

  printf(1, "  latency %d, min_gran %d, %d procs: slice %d ticks -> expect ~%d"
         " preemptions, got %d\n", latency, gran, n, slice, want, got);
  strcpy(msg, "preemption count matches the CFS slice formula");
  check(ok, msg);
}

static void
test_slices(void)
{
  struct schedstat st;
  int n, window = 60;

  printf(1, "test 5: CFS time slices (sched_latency, min_granularity)\n");
  schedstat(SS_GET, &st);
  n = 2 * st.ncpu;          // two runnable processes per CPU
  if(n > 16)
    n = 16;
  setscheduler(SCHED_CFS);

  check_slices(24, 1, n, st.ncpu, window);   // long latency: long slices
  check_slices(1, 1, n, st.ncpu, window);    // 1-tick slices
  check_slices(4, 1, n, st.ncpu, window);    // slice = 4/n, floor 1
  check_slices(4, 4, n, st.ncpu, window);    // min_granularity stretches period

  check(schedtune(5, 6) < 0 && schedtune(SCHED_LATENCY_MAX + 1, 1) < 0,
        "schedtune rejects min_granularity > latency and huge latency");
  schedtune(SCHED_LATENCY_DEFAULT, SCHED_MIN_GRAN_DEFAULT);
  setscheduler(SCHED_RR);
}

static void
test_trace(void)
{
  struct trace_event *ev;
  int running[8], seen[8], i, n, cpu, ok = 1, exits = 0, sleeps = 0;
  int kids[2];
  uint last[8];

  printf(1, "test 6: scheduling trace\n");
  setscheduler(SCHED_CFS);
  tracectl(TRACE_START);
  for(i = 0; i < 2; i++){
    kids[i] = fork();
    if(kids[i] == 0){
      burn_ticks(3);
      sleep(2);
      burn_ticks(2);
      exit();
    }
  }
  wait();
  wait();
  n = tracectl(TRACE_STOP);
  check(tracectl(TRACE_DROPPED) == 0, "no events dropped");
  ev = malloc(n * sizeof(*ev));
  check(ev != 0 && traceread(ev, 0, n) == n, "read back all events");
  if(ev == 0)
    return;

  for(i = 0; i < 8; i++){
    running[i] = -1;
    seen[i] = 0;
    last[i] = 0;
  }
  for(i = 0; i < n && ok; i++){
    cpu = ev[i].cpu;
    if(ev[i].type == TR_SCHED)
      continue;
    if(cpu >= 8 || ev[i].t < last[cpu]){
      printf(1, "  event %d: bad cpu or time went backwards\n", i);
      ok = 0;
    }
    last[cpu] = ev[i].t;
    if(ev[i].type == TR_RUN){
      if(running[cpu] != -1){
        printf(1, "  event %d: cpu %d runs pid %d while pid %d still running\n",
               i, cpu, ev[i].pid, running[cpu]);
        ok = 0;
      }
      running[cpu] = ev[i].pid;
    } else if(ev[i].type == TR_STOP){
      // The first event on a CPU may stop a process that was already
      // running when the trace started.
      if(running[cpu] != ev[i].pid && seen[cpu]){
        printf(1, "  event %d: cpu %d stops pid %d but pid %d was running\n",
               i, cpu, ev[i].pid, running[cpu]);
        ok = 0;
      }
      running[cpu] = -1;
      if(ev[i].pid == kids[0] || ev[i].pid == kids[1]){
        exits += ev[i].arg == TRS_EXIT;
        sleeps += ev[i].arg == TRS_SLEEP;
      }
    }
    seen[cpu] = 1;
  }
  printf(1, "  %d events; children: %d exits, %d sleeps recorded\n",
         n, exits, sleeps);
  check(ok, "runs and stops alternate on every CPU; time is monotonic");
  check(exits == 2 && sleeps >= 2, "exits and sleeps are recorded");
  free(ev);
  setscheduler(SCHED_RR);
}

static void
test_scan(void)
{
  struct schedstat st;
  int i, pids[6];

  printf(1, "test 7: CFS tree pick == linear-scan pick\n");
  setscheduler(SCHED_CFS);
  schedstat(SS_SCAN_ON, &st);
  schedstat(SS_RESET, &st);
  for(i = 0; i < 6; i++){
    pids[i] = fork();
    if(pids[i] == 0){
      setnice(i * 4 - 10);
      burn_ticks(4);
      sleep(1);
      burn_ticks(4);
      exit();
    }
  }
  renice(pids[0], 15);
  for(i = 0; i < 6; i++)
    wait();
  schedstat(SS_GET, &st);
  schedstat(SS_SCAN_OFF, &st);
  printf(1, "  %d picks cross-checked, %d mismatches\n", st.scans,
         st.scan_mismatches);
  check(st.scans > 10 && st.scan_mismatches == 0,
        "the tree always picks the minimum-vruntime process");
  setscheduler(SCHED_RR);
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
  test_slices();
  test_trace();
  test_scan();
  setscheduler(orig);

  if(failures == 0)
    printf(1, "schedtest: ALL TESTS PASSED\n");
  else
    printf(1, "schedtest: %d CHECK(S) FAILED\n", failures);
  exit();
}
