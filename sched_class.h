// Kernel-internal scheduler-class interface (modelled on Linux's
// struct sched_class). Include after types.h, param.h, proc.h.
//
// Each policy (RR, FIFO, CFS) owns its run queue and implements these
// hooks. The core (sched.c) never looks inside a run queue; it only calls
// the active class's hooks, so adding a policy means writing one file and
// adding one line to the class table.
//
// Locking: every hook is called with ptable.lock held.
//
// Run-queue invariant (checked by sched_check() in SCHED_DEBUG builds):
//   p->on_rq == 1  <=>  p->state == RUNNABLE  <=>  p is in the active
//   class's run queue.

struct sched_class {
  char *name;
  // p has just become RUNNABLE. wakeup != 0 when it comes from sleep, fork,
  // kill or a scheduler switch (CFS places it at min_vruntime); 0 when it
  // was preempted while running.
  void (*enqueue)(struct proc *p, int wakeup);
  // Remove the queued process p from the run queue.
  void (*dequeue)(struct proc *p);
  // Remove and return the process that should run next, or 0 if empty.
  struct proc* (*pick_next)(void);
  // p was just dispatched (state RUNNING) by this class.
  void (*set_curr)(struct proc *p);
  // p stopped running after `cycles` CPU cycles; p->state already says why
  // (RUNNABLE = preempted, SLEEPING, ZOMBIE). Called before any requeue.
  // Must cope with a p that a different class dispatched (scheduler switch).
  void (*put_prev)(struct proc *p, uint64 cycles);
  // Timer tick while p is running on this CPU. Return 1 to preempt p.
  int (*tick)(struct proc *p);
  // Called when this class becomes the active one, before any process is
  // moved into it.
  void (*switched_to)(void);
  // SCHED_DEBUG: verify the run queue's internal structure. Returns the
  // number of queued processes, or -1 after printing what is wrong.
  int (*check)(void);
};

extern struct sched_class rr_sched_class;
extern struct sched_class fifo_sched_class;
extern struct sched_class cfs_sched_class;

// Shared helpers provided by sched.c.
uint64          div64_32(uint64 n, uint d);
uint64          sched_clock(void);
struct proc*    proc_table(void);       // &ptable.proc[0] (proc.c)
extern uint     sched_latency;          // CFS tunables, in ticks
extern uint     sched_min_granularity;
extern int      sched_scan_enabled;     // cross-check CFS picks
void            sched_scan_result(uint64 cycles, int mismatch);
void            cfs_load_info(int *nr, uint *load);
