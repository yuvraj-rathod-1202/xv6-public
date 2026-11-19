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

#endif // XV6_SCHED_H
