// Completely Fair Scheduler class.
//
// RUNNABLE processes sit in a red-black tree ordered by (vruntime, pid).
// The next process to run is the leftmost node: the one that has received
// the least weighted CPU time. vruntime advances by
//     cycles_run * NICE_0_LOAD / weight(nice)
// so a process's long-run CPU share is proportional to its weight.
//
// Time slices (sched_latency, min_granularity):
//   period = sched_latency                       if nr_running <= latency/min_gran
//          = nr_running * min_granularity        otherwise
//   slice(p) = max(period * weight(p) / total_weight, min_granularity)
// where nr_running and total_weight count the queued processes plus those
// currently running under CFS. A running process is preempted at the first
// tick on which it has used its slice (and only if someone is waiting).
// All of this is in ticks: the timer interrupt is the only preemption point.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "rbtree.h"
#include "sched_class.h"

static struct {
  struct rb_root tree;
  uint64 min_vruntime;   // monotonic floor for placing woken processes
  uint queued_load;      // sum of weights of processes in the tree
  int queued_nr;
  uint running_load;     // sum of weights of processes running under CFS
  int running_nr;
} cfs;

// Linux sched_prio_to_weight: nice 0 = 1024, each step is ~1.25x.
static const uint weights[NICE_MAX - NICE_MIN + 1] = {
  88761, 71755, 56483, 46273, 36291,
  29154, 23254, 18705, 14949, 11916,
   9548,  7620,  6100,  4904,  3906,
   3121,  2501,  1991,  1586,  1277,
   1024,   820,   655,   526,   423,
    335,   272,   215,   172,   137,
    110,    87,    70,    56,    45,
     36,    29,    23,    18,    15
};

static uint
cfs_weight(int nice)
{
  if(nice < NICE_MIN)
    nice = NICE_MIN;
  if(nice > NICE_MAX)
    nice = NICE_MAX;
  return weights[nice - NICE_MIN];
}

// vruntime += cycles * NICE_0_LOAD / weight, without a 64-bit division (the
// kernel has no libgcc): the per-cycle factor is computed once in 32 bits
// with CFS_SHIFT fraction bits. At nice 0 one cycle adds exactly 1.
#define CFS_SHIFT 16

static uint64
cfs_charge(uint64 cycles, uint weight)
{
  uint factor = (NICE_0_LOAD << CFS_SHIFT) / weight;
  return (cycles * factor) >> CFS_SHIFT;
}

static int
cfs_less(struct rb_node *a, struct rb_node *b)
{
  struct proc *pa = rb_entry(a, struct proc, rb);
  struct proc *pb = rb_entry(b, struct proc, rb);

  if(pa->vruntime != pb->vruntime)
    return pa->vruntime < pb->vruntime;
  return pa->pid < pb->pid;
}

static void
update_min_vruntime(struct proc *curr)
{
  struct rb_node *left = rb_first(&cfs.tree);
  uint64 v;
  int have = 0;

  if(curr){
    v = curr->vruntime;
    have = 1;
  }
  if(left){
    struct proc *lp = rb_entry(left, struct proc, rb);
    if(!have || lp->vruntime < v)
      v = lp->vruntime;
    have = 1;
  }
  if(have && v > cfs.min_vruntime)
    cfs.min_vruntime = v;
}

static void
cfs_enqueue(struct proc *p, int wakeup)
{
  // A process returning from sleep (or new, or migrated in) has not been
  // charged for a while; lift it to min_vruntime so it cannot use a stale,
  // small vruntime to monopolise the CPU.
  if(wakeup && p->vruntime < cfs.min_vruntime)
    p->vruntime = cfs.min_vruntime;
  p->cfs_load = cfs_weight(p->nice_value);
  rb_insert(&cfs.tree, &p->rb, cfs_less);
  cfs.queued_load += p->cfs_load;
  cfs.queued_nr++;
}

static void
cfs_dequeue(struct proc *p)
{
  rb_erase(&cfs.tree, &p->rb);
  cfs.queued_load -= p->cfs_load;
  cfs.queued_nr--;
}

// Cross-check: find the minimum (vruntime, pid) RUNNABLE process the slow
// way and compare with the tree's answer. Used by the overhead benchmark
// (as the O(n) baseline) and by SCHED_DEBUG builds (as a correctness check).
static void
scan_check(struct proc *tree_pick)
{
  struct proc *p, *best = 0, *table = proc_table();
  uint64 t0 = sched_clock();

  for(p = table; p < &table[NPROC]; p++){
    if(p->state != RUNNABLE)
      continue;
    if(best == 0 || p->vruntime < best->vruntime ||
       (p->vruntime == best->vruntime && p->pid < best->pid))
      best = p;
  }
  sched_scan_result(sched_clock() - t0, best != tree_pick);
}

static struct proc*
cfs_pick_next(void)
{
  struct rb_node *n = rb_first(&cfs.tree);
  struct proc *p;

  if(n == 0)
    return 0;
  p = rb_entry(n, struct proc, rb);
  if(sched_scan_enabled)
    scan_check(p);
  cfs_dequeue(p);
  return p;
}

static void
cfs_set_curr(struct proc *p)
{
  p->cfs_accounting = 1;
  p->slice_ticks = 0;
  p->cfs_load = cfs_weight(p->nice_value);
  cfs.running_load += p->cfs_load;
  cfs.running_nr++;
}

static void
cfs_put_prev(struct proc *p, uint64 cycles)
{
  // A process dispatched by another class before a switch into CFS is not
  // charged: switched_to() gave everyone a fresh, equal start.
  if(p->cfs_accounting){
    p->vruntime += cfs_charge(cycles, cfs_weight(p->nice_value));
    cfs.running_load -= p->cfs_load;
    cfs.running_nr--;
    p->cfs_accounting = 0;
  }
  update_min_vruntime(p->state == RUNNABLE ? p : 0);
}

// This process's slice of the current period, in ticks.
static uint
cfs_slice(struct proc *p)
{
  uint nr = cfs.queued_nr + cfs.running_nr;
  uint total = cfs.queued_load + cfs.running_load;
  uint period = sched_latency;
  uint slice;

  if(nr * sched_min_granularity > period)
    period = nr * sched_min_granularity;
  if(total == 0)
    return period;
  // period * weight can exceed 32 bits; total fits in 32.
  slice = (uint)div64_32((uint64)period * p->cfs_load + total / 2, total);
  if(slice < sched_min_granularity)
    slice = sched_min_granularity;
  return slice;
}

static int
cfs_tick(struct proc *p)
{
  // Still running from before a switch into CFS: hand over at once.
  if(!p->cfs_accounting)
    return 1;
  p->slice_ticks++;
  if(cfs.queued_nr == 0)
    return 0;            // nobody is waiting; keep running
  return p->slice_ticks >= cfs_slice(p);
}

// Becoming the active scheduler. vruntime did not advance under RR/FIFO,
// so values left from an earlier CFS period are stale relative to each
// other (a process that slept through it would come back far behind and
// hog the CPU). Start every process from zero. The tree is empty here: the
// core moves queued processes in only after this returns.
static void
cfs_switched_to(void)
{
  struct proc *p, *table = proc_table();

  for(p = table; p < &table[NPROC]; p++){
    p->vruntime = 0;
    p->cfs_accounting = 0;
  }
  cfs.min_vruntime = 0;
  cfs.running_load = 0;
  cfs.running_nr = 0;
}

static int
cfs_check(void)
{
  int n = rb_verify(&cfs.tree, cfs_less);
  struct rb_node *x;
  uint load = 0;

  if(n < 0){
    cprintf("cfs tree: %s\n", rb_strerror(n));
    return -1;
  }
  if(n != cfs.queued_nr){
    cprintf("cfs tree: %d nodes but queued_nr %d\n", n, cfs.queued_nr);
    return -1;
  }
  for(x = rb_first(&cfs.tree); x; x = rb_next(x)){
    struct proc *p = rb_entry(x, struct proc, rb);
    if(p->state != RUNNABLE){
      cprintf("cfs tree: pid %d in tree but state %d\n", p->pid, p->state);
      return -1;
    }
    load += p->cfs_load;
  }
  if(load != cfs.queued_load){
    cprintf("cfs tree: load %d but queued_load %d\n", load, cfs.queued_load);
    return -1;
  }
  // The running totals used for time slices must match the processes that
  // are actually running under CFS accounting.
  {
    struct proc *p, *table = proc_table();
    int rn = 0;
    uint rl = 0;
    for(p = table; p < &table[NPROC]; p++)
      if(p->state == RUNNING && p->cfs_accounting){
        rn++;
        rl += p->cfs_load;
      }
    if(rn != cfs.running_nr || rl != cfs.running_load){
      cprintf("cfs: %d running (load %d) but running_nr %d (load %d)\n",
              rn, rl, cfs.running_nr, cfs.running_load);
      return -1;
    }
  }
  return n;
}

// For schedstat: current CFS run-queue totals.
void
cfs_load_info(int *nr, uint *load)
{
  *nr = cfs.queued_nr + cfs.running_nr;
  *load = cfs.queued_load + cfs.running_load;
}

struct sched_class cfs_sched_class = {
  .name = "cfs",
  .enqueue = cfs_enqueue,
  .dequeue = cfs_dequeue,
  .pick_next = cfs_pick_next,
  .set_curr = cfs_set_curr,
  .put_prev = cfs_put_prev,
  .tick = cfs_tick,
  .switched_to = cfs_switched_to,
  .check = cfs_check,
};
