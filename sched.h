// Scheduler definitions shared by the kernel and user programs.
// Include after types.h.

#ifndef XV6_SCHED_H
#define XV6_SCHED_H

#define SCHED_RR   0
#define SCHED_FCFS 1   // FIFO: first ready, first run; non-preemptive
#define SCHED_CFS  2

#define NICE_MIN     -20
#define NICE_MAX      19
#define NICE_DEFAULT   0

// Process states as reported in struct pinfo (same values as enum procstate).
#define PSTATE_UNUSED   0
#define PSTATE_EMBRYO   1
#define PSTATE_SLEEPING 2
#define PSTATE_RUNNABLE 3
#define PSTATE_RUNNING  4
#define PSTATE_ZOMBIE   5

// One row of getpinfo(): a snapshot of a process's scheduling state.
// Times are in timer ticks. rtime is measured exactly (CPU cycles, converted
// to ticks); wtime and stime are sampled once per tick.
struct pinfo {
  int pid;
  int ppid;
  int state;
  int nice;
  uint64 vruntime;   // CFS virtual runtime, in weighted CPU cycles
  uint ctime;        // tick the process was created
  uint rtime;        // CPU time used, in ticks
  uint wtime;        // ticks spent RUNNABLE (waiting for a CPU)
  uint stime;        // ticks spent SLEEPING
  char name[16];
};

// Filled in by waitx() for the child it reaps. Times are in ticks.
//   turnaround = etime - ctime
//   response   = first_run - ctime
struct waitinfo {
  int pid;
  uint ctime;        // created
  uint first_run;    // first dispatched onto a CPU
  uint etime;        // exited
  uint rtime;        // CPU time used, in ticks (measured in cycles)
  uint wtime;        // ticks spent RUNNABLE
  uint stime;        // ticks spent SLEEPING
};

// ---- CFS tunables: schedtune(latency, min_granularity), both in ticks ----
// The CFS period is sched_latency, stretched to nr_running * min_granularity
// when too many processes are runnable; each process's slice is its weight's
// share of the period, never less than min_granularity.
#define SCHED_LATENCY_DEFAULT      6
#define SCHED_MIN_GRAN_DEFAULT     1
#define SCHED_LATENCY_MAX       1000

// ---- Scheduler statistics: schedstat(cmd, &st) ----
#define SS_GET       0   // copy counters into *st
#define SS_RESET     1   // zero the counters (then copy)
#define SS_SCAN_ON   2   // CFS: cross-check every pick with a linear scan
#define SS_SCAN_OFF  3

struct schedstat {
  int policy;              // active scheduler
  int ncpu;                // number of CPUs
  uint latency;            // CFS sched_latency (ticks)
  uint min_granularity;    // CFS min_granularity (ticks)
  uint cycles_per_tick;    // measured TSC rate
  int nr_queued;           // processes currently in the run queue
  int cfs_nr_running;      // CFS: queued + running under CFS (for slices)
  uint cfs_load;           // CFS: total weight of those processes
  uint picks;              // processes dispatched
  uint preemptions;        // timer-driven preemptions
  uint enqueues;           // run-queue insertions (wakeups + requeues)
  uint64 pick_cycles;      // total cycles spent choosing the next process
  uint64 enqueue_cycles;   // total cycles spent inserting into the run queue
  int scan_enabled;        // SS_SCAN_ON active
  uint scans;              // linear-scan cross-checks performed
  uint64 scan_cycles;      // total cycles those linear scans took
  uint scan_mismatches;    // times the scan disagreed with the tree (must be 0)
};

// ---- Scheduling trace: tracectl(cmd), traceread(buf, start, n) ----
#define TRACE_START  0   // clear the buffer and start recording
#define TRACE_STOP   1   // stop recording; returns the number of events
#define TRACE_COUNT  2   // number of events recorded so far
#define TRACE_DROPPED 3  // events lost because the buffer was full
#define TRACE_RESTART 4  // if recording: discard events so far, restart clock
#define TRACE_MAX    8192

#define TR_RUN    1      // pid starts running on cpu
#define TR_STOP   2      // pid stops running; arg = TRS_* reason
#define TR_SCHED  3      // scheduler switched; arg = new policy, pid = -1

#define TRS_PREEMPT 0    // preempted (back in the run queue)
#define TRS_SLEEP   1    // blocked
#define TRS_EXIT    2    // exited

struct trace_event {
  uint t;                // time since TRACE_START, in 1/100 tick
  int pid;
  uchar cpu;
  uchar type;            // TR_*
  uchar arg;
  uchar pad;
  char name[16];
};

#endif // XV6_SCHED_H
