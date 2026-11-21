// Scheduler core: policy-independent glue between proc.c and the scheduler
// classes (sched_rr.c, sched_cfs.c).
//
//   - the class table and runtime switching (with run-queue migration)
//   - dispatch / stop bookkeeping: CPU-time accounting in TSC cycles
//   - timer-tick preemption decisions
//   - statistics (schedstat), the scheduling trace (tracectl/traceread),
//     CFS tunables (schedtune)
//   - SCHED_DEBUG invariant checking
//
// Everything here is called with ptable.lock held, except the sys_*-facing
// wrappers in proc.c, which take the lock first.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "sched_class.h"

static struct sched_class *classes[] = {
  [SCHED_RR]   &rr_sched_class,
  [SCHED_FCFS] &fifo_sched_class,
  [SCHED_CFS]  &cfs_sched_class,
};

static int policy = SCHED_RR;
#define CUR (classes[policy])

uint sched_latency = SCHED_LATENCY_DEFAULT;
uint sched_min_granularity = SCHED_MIN_GRAN_DEFAULT;
int sched_scan_enabled;

static int nr_queued;           // processes with on_rq set
static struct schedstat stats;

// ---- clock ----

// CPU time is measured with the time-stamp counter, not timer ticks: with
// ticks, a process that blocks before the next tick would be charged
// nothing. A run starts and stops on the same CPU, so per-CPU TSC offsets
// cancel out.
uint64
sched_clock(void)
{
  return rdtsc();
}

static uint cycles_per_tick;    // measured on CPU 0, smoothed

// 64-by-32-bit division with the x86 divl instruction (the kernel has no
// libgcc). The high word is divided first so divl's quotient fits.
uint64
div64_32(uint64 n, uint d)
{
  uint hi = n >> 32, lo = (uint)n;
  uint qhi = hi / d, r = hi % d, qlo;

  asm("divl %4" : "=a" (qlo), "=d" (r) : "0" (lo), "1" (r), "rm" (d));
  return ((uint64)qhi << 32) | qlo;
}

uint
sched_cycles_to_ticks(uint64 cycles)
{
  if(cycles_per_tick == 0)
    return 0;
  return (uint)div64_32(cycles + cycles_per_tick / 2, cycles_per_tick);
}

// Called by CPU 0 on every timer tick.
void
sched_clock_tick(void)
{
  static uint64 last;
  uint64 now = sched_clock();

  if(last){
    uint d = (uint)(now - last);
    // Smooth over ~8 ticks so one late interrupt does not skew the rate.
    if(cycles_per_tick == 0)
      cycles_per_tick = d;
    else
      cycles_per_tick = cycles_per_tick - cycles_per_tick / 8 + d / 8;
  }
  last = now;
}

// ---- trace ----

static struct trace_event trace_buf[TRACE_MAX];
static int trace_on, trace_n, trace_dropped;
static uint64 trace_t0;
// Cycles-per-tick rate frozen at TRACE_START. The live estimate changes on
// every tick; converting with it could make a later event on the same CPU
// get a smaller timestamp. With one fixed rate, a CPU's timestamps follow
// its cycle counter and never go backwards.
static uint trace_cpt;

static void
trace(int type, struct proc *p, int arg)
{
  struct trace_event *e;
  uint64 now;

  if(!trace_on)
    return;
  if(trace_n >= TRACE_MAX){
    trace_dropped++;
    return;
  }
  e = &trace_buf[trace_n++];
  // Time in 1/100 tick since TRACE_START. Another CPU's counter can be a
  // little behind the one that started the trace: clamp that to 0 rather
  // than let the unsigned subtraction wrap around.
  now = sched_clock();
  e->t = (trace_cpt && now > trace_t0) ?
    (uint)div64_32((now - trace_t0) * 100, trace_cpt) : 0;
  e->cpu = cpuid();
  e->type = type;
  e->arg = arg;
  e->pad = 0;
  if(p){
    e->pid = p->pid;
    safestrcpy(e->name, p->name, sizeof(e->name));
  } else {
    e->pid = -1;
    e->name[0] = 0;
  }
}

int
sched_tracectl(int cmd)
{
  switch(cmd){
  case TRACE_START:
    trace_n = 0;
    trace_dropped = 0;
    trace_t0 = sched_clock();
    trace_cpt = cycles_per_tick;
    trace_on = 1;
    trace(TR_SCHED, 0, policy);   // record the policy in force at t = 0
    return 0;
  case TRACE_STOP:
    trace_on = 0;
    return trace_n;
  case TRACE_COUNT:
    return trace_n;
  case TRACE_DROPPED:
    return trace_dropped;
  case TRACE_RESTART:
    // Discard what has been recorded so far, if tracing is on (lets a
    // traced program skip its own set-up phase).
    if(!trace_on)
      return -1;
    return sched_tracectl(TRACE_START);
  }
  return -1;
}

int
sched_traceread(struct trace_event *dst, int start, int n)
{
  int i;

  if(start < 0 || n < 0)
    return -1;
  for(i = 0; i < n && start + i < trace_n; i++)
    dst[i] = trace_buf[start + i];
  return i;
}

// ---- debug invariants ----

#ifdef SCHED_DEBUG
// Verify the run-queue invariant: a process is queued iff it is RUNNABLE,
// the counts agree, and the active class's structure is intact.
static void
sched_check(char *where)
{
  struct proc *p, *table = proc_table();
  int n = 0, cn;

  for(p = table; p < &table[NPROC]; p++){
    if(p->on_rq != (p->state == RUNNABLE)){
      cprintf("sched_check(%s): pid %d state %d on_rq %d\n",
              where, p->pid, p->state, p->on_rq);
      panic("sched invariant: on_rq vs state");
    }
    n += p->on_rq;
  }
  if(n != nr_queued){
    cprintf("sched_check(%s): %d queued, nr_queued %d\n", where, n, nr_queued);
    panic("sched invariant: nr_queued");
  }
  cn = CUR->check();
  if(cn != n){
    cprintf("sched_check(%s): class %s holds %d, expected %d\n",
            where, CUR->name, cn, n);
    panic("sched invariant: class queue");
  }
}
#define CHECK(where) sched_check(where)
#else
#define CHECK(where)
#endif

// ---- run queue ----

static void
do_enqueue(struct proc *p, int wakeup)
{
  uint64 t0 = sched_clock();

  if(p->on_rq)
    panic("sched: enqueue twice");
  CUR->enqueue(p, wakeup);
  p->on_rq = 1;
  nr_queued++;
  stats.enqueues++;
  stats.enqueue_cycles += sched_clock() - t0;
}

// p has just become RUNNABLE (new, woken, or killed while asleep).
void
sched_wakeup(struct proc *p)
{
  if(p->state != RUNNABLE)
    panic("sched_wakeup: not runnable");
  do_enqueue(p, 1);
  CHECK("wakeup");
}

static uint64 scan_cycles_now;   // linear-scan time inside the current pick

// Choose the next process to run on this CPU and take it off the run queue.
struct proc*
sched_pick(void)
{
  uint64 t0, t1;
  struct proc *p;

  scan_cycles_now = 0;
  t0 = sched_clock();
  p = CUR->pick_next();
  t1 = sched_clock();
  if(p){
    p->on_rq = 0;
    nr_queued--;
    stats.picks++;
    // Exclude the optional linear-scan cross-check from the pick cost.
    stats.pick_cycles += (t1 - t0) - scan_cycles_now;
  }
  // (Invariants are checked in sched_start: until the caller marks p
  // RUNNING, p is RUNNABLE but legitimately off the queue.)
  return p;
}

// p (now RUNNING) is about to be switched to on this CPU.
void
sched_start(struct proc *p)
{
  p->run_start = sched_clock();
  CUR->set_curr(p);
  trace(TR_RUN, p, 0);
  CHECK("start");
}

// p is giving up the CPU; p->state is RUNNABLE, SLEEPING or ZOMBIE.
void
sched_stop(struct proc *p)
{
  uint64 cycles = sched_clock() - p->run_start;

  p->exec_time += cycles;
  trace(TR_STOP, p, p->state == RUNNABLE ? TRS_PREEMPT :
                    p->state == ZOMBIE ? TRS_EXIT : TRS_SLEEP);
  CUR->put_prev(p, cycles);
  if(p->state == RUNNABLE)
    do_enqueue(p, 0);
  CHECK("stop");
}

// Timer tick while p runs on this CPU: should it be preempted?
int
sched_tick(struct proc *p)
{
  int preempt = CUR->tick(p);

  if(preempt)
    stats.preemptions++;
  return preempt;
}

// Switch the active policy. Every queued process is moved from the old
// class's run queue to the new one (in the old class's pick order), so no
// process is lost or duplicated. Running processes finish their current
// run and are then handed to the new class by sched_stop().
int
sched_set_policy(int s)
{
  struct sched_class *old;
  struct proc *p;
  int scan = sched_scan_enabled;

  if(s < SCHED_RR || s > SCHED_CFS)
    return -1;
  if(s == policy)
    return 0;
  old = CUR;
  policy = s;
  CUR->switched_to();
  // Processes already moved are RUNNABLE but no longer in the old queue,
  // so the CFS linear-scan cross-check would not apply while draining.
  sched_scan_enabled = 0;
  while((p = old->pick_next()) != 0){
    p->on_rq = 0;
    nr_queued--;
    do_enqueue(p, 1);
  }
  sched_scan_enabled = scan;
  trace(TR_SCHED, 0, s);
  CHECK("switch");
  return 0;
}

int
sched_get_policy(void)
{
  return policy;
}

// ---- tunables & statistics ----

int
sched_tune(int latency, int min_gran)
{
  uint lat = latency > 0 ? latency : sched_latency;
  uint gran = min_gran > 0 ? min_gran : sched_min_granularity;

  if(latency < 0 || min_gran < 0 || lat > SCHED_LATENCY_MAX || gran > lat)
    return -1;
  sched_latency = lat;
  sched_min_granularity = gran;
  return 0;
}

void
sched_scan_result(uint64 cycles, int mismatch)
{
  stats.scans++;
  stats.scan_cycles += cycles;
  scan_cycles_now += cycles;
  if(mismatch){
    stats.scan_mismatches++;
#ifdef SCHED_DEBUG
    panic("cfs: tree pick differs from linear scan");
#endif
  }
}

int
sched_stat(int cmd, struct schedstat *st)
{
  switch(cmd){
  case SS_GET:
    break;
  case SS_RESET:
    memset(&stats, 0, sizeof(stats));
    break;
  case SS_SCAN_ON:
    sched_scan_enabled = 1;
    break;
  case SS_SCAN_OFF:
#ifndef SCHED_DEBUG
    sched_scan_enabled = 0;
#endif
    break;
  default:
    return -1;
  }
  stats.policy = policy;
  stats.ncpu = ncpu;
  stats.latency = sched_latency;
  stats.min_granularity = sched_min_granularity;
  stats.cycles_per_tick = cycles_per_tick;
  stats.nr_queued = nr_queued;
  cfs_load_info(&stats.cfs_nr_running, &stats.cfs_load);
  stats.scan_enabled = sched_scan_enabled;
  if(st)
    *st = stats;
  return 0;
}

void
sched_init(void)
{
#ifdef SCHED_DEBUG
  sched_scan_enabled = 1;   // debug kernels cross-check every CFS pick
  cprintf("sched: SCHED_DEBUG build, invariant checking enabled\n");
#endif
}
