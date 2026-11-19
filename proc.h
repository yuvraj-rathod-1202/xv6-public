// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#include "sched.h"   // SCHED_*, NICE_*, struct pinfo, struct waitinfo
#include "rbtree.h"  // struct rb_node

#define NICE_0_LOAD 1024

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // ---- Scheduling (see sched.c, sched_rr.c, sched_cfs.c) ----
  int nice_value;              // NICE_MIN..NICE_MAX; only CFS uses it
  int on_rq;                   // In the active class's run queue (iff RUNNABLE)
  // Statistics, in ticks
  uint ctime;                  // Creation time
  uint etime;                  // Exit time
  uint first_run;              // Tick of first dispatch (valid if has_run)
  int has_run;                 // Has been dispatched at least once
  uint wtime;                  // Ticks spent RUNNABLE (sampled)
  uint stime;                  // Ticks spent SLEEPING (sampled)
  uint64 exec_time;            // Total CPU cycles spent RUNNING
  uint64 run_start;            // TSC value when the current run started
  // RR / FIFO run queue links
  struct proc *rq_next;
  struct proc *rq_prev;
  // CFS
  struct rb_node rb;           // Node in the CFS red-black tree
  uint64 vruntime;             // Virtual runtime (weighted CPU cycles)
  int cfs_accounting;          // Current run was dispatched by CFS (charge it)
  uint cfs_load;               // Weight counted in the CFS load sums
  uint slice_ticks;            // Ticks used in the current CFS run
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
